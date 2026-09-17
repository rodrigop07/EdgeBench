"""
Desenho da Tabela do Banco de Dados.
Aqui definimos quais informações vamos guardar de cada mensagem que chega das bancadas.
"""

from datetime import datetime, timezone

from sqlalchemy import DateTime, Index, Integer, String, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column

from database import Base


class TelemetriaBancada(Base):
    """Representa uma leitura recebida de uma bancada."""

    __tablename__ = "telemetria_bancada"

    # ID único
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)

    # Qual bancada mandou
    bancada_id: Mapped[str] = mapped_column(String(20), nullable=False, index=True)

    # Quantas peças
    contagem_total: Mapped[int] = mapped_column(Integer, nullable=False)
    delta_pecas: Mapped[int] = mapped_column(Integer, nullable=False, default=0)

    # Data e hora
    timestamp_esp: Mapped[datetime] = mapped_column(
        DateTime(timezone=False),
        nullable=False,
    )
    timestamp_servidor: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),  # Gerado pelo PostgreSQL automaticamente
    )

    # Situação da bancada
    status_bancada: Mapped[str] = mapped_column(String(30), nullable=False, default="DESCONHECIDO")

    # Evita salvar a mesma leitura duas vezes
    idempotency_key: Mapped[str] = mapped_column(
        String(120),
        nullable=False,
        unique=True,
    )

    # Regras extras do banco
    __table_args__ = (
        UniqueConstraint("idempotency_key", name="uq_telemetria_idempotency_key"),
        Index("ix_telemetria_bancada_timestamp", "bancada_id", "timestamp_esp"),
    )

    def __repr__(self) -> str:
        return (
            f"<TelemetriaBancada "
            f"bancada={self.bancada_id!r} "
            f"contagem={self.contagem_total} "
            f"status={self.status_bancada!r} "
            f"ts_esp={self.timestamp_esp}>"
        )

    @classmethod
    def build_idempotency_key(
        cls, bancada_id: str, timestamp_esp: str, contagem_total: int
    ) -> str:
        """Cria o código único pra não termos dados repetidos."""
        return f"{bancada_id}_{timestamp_esp}_{contagem_total}"
