"""
main.py — Ponto de entrada do EdgeBench Backend.

Responsabilidades:
    1. Configurar logging estruturado.
    2. Aguardar conectividade com o PostgreSQL (retry com backoff).
    3. Inicializar o schema do banco (create_all idempotente).
    4. Iniciar o listener MQTT em thread background.
    5. Expor `export_report()` para geração de planilha sob demanda.
    6. Manter o loop principal vivo com tratamento de SIGINT/SIGTERM.

Uso:
    python main.py                      # Inicia o backend completo
    python main.py --export             # Gera relatório e encerra
    python main.py --export --bancada BC-01 --start 2026-09-01 --end 2026-09-14
"""

import argparse
import logging
import signal
import sys
import time
from typing import Optional

DB_RETRY_MAX_WAIT = 30   # segundos máximos entre tentativas
DB_RETRY_TIMEOUT  = 300  # tempo total máximo de espera (5 minutos)

from config import app_config, db_config, mqtt_config
from database import check_connection, init_db
from mqtt_listener import MQTTListener
from scheduler import start_scheduler

# ─────────────────────────────────────────────────────────────────────────────
# Logging estruturado
# ─────────────────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=getattr(logging, app_config.log_level.upper(), logging.INFO),
    format="%(asctime)s [%(levelname)-8s] %(name)s — %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ],
)
logger = logging.getLogger("edgebench.main")


# ─────────────────────────────────────────────────────────────────────────────
# Estado global da aplicação
# ─────────────────────────────────────────────────────────────────────────────

_listener: Optional[MQTTListener] = None
_scheduler = None
_running: bool = False


# ─────────────────────────────────────────────────────────────────────────────
# Gerenciamento de sinais do SO
# ─────────────────────────────────────────────────────────────────────────────

def _handle_shutdown(signum: int, frame) -> None:
    """Handler para SIGINT e SIGTERM — encerra o backend graciosamente."""
    global _running
    sig_name = signal.Signals(signum).name
    logger.info("Sinal %s recebido — iniciando shutdown gracioso…", sig_name)
    _running = False


signal.signal(signal.SIGINT,  _handle_shutdown)
signal.signal(signal.SIGTERM, _handle_shutdown)


# ─────────────────────────────────────────────────────────────────────────────
# Startup
# ─────────────────────────────────────────────────────────────────────────────

def _wait_for_postgres() -> None:
    """
    Aguarda o PostgreSQL ficar disponível com backoff exponencial.
    Evita que o container saia em loop durante o startup do Docker Compose.

    Raises:
        SystemExit: Se o banco não ficar acessível dentro de DB_RETRY_TIMEOUT segundos.
    """
    attempt = 0
    elapsed = 0.0
    wait = 1  # segundos iniciais

    logger.info(
        "Aguardando PostgreSQL em %s:%d (timeout=%ds)…",
        db_config.host,
        db_config.port,
        DB_RETRY_TIMEOUT,
    )

    while elapsed < DB_RETRY_TIMEOUT:
        if check_connection():
            logger.info("PostgreSQL disponível após %.0fs (tentativa %d). ✓", elapsed, attempt + 1)
            return

        attempt += 1
        logger.warning(
            "PostgreSQL indisponível — tentativa %d | próxima em %ds (%.0fs decorridos)…",
            attempt,
            wait,
            elapsed,
        )
        time.sleep(wait)
        elapsed += wait
        wait = min(wait * 2, DB_RETRY_MAX_WAIT)  # backoff exponencial com teto

    logger.critical(
        "PostgreSQL não ficou disponível em %ds. Abortando.",
        DB_RETRY_TIMEOUT,
    )
    sys.exit(1)


