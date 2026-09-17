"""
Ponto de entrada do EdgeBench Backend.
Aqui iniciamos o banco de dados, o MQTT e o agendador de tarefas.
"""

import argparse
import logging
import signal
import sys
import time
from typing import Optional

DB_RETRY_MAX_WAIT = 30   # segundos máximos entre tentativas
DB_RETRY_TIMEOUT  = 300  # tempo total máximo de espera (5 minutos)

from settings.config import app_config, db_config, mqtt_config
from settings.database import check_connection, init_db
from hardware_comunication.mqtt_listener import MQTTListener
from services.scheduler import start_scheduler

# Configuração de Logs

logging.basicConfig(
    level=getattr(logging, app_config.log_level.upper(), logging.INFO),
    format="%(asctime)s [%(levelname)-8s] %(name)s — %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ],
)
logger = logging.getLogger("edgebench.main")


# Variáveis Globais

_listener: Optional[MQTTListener] = None
_scheduler = None
_running: bool = False


# Tratamento de Sinais (Encerramento)

def _handle_shutdown(signum: int, frame) -> None:
    """Encerra a aplicação de forma segura quando recebe Ctrl+C."""
    global _running
    sig_name = signal.Signals(signum).name
    logger.info("Sinal %s recebido — iniciando shutdown gracioso…", sig_name)
    _running = False


signal.signal(signal.SIGINT,  _handle_shutdown)
signal.signal(signal.SIGTERM, _handle_shutdown)


# Inicialização (Startup)

def _wait_for_postgres() -> None:
    """Garante que o PostgreSQL subiu antes de continuarmos (útil no Docker)."""
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
    """Prepara tudo: banco, MQTT e tarefas agendadas."""
    logger.info("=" * 60)
    logger.info("  EdgeBench Backend — Iniciando…")
    logger.info("=" * 60)
    logger.info("Banco de dados : %s:%d/%s", db_config.host, db_config.port, db_config.name)
    logger.info("Broker MQTT    : %s:%d", mqtt_config.host, mqtt_config.port)
    logger.info("Tópico MQTT    : %s (QoS %d)", mqtt_config.topic_filter, mqtt_config.qos)

    # Espera o banco ficar pronto
    _wait_for_postgres()

    # Cria as tabelas necessárias
    init_db()

    # Começa a ouvir mensagens do MQTT
    listener = MQTTListener()
    listener.start()

    # Liga as rotinas de backup e sincronização
    global _scheduler
    _scheduler = start_scheduler()

    logger.info("EdgeBench Backend operacional. Aguardando mensagens MQTT…")
    logger.info("Pressione Ctrl+C ou envie SIGTERM para encerrar.")
    return listener


# Geração de Relatórios

def export_report(
    bancada_id: Optional[str] = None,
    start_dt: Optional[str] = None,
    end_dt: Optional[str] = None,
    output_path: Optional[str] = None,
) -> str:
    """Cria a planilha Excel sob demanda filtrando os dados se necessário."""
    from services.analytics import full_report
    from services.excel_generator import generate_excel_report

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


# Execução Principal

def run() -> None:
    """Inicia os serviços e fica rodando infinitamente."""
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


# Comandos do Terminal

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
    parser.add_argument(
        "--upload-drive",
        action="store_true",
        help="Realiza o upload automático do relatório gerado para o Google Drive / Sheets.",
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

        if args.upload_drive:
            from external_integrations.google_sheets_sync import upload_to_sheets
            print("\nEnviando relatório para o Google Drive / Sheets...")
            bancada_tag = args.bancada or "Consolidado"
            sheet_title = f"EdgeBench_{bancada_tag}_{time.strftime('%Y-%m-%d_%H%M%S')}"
            result = upload_to_sheets(
                excel_filepath=path,
                sheet_name=sheet_title,
                convert_to_sheets=True,
                update_if_exists=False,
            )
            if result and result.get("web_view_link"):
                print(f"Planilha criada com sucesso no Google Sheets!")
                print(f"Link de acesso: {result['web_view_link']}")
            else:
                print("[AVISO] Falha ao enviar para o Google Drive. Verifique o arquivo token.json.")

        sys.exit(0)
    else:
        # Modo normal: ingestão MQTT contínua
        run()
