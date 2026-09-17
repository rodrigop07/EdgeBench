"""
Ouvinte MQTT (Recebe dados das bancadas).
Fica rodando em segundo plano ouvindo as mensagens das placas e salva no banco de dados.
"""

import json
import logging
import os
import re
import sys
import time
from datetime import datetime, timezone
from typing import Any

import paho.mqtt.client as mqtt
from sqlalchemy.exc import IntegrityError

APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from settings.config import mqtt_config
from settings.database import get_session
from settings.models import TelemetriaBancada

logger = logging.getLogger(__name__)


# Funções de Limpeza de Dados

def _extract_bancada_id(data: dict[str, Any], topic: str) -> str:
    """Descobre qual é o ID da bancada que mandou a mensagem."""
    # 1. Prioridade: Campo 'bancada_id' explícito no JSON
    if "bancada_id" in data and data["bancada_id"]:
        return str(data["bancada_id"]).strip()

    # 2. Segunda opção: Campo numérico 'bancada' no JSON (ex: LWT -> {"bancada": 1})
    if "bancada" in data:
        try:
            return f"BC-{int(data['bancada']):02d}"
        except (ValueError, TypeError):
            return str(data["bancada"]).strip()

    # 3. Fallback: Extrai do nome do tópico (fabrica/bancada_X/...)
    match = re.search(r"bancada_(\d+)", topic)
    if match:
        return f"BC-{int(match.group(1)):02d}"

    # Fallback genérico caso a regex não encontre o número
    parts = topic.split("/")
    if len(parts) >= 2:
        return parts[1]

    return "BC-UNKNOWN"


# Leitura das Mensagens

