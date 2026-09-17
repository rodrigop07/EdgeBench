"""
Configurações do Sistema.
Lê as variáveis do arquivo .env para configurar banco de dados, MQTT e Google Drive.
"""

import os
from dataclasses import dataclass, field
from dotenv import load_dotenv

load_dotenv()  # Lê o arquivo .env se ele existir


@dataclass(frozen=True)
class DatabaseConfig:
    """Configurações do banco de dados PostgreSQL."""

    host: str = field(default_factory=lambda: os.getenv("POSTGRES_HOST", "localhost"))
    port: int = field(default_factory=lambda: int(os.getenv("POSTGRES_PORT", "5432")))
    name: str = field(default_factory=lambda: os.getenv("POSTGRES_DB", "edgebench"))
    user: str = field(default_factory=lambda: os.getenv("POSTGRES_USER", "edgebench_user"))
    password: str = field(default_factory=lambda: os.getenv("POSTGRES_PASSWORD", "edgebench_pass"))
    pool_size: int = field(default_factory=lambda: int(os.getenv("DB_POOL_SIZE", "5")))
    max_overflow: int = field(default_factory=lambda: int(os.getenv("DB_MAX_OVERFLOW", "10")))

    @property
    def url(self) -> str:
        """Monta a URL de conexão do banco."""
        return (
            f"postgresql+psycopg2://{self.user}:{self.password}"
            f"@{self.host}:{self.port}/{self.name}"
        )


@dataclass(frozen=True)
class MQTTConfig:
    """Configurações do servidor de mensagens MQTT."""

    host: str = field(default_factory=lambda: os.getenv("MQTT_HOST", "localhost"))
    port: int = field(default_factory=lambda: int(os.getenv("MQTT_PORT", "1883")))
    client_id: str = field(default_factory=lambda: os.getenv("MQTT_CLIENT_ID", "edgebench_backend"))
    keepalive: int = field(default_factory=lambda: int(os.getenv("MQTT_KEEPALIVE", "60")))
    topic_filter: str = field(
        default_factory=lambda: os.getenv(
            "MQTT_TOPIC_FILTER", "fabrica/bancada_+/producao"
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
    """Outras configurações gerais."""

    log_level: str = field(default_factory=lambda: os.getenv("LOG_LEVEL", "INFO"))
    reports_dir: str = field(default_factory=lambda: os.getenv("REPORTS_DIR", "reports"))


# Instâncias prontas para usar no resto do código
db_config = DatabaseConfig()
mqtt_config = MQTTConfig()
app_config = AppConfig()

@dataclass(frozen=True)
class GoogleConfig:
    """Configurações da nuvem do Google."""

    credentials_path: str = field(
        default_factory=lambda: os.getenv("GOOGLE_APPLICATION_CREDENTIALS", "credentials.json")
    )
    token_path: str = field(
        default_factory=lambda: os.getenv("GOOGLE_TOKEN_PATH", "/app/token.json" if os.path.exists("/app") else "token.json")
    )
    folder_id: str = field(default_factory=lambda: os.getenv("GOOGLE_DRIVE_FOLDER_ID", ""))

google_config = GoogleConfig()
