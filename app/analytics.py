"""
analytics.py — Análise de séries temporais de telemetria via Pandas + SQL.

Funções disponíveis:
    - load_telemetry_df()      : Carrega todos os registros do PostgreSQL em um DataFrame.
    - aggregate_by_hour()      : Agrupamento de produção por hora.
    - aggregate_by_shift()     : Agrupamento por turno (T1/T2/T3).
    - compute_kpis()           : KPIs consolidados por bancada.
    - full_report()            : Dicionário completo com todos os DataFrames prontos para exportação.
"""

import logging
from typing import Any

import pandas as pd
from sqlalchemy import text

from database import engine

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Definição dos turnos de produção
# ─────────────────────────────────────────────────────────────────────────────

SHIFT_DEFINITIONS: list[dict[str, Any]] = [
    {"turno": "Turno 1", "label": "T1 (06h–14h)", "start": 6,  "end": 14},
    {"turno": "Turno 2", "label": "T2 (14h–22h)", "start": 14, "end": 22},
    {"turno": "Turno 3", "label": "T3 (22h–06h)", "start": 22, "end": 6},  # cruza meia-noite
]


def _classify_shift(hour: int) -> str:
    """Classifica uma hora (0-23) no turno correspondente."""
    if 6 <= hour < 14:
        return "T1 (06h–14h)"
    elif 14 <= hour < 22:
        return "T2 (14h–22h)"
    else:
        return "T3 (22h–06h)"


# ─────────────────────────────────────────────────────────────────────────────
# Carregamento de dados
# ─────────────────────────────────────────────────────────────────────────────

def load_telemetry_df(
    bancada_id: str | None = None,
    start_dt: str | None = None,
    end_dt: str | None = None,
) -> pd.DataFrame:
    """
    Carrega registros de telemetria do PostgreSQL em um DataFrame.

    Args:
        bancada_id : Filtra por bancada específica (opcional).
        start_dt   : Data/hora inicial no formato ISO 8601 (opcional).
        end_dt     : Data/hora final no formato ISO 8601 (opcional).

    Returns:
        DataFrame com colunas: id, bancada_id, contagem_total, delta_pecas,
        timestamp_esp, timestamp_servidor, status_bancada.
    """
    query = """
        SELECT
            id,
            bancada_id,
            contagem_total,
            delta_pecas,
            timestamp_esp,
            timestamp_servidor,
            status_bancada
        FROM telemetria_bancada
        WHERE 1=1
    """
    params: dict[str, Any] = {}

    if bancada_id:
        query += " AND bancada_id = :bancada_id"
        params["bancada_id"] = bancada_id

    if start_dt:
        query += " AND timestamp_esp >= :start_dt"
        params["start_dt"] = start_dt

    if end_dt:
        query += " AND timestamp_esp <= :end_dt"
        params["end_dt"] = end_dt

    query += " ORDER BY bancada_id, timestamp_esp"

    logger.info("Carregando telemetria do banco… filtros=%s", params or "nenhum")

    with engine.connect() as conn:
        df = pd.read_sql(text(query), conn, params=params)

    if df.empty:
        logger.warning("Nenhum registro encontrado com os filtros aplicados.")
        return df

    # ── Tipagem explícita ────────────────────────────────────────────────────
    df["timestamp_esp"] = pd.to_datetime(df["timestamp_esp"]).dt.tz_localize(None)
    df["timestamp_servidor"] = pd.to_datetime(df["timestamp_servidor"], utc=True).dt.tz_localize(None)
    df["hora"] = df["timestamp_esp"].dt.hour
    df["data"] = df["timestamp_esp"].dt.date
    df["turno"] = df["hora"].apply(_classify_shift)

    logger.info("DataFrame carregado: %d registros | %d bancadas únicas", len(df), df["bancada_id"].nunique())
    return df


# ─────────────────────────────────────────────────────────────────────────────
# Análises
# ─────────────────────────────────────────────────────────────────────────────

