"""
config.py — Centralização de configurações via variáveis de ambiente.

Carrega automaticamente um arquivo `.env` na raiz do projeto (se existir),
permitindo sobrescrita sem alterar o código-fonte.
"""

import os
from dataclasses import dataclass, field
from dotenv import load_dotenv

load_dotenv()  # Carrega variáveis do arquivo .env (se presente)


@dataclass(frozen=True)
class DatabaseConfig:
    """Parâmetros de conexão com o PostgreSQL."""

    host: str = field(default_factory=lambda: os.getenv("POSTGRES_HOST", "localhost"))
    port: int = field(default_factory=lambda: int(os.getenv("POSTGRES_PORT", "5432")))
    name: str = field(default_factory=lambda: os.getenv("POSTGRES_DB", "edgebench"))
    user: str = field(default_factory=lambda: os.getenv("POSTGRES_USER", "edgebench_user"))
    password: str = field(default_factory=lambda: os.getenv("POSTGRES_PASSWORD", "edgebench_pass"))
    pool_size: int = field(default_factory=lambda: int(os.getenv("DB_POOL_SIZE", "5")))
    max_overflow: int = field(default_factory=lambda: int(os.getenv("DB_MAX_OVERFLOW", "10")))

    @property
    def url(self) -> str:
        """Retorna a DSN completa para o SQLAlchemy."""
        return (
            f"postgresql+psycopg2://{self.user}:{self.password}"
            f"@{self.host}:{self.port}/{self.name}"
        )


@dataclass(frozen=True)
class MQTTConfig:
    """Parâmetros de conexão com o broker MQTT."""

    host: str = field(default_factory=lambda: os.getenv("MQTT_HOST", "localhost"))
    port: int = field(default_factory=lambda: int(os.getenv("MQTT_PORT", "1883")))
    client_id: str = field(default_factory=lambda: os.getenv("MQTT_CLIENT_ID", "edgebench_backend"))
    keepalive: int = field(default_factory=lambda: int(os.getenv("MQTT_KEEPALIVE", "60")))
    topic_filter: str = field(
        default_factory=lambda: os.getenv(
            "MQTT_TOPIC_FILTER", "edgebench/bancadas/+/telemetria"
        )
    )
    qos: int = field(default_factory=lambda: int(os.getenv("MQTT_QOS", "1")))
    reconnect_delay_min: int = field(
        default_factory=lambda: int(os.getenv("MQTT_RECONNECT_MIN", "1"))
    )
    reconnect_delay_max: int = field(
        default_factory=lambda: int(os.getenv("MQTT_RECONNECT_MAX", "30"))
    )


@dataclass(frozen=True)
class AppConfig:
    """Configurações gerais da aplicação."""

    log_level: str = field(default_factory=lambda: os.getenv("LOG_LEVEL", "INFO"))
    reports_dir: str = field(default_factory=lambda: os.getenv("REPORTS_DIR", "reports"))


# ── Instâncias prontas para importação ───────────────────────────────────────
db_config = DatabaseConfig()
mqtt_config = MQTTConfig()
app_config = AppConfig()

@dataclass(frozen=True)
class GoogleConfig:
    """Configurações de integração com Google Sheets/Drive."""

    credentials_path: str = field(
        default_factory=lambda: os.getenv("GOOGLE_APPLICATION_CREDENTIALS", "credentials.json")
    )
    token_path: str = field(
        default_factory=lambda: os.getenv("GOOGLE_TOKEN_PATH", "/app/token.json" if os.path.exists("/app") else "token.json")
    )
    folder_id: str = field(default_factory=lambda: os.getenv("GOOGLE_DRIVE_FOLDER_ID", ""))

google_config = GoogleConfig()