def startup() -> MQTTListener:
    """
    Executa a sequência de inicialização do backend.

    Returns:
        Instância do MQTTListener já iniciada.

    Raises:
        SystemExit: Se o banco de dados não ficar acessível dentro do timeout.
    """
    logger.info("=" * 60)
    logger.info("  EdgeBench Backend — Iniciando…")
    logger.info("=" * 60)
    logger.info("Banco de dados : %s:%d/%s", db_config.host, db_config.port, db_config.name)
    logger.info("Broker MQTT    : %s:%d", mqtt_config.host, mqtt_config.port)
    logger.info("Tópico MQTT    : %s (QoS %d)", mqtt_config.topic_filter, mqtt_config.qos)

    # ── Aguarda PostgreSQL com retry + backoff ────────────────────────────────
    _wait_for_postgres()

    # ── Inicializa o schema ───────────────────────────────────────────────────
    init_db()

    # ── Inicia o listener MQTT ────────────────────────────────────────────────
    listener = MQTTListener()
    listener.start()

    # ── Inicia o agendador de backups ─────────────────────────────────────────
    global _scheduler
    _scheduler = start_scheduler()

    logger.info("EdgeBench Backend operacional. Aguardando mensagens MQTT…")
    logger.info("Pressione Ctrl+C ou envie SIGTERM para encerrar.")
    return listener


# ─────────────────────────────────────────────────────────────────────────────
# Exportação de relatório sob demanda
# ─────────────────────────────────────────────────────────────────────────────

def export_report(
    bancada_id: Optional[str] = None,
    start_dt: Optional[str] = None,
    end_dt: Optional[str] = None,
    output_path: Optional[str] = None,
) -> str:
    """
    Gera o relatório Excel sob demanda.

    Args:
        bancada_id  : Filtra por bancada específica (ex.: "BC-01"). None = todas.
        start_dt    : Data/hora inicial ISO 8601 (ex.: "2026-09-01T00:00:00").
        end_dt      : Data/hora final ISO 8601 (ex.: "2026-09-14T23:59:59").
        output_path : Caminho personalizado para o arquivo `.xlsx`. Opcional.

    Returns:
        Caminho absoluto do arquivo gerado.
    """
    from analytics import full_report
    from excel_generator import generate_excel_report

    logger.info(
        "Gerando relatório… bancada=%s | início=%s | fim=%s",
        bancada_id or "todas",
        start_dt or "sem filtro",
        end_dt or "sem filtro",
    )

    data = full_report(bancada_id=bancada_id, start_dt=start_dt, end_dt=end_dt)
    path = generate_excel_report(report_data=data, output_path=output_path)

    logger.info("Relatório exportado com sucesso → %s", path)
    return path


# ─────────────────────────────────────────────────────────────────────────────
# Loop principal
# ─────────────────────────────────────────────────────────────────────────────

def run() -> None:
    """Loop principal que mantém o backend ativo."""
    global _listener, _running

    _listener = startup()
    _running = True

    try:
        while _running:
            time.sleep(1)
    finally:
        if _scheduler:
            _scheduler.shutdown(wait=False)
            logger.info("Scheduler encerrado.")
        if _listener:
            _listener.stop()
        logger.info("EdgeBench Backend encerrado. Até logo!")


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="EdgeBench Backend — Ingestão MQTT + PostgreSQL + Relatórios Excel",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--export",
        action="store_true",
        help="Gera o relatório Excel e encerra sem iniciar o listener MQTT.",
    )
    parser.add_argument(
        "--bancada",
        type=str,
        default=None,
        metavar="ID",
        help='Filtra o relatório por bancada (ex.: "BC-01").',
    )
    parser.add_argument(
        "--start",
        type=str,
        default=None,
        metavar="DATETIME",
        help="Data/hora inicial do relatório (ISO 8601).",
    )
    parser.add_argument(
        "--end",
        type=str,
        default=None,
        metavar="DATETIME",
        help="Data/hora final do relatório (ISO 8601).",
    )
    parser.add_argument(
        "--output",
        type=str,
        default=None,
        metavar="PATH",
        help="Caminho de saída do arquivo .xlsx.",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = _parse_args()

    if args.export:
        # Modo exportação standalone (não inicia MQTT, só gera planilha)
        logger.info("Modo exportação ativado.")
        _wait_for_postgres()
        init_db()
        path = export_report(
            bancada_id=args.bancada,
            start_dt=args.start,
            end_dt=args.end,
            output_path=args.output,
        )
        print(f"\nRelatório gerado: {path}")
        sys.exit(0)
    else:
        # Modo normal: ingestão MQTT contínua
        run()