def aggregate_by_hour(df: pd.DataFrame) -> pd.DataFrame:
    """
    Agrega produção por bancada e hora do dia.

    Returns:
        DataFrame com colunas: bancada_id, data, hora, total_pecas,
        media_pecas_por_msg, mensagens, status_mais_frequente.
    """
    if df.empty:
        logger.warning("aggregate_by_hour: DataFrame vazio recebido.")
        return pd.DataFrame()

    grp = (
        df.groupby(["bancada_id", "data", "hora"])
        .agg(
            total_pecas=("delta_pecas", "sum"),
            media_pecas_por_msg=("delta_pecas", "mean"),
            mensagens=("id", "count"),
            status_mais_frequente=("status_bancada", lambda x: x.mode().iloc[0] if not x.empty else "N/A"),
        )
        .reset_index()
    )
    grp["media_pecas_por_msg"] = grp["media_pecas_por_msg"].round(2)
    grp = grp.sort_values(["bancada_id", "data", "hora"])
    logger.info("aggregate_by_hour: %d linhas geradas.", len(grp))
    return grp


def aggregate_by_shift(df: pd.DataFrame) -> pd.DataFrame:
    """
    Agrega produção por bancada e turno de produção.

    Returns:
        DataFrame com colunas: bancada_id, data, turno, total_pecas,
        mensagens, eficiencia_pct (vs. meta_por_turno=3200).
    """
    if df.empty:
        logger.warning("aggregate_by_shift: DataFrame vazio recebido.")
        return pd.DataFrame()

    META_PECAS_TURNO = 3200  # Meta de produção por turno (configurável)

    grp = (
        df.groupby(["bancada_id", "data", "turno"])
        .agg(
            total_pecas=("delta_pecas", "sum"),
            mensagens=("id", "count"),
            contagem_final=("contagem_total", "max"),
            status_mais_frequente=("status_bancada", lambda x: x.mode().iloc[0] if not x.empty else "N/A"),
        )
        .reset_index()
    )
    grp["eficiencia_pct"] = ((grp["total_pecas"] / META_PECAS_TURNO) * 100).round(2)
    grp["meta_turno"] = META_PECAS_TURNO
    grp = grp.sort_values(["bancada_id", "data", "turno"])
    logger.info("aggregate_by_shift: %d linhas geradas.", len(grp))
    return grp


def compute_kpis(df: pd.DataFrame) -> pd.DataFrame:
    """
    Gera KPIs consolidados por bancada.

    Returns:
        DataFrame com colunas: bancada_id, total_pecas, total_mensagens,
        media_delta, max_contagem, uptime_pct, primeiro_registro, ultimo_registro.
    """
    if df.empty:
        logger.warning("compute_kpis: DataFrame vazio recebido.")
        return pd.DataFrame()

    kpis = (
        df.groupby("bancada_id")
        .agg(
            total_pecas=("delta_pecas", "sum"),
            total_mensagens=("id", "count"),
            media_delta=("delta_pecas", "mean"),
            max_contagem=("contagem_total", "max"),
            primeiro_registro=("timestamp_esp", "min"),
            ultimo_registro=("timestamp_esp", "max"),
        )
        .reset_index()
    )

    # Uptime: % do tempo em status OPERANDO
    operando_df = df[df["status_bancada"] == "OPERANDO"]
    operando_count = (
        operando_df.groupby("bancada_id")["id"]
        .count()
        .rename("msgs_operando")
        .reset_index()
    )
    kpis = kpis.merge(operando_count, on="bancada_id", how="left")
    kpis["msgs_operando"] = kpis["msgs_operando"].fillna(0)
    kpis["uptime_pct"] = ((kpis["msgs_operando"] / kpis["total_mensagens"]) * 100).round(2)
    kpis["media_delta"] = kpis["media_delta"].round(2)

    kpis = kpis.drop(columns=["msgs_operando"])
    logger.info("compute_kpis: KPIs gerados para %d bancadas.", len(kpis))
    return kpis


def full_report(
    bancada_id: str | None = None,
    start_dt: str | None = None,
    end_dt: str | None = None,
) -> dict[str, pd.DataFrame]:
    """
    Gera o pacote completo de análises prontas para exportação.

    Returns:
        Dicionário com chaves: 'raw', 'por_hora', 'por_turno', 'kpis'.
    """
    df = load_telemetry_df(bancada_id=bancada_id, start_dt=start_dt, end_dt=end_dt)

    return {
        "raw": df,
        "por_hora": aggregate_by_hour(df),
        "por_turno": aggregate_by_shift(df),
        "kpis": compute_kpis(df),
    }
