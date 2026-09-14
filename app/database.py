"""
database.py — Engine e fábrica de sessões SQLAlchemy 2.0.

Fornece:
    - `engine`        : Engine configurada com pool de conexões.
    - `SessionLocal`  : Fábrica de sessões (scoped_session para thread-safety).
    - `get_session()` : Context manager para uso com `with`.
    - `init_db()`     : Cria todas as tabelas mapeadas (idempotente).
"""

import logging
from contextlib import contextmanager
from typing import Generator

from sqlalchemy import create_engine, event, text
from sqlalchemy.orm import DeclarativeBase, Session, scoped_session, sessionmaker

from config import db_config

logger = logging.getLogger(__name__)


# ── Base declarativa compartilhada por todos os models ───────────────────────
class Base(DeclarativeBase):
    pass


# ── Engine com pool de conexões ──────────────────────────────────────────────
engine = create_engine(
    db_config.url,
    pool_size=db_config.pool_size,
    max_overflow=db_config.max_overflow,
    pool_pre_ping=True,         # Valida conexões antes de entregar do pool
    pool_recycle=1800,          # Recicla conexões a cada 30 minutos
    echo=False,                 # Defina True para debug de queries SQL
)


# ── Verificação de conectividade no startup ───────────────────────────────────
@event.listens_for(engine, "connect")
def _on_connect(dbapi_conn, connection_record):
    logger.debug("Nova conexão PostgreSQL estabelecida (pid=%s)", dbapi_conn.get_backend_pid())


# ── Fábrica de sessões thread-safe ───────────────────────────────────────────
_session_factory = sessionmaker(
    bind=engine,
    autocommit=False,
    autoflush=False,
    expire_on_commit=False,     # Evita lazy-load após commit em threads secundárias
)

SessionLocal: scoped_session = scoped_session(_session_factory)


@contextmanager
def get_session() -> Generator[Session, None, None]:
    """
    Context manager que fornece uma sessão e garante commit/rollback/close.

    Uso:
        with get_session() as session:
            session.add(obj)
    """
    session: Session = SessionLocal()
    try:
        yield session
        session.commit()
    except Exception:
        session.rollback()
        raise
    finally:
        session.close()


def init_db() -> None:
    """
    Cria todas as tabelas no banco de dados caso não existam.
    Deve ser chamado uma única vez durante o startup da aplicação.
    """
    import models  # noqa: F401 — importar para registrar os mappers na Base

    logger.info("Inicializando schema do banco de dados…")
    Base.metadata.create_all(bind=engine)
    logger.info("Schema criado/verificado com sucesso.")


def check_connection() -> bool:
    """Verifica se o banco está acessível. Retorna True em caso de sucesso."""
    try:
        with engine.connect() as conn:
            conn.execute(text("SELECT 1"))
        return True
    except Exception as exc:
        logger.error("Falha ao conectar ao PostgreSQL: %s", exc)
        return False