def _parse_production_payload(raw: bytes, topic: str) -> dict[str, Any] | None:
    """Lê e entende a mensagem de produção da bancada."""
    try:
        data = json.loads(raw.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        logger.warning("Payload de produção inválido (não é JSON): %s | raw=%r", exc, raw[:200])
        return None

    bancada_id = _extract_bancada_id(data, topic)

    # Converte a hora recebida
    ts_raw = data.get("timestamp") or data.get("timestamp_esp")
    if ts_raw is None:
        logger.warning("Payload de produção incompleto (sem timestamp) para %s | data=%s", bancada_id, data)
        return None

    if isinstance(ts_raw, (int, float)):
        timestamp_esp = datetime.fromtimestamp(ts_raw, tz=timezone.utc).isoformat()
    else:
        timestamp_esp = str(ts_raw)

    # Trata quantidade/delta de peças e contagem acumulada
    delta_pecas = int(data.get("quantidade", data.get("delta_pecas", 1)))
    contagem_total = int(data.get("contagem", data.get("contagem_total", delta_pecas)))

    # Verifica se a placa estava offline quando gravou
    modo_offline = data.get("modo_offline", False)
    if modo_offline:
        status_raw = "OFFLINE_REPLAY"
    else:
        status_raw = str(data.get("status", "OPERANDO")).upper().strip()

    return {
        "bancada_id": bancada_id,
        "contagem_total": contagem_total,
        "delta_pecas": delta_pecas,
        "timestamp_esp": timestamp_esp,
        "status": status_raw if status_raw in {"OPERANDO", "PARADO", "OFFLINE_REPLAY"} else "OPERANDO",
    }

def _parse_status_payload(raw: bytes, topic: str) -> dict[str, Any] | None:
    """Lê e entende a mensagem de status da bancada."""
    try:
        data = json.loads(raw.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        logger.warning("Payload de status inválido: %s | raw=%r", exc, raw[:200])
        return None

    bancada_id = _extract_bancada_id(data, topic)
    
    # Padroniza o status
    status_input = str(data.get("status", "OFFLINE")).upper().strip()
    if status_input == "ONLINE":
        status_raw = "OPERANDO"
    elif status_input in {"OFFLINE", "PARADO"}:
        status_raw = status_input
    else:
        status_raw = "OPERANDO"

    # Trata timestamp Epoch ou data atual
    ts_raw = data.get("timestamp") or data.get("timestamp_esp")
    if isinstance(ts_raw, (int, float)):
        timestamp_esp = datetime.fromtimestamp(ts_raw, tz=timezone.utc).isoformat()
    else:
        timestamp_esp = str(ts_raw) if ts_raw else datetime.now(timezone.utc).isoformat()

    return {
        "bancada_id": bancada_id,
        "contagem_total": int(data.get("contagem_total", 0)),
        "delta_pecas": 0,  # Status/LWT não incrementa produção
        "timestamp_esp": timestamp_esp,
        "status": status_raw,
    }

# Salvamento no Banco

def _persist_telemetry(data: dict[str, Any]) -> bool:
    """Salva as informações no banco de dados."""
    bancada_id: str = data["bancada_id"]
    contagem_total: int = data["contagem_total"]
    delta_pecas: int = data["delta_pecas"]
    timestamp_esp_raw: str = data["timestamp_esp"]
    status_bancada: str = data["status"]

    try:
        timestamp_esp = datetime.fromisoformat(timestamp_esp_raw)
    except ValueError as exc:
        logger.warning("timestamp_esp inválido: %s | valor=%r", exc, timestamp_esp_raw)
        return False

    idempotency_key = TelemetriaBancada.build_idempotency_key(
        bancada_id=bancada_id,
        timestamp_esp=timestamp_esp_raw,
        contagem_total=contagem_total,
    )

    record = TelemetriaBancada(
        bancada_id=bancada_id,
        contagem_total=contagem_total,
        delta_pecas=delta_pecas,
        timestamp_esp=timestamp_esp,
        timestamp_servidor=datetime.now(tz=timezone.utc),
        status_bancada=status_bancada,
        idempotency_key=idempotency_key,
    )

    try:
        with get_session() as session:
            session.add(record)
        logger.info(
            "Telemetria persistida | bancada=%s | status=%s | contagem=%d | ts=%s",
            bancada_id, status_bancada, contagem_total, timestamp_esp_raw,
        )
        return True

    except IntegrityError:
        logger.info(
            "Duplicata ignorada | bancada=%s | key=%s",
            bancada_id, idempotency_key,
        )
        return False

    except Exception as exc:
        logger.error(
            "Erro ao persistir telemetria | bancada=%s | erro=%s",
            bancada_id, exc, exc_info=True,
        )
        return False


# Eventos do MQTT

def _on_connect(client: mqtt.Client, userdata: Any, flags: dict, rc: int) -> None:
    if rc == 0:
        logger.info(
            "Conectado ao broker MQTT %s:%d | Topico='%s' QoS=%d",
            mqtt_config.host, mqtt_config.port,
            mqtt_config.topic_filter, mqtt_config.qos,
        )
        client.subscribe(mqtt_config.topic_filter, qos=mqtt_config.qos)
    else:
        logger.error("Falha na conexão MQTT -- código de retorno: %d", rc)


def _on_disconnect(client: mqtt.Client, userdata: Any, rc: int) -> None:
    if rc != 0:
        logger.warning("Desconexão inesperada do broker MQTT (rc=%d). Reconectando...", rc)


def _on_message(client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
    """Define o que fazer quando chega uma mensagem nova."""
    topic = msg.topic
    logger.debug("Mensagem recebida | tópico=%s | %d bytes", topic, len(msg.payload))

    if topic.endswith("/producao"):
        data = _parse_production_payload(msg.payload, topic)
    elif topic.endswith("/status"):
        data = _parse_status_payload(msg.payload, topic)
    else:
        logger.warning("Tópico não reconhecido pelo roteador: %s", topic)
        return

    if data is not None:
        _persist_telemetry(data)


def _on_subscribe(client: mqtt.Client, userdata: Any, mid: int, granted_qos: list) -> None:
    logger.info("Subscrição confirmada | mid=%d | QoS concedido=%s", mid, granted_qos)


# O Ouvinte Principal

class MQTTListener:
    def __init__(self) -> None:
        self._client = mqtt.Client(
            client_id=mqtt_config.client_id,
            clean_session=True,
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = _on_connect
        self._client.on_disconnect = _on_disconnect
        self._client.on_message = _on_message
        self._client.on_subscribe = _on_subscribe

        self._client.reconnect_delay_set(
            min_delay=mqtt_config.reconnect_delay_min,
            max_delay=mqtt_config.reconnect_delay_max,
        )

    def start(self) -> None:
        logger.info("Iniciando MQTTListener -> %s:%d", mqtt_config.host, mqtt_config.port)
        self._client.connect(
            host=mqtt_config.host,
            port=mqtt_config.port,
            keepalive=mqtt_config.keepalive,
        )
        self._client.loop_start()
        logger.info("Loop MQTT iniciado em background.")

    def stop(self) -> None:
        logger.info("Encerrando MQTTListener...")
        self._client.loop_stop()
        self._client.disconnect()
        logger.info("MQTTListener encerrado.")

    def is_connected(self) -> bool:
        return self._client.is_connected()


if __name__ == "__main__":
    import sys

    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(name)s -- %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S",
    )

    from settings.database import init_db
    init_db()

    listener = MQTTListener()
    listener.start()

    try:
        logger.info("Listener rodando. Pressione Ctrl+C para sair.")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        logger.info("Interrompido pelo usuário.")
    finally:
        listener.stop()
        sys.exit(0)