"""
models.py — Mapeamento ORM da tabela `telemetria_bancada`.

Campos:
    id                : PK serial auto-incrementada.
    bancada_id        : Identificador da bancada (ex.: "BC-01").
    contagem_total    : Contador acumulado de peças desde a inicialização do ESP.
    delta_pecas       : Incremento de peças em relação à leitura anterior.
    timestamp_esp     : Timestamp gerado pelo ESP32 (sem fuso — UTC assumido).
    timestamp_servidor: Timestamp de recebimento no servidor (UTC).
    status_bancada    : Estado operacional da bancada ("OPERANDO", "PARADO", etc.).
    idempotency_key   : Chave única para deduplicação: {bancada_id}_{timestamp_esp}_{contagem_total}.
"""

from datetime import datetime, timezone

from sqlalchemy import DateTime, Index, Integer, String, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column

from database import Base


class TelemetriaBancada(Base):
    """Representa um registro de telemetria enviado por uma bancada de produção."""

    __tablename__ = "telemetria_bancada"

    # ── Chave primária ────────────────────────────────────────────────────────
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)

    # ── Identificação da bancada ──────────────────────────────────────────────
    bancada_id: Mapped[str] = mapped_column(String(20), nullable=False, index=True)

    # ── Métricas de produção ──────────────────────────────────────────────────
    contagem_total: Mapped[int] = mapped_column(Integer, nullable=False)
    delta_pecas: Mapped[int] = mapped_column(Integer, nullable=False, default=0)

    # ── Timestamps ────────────────────────────────────────────────────────────
    timestamp_esp: Mapped[datetime] = mapped_column(
        DateTime(timezone=False),
        nullable=False,
    )
    timestamp_servidor: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=func.now(),  # Gerado pelo PostgreSQL automaticamente
    )

    # ── Status operacional ────────────────────────────────────────────────────
    status_bancada: Mapped[str] = mapped_column(String(30), nullable=False, default="DESCONHECIDO")

    # ── Chave de idempotência ─────────────────────────────────────────────────
    idempotency_key: Mapped[str] = mapped_column(
        String(120),
        nullable=False,
        unique=True,
    )

    # ── Constraints e índices adicionais ──────────────────────────────────────
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
        """
        Constrói a chave de idempotência no formato:
            {bancada_id}_{timestamp_esp}_{contagem_total}

        Exemplo: BC-01_2026-09-14T14:30:00_1250
        """
        return f"{bancada_id}_{timestamp_esp}_{contagem_total}"
