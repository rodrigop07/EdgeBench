"""
analytics.py — Análise de séries temporais de telemetria via Pandas + SQL.

Funções disponíveis:
    - load_telemetry_df()      : Carrega registros do PostgreSQL com tipagem e campos de data/hora.
    - detect_idleness()        : Detecta paradas não programadas (>15 min) conforme a regra RN-06.
    - aggregate_hourly_pivot() : Gera Matriz Cruzada (Pivot) de Bancada x Hora para o PCP.
    - aggregate_by_hour()      : Agrupamento plano de produção por hora.
    - aggregate_by_shift()     : Agrupamento por turno (T1/T2/T3) com totais e eficiência.
    - compute_kpis()           : KPIs consolidados individuais por bancada.
    - full_report()            : Dicionário completo estruturado para exportação executiva no Excel.
"""

import logging
from typing import Any, Dict, List, Optional, Tuple

import pandas as pd
from sqlalchemy import text

from database import engine

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Definição dos turnos de produção (Regra RN-02)
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

    # Tipagem e campos derivados
    df["timestamp_esp"] = pd.to_datetime(df["timestamp_esp"]).dt.tz_localize(None)
    df["timestamp_servidor"] = pd.to_datetime(df["timestamp_servidor"], utc=True).dt.tz_localize(None)
    df["hora"] = df["timestamp_esp"].dt.hour
    df["data"] = df["timestamp_esp"].dt.date
    df["turno"] = df["hora"].apply(_classify_shift)

    logger.info("DataFrame carregado: %d registros | %d bancadas únicas", len(df), df["bancada_id"].nunique())
    return df


# ─────────────────────────────────────────────────────────────────────────────
# Detecção de Paradas Não Programadas (>15 min)
# ─────────────────────────────────────────────────────────────────────────────

def detect_idleness(df: pd.DataFrame, threshold_minutes: int = 15) -> Tuple[pd.DataFrame, Dict[str, Dict[str, Any]]]:
    """
    Identifica períodos onde uma bancada permaneceu mais de threshold_minutes
    consecutivos sem produzir qualquer peça durante o turno operacional.
    """
    if df.empty:
        return pd.DataFrame(), {}

    downtimes: List[Dict[str, Any]] = []
    summary: Dict[str, Dict[str, Any]] = {}
    threshold = pd.Timedelta(minutes=threshold_minutes)

    for bancada, group in df.sort_values("timestamp_esp").groupby("bancada_id"):
        prod_group = group[group["delta_pecas"] > 0]
        total_paradas = 0
        tempo_total_min = 0.0

        if len(prod_group) >= 2:
            timestamps = prod_group["timestamp_esp"].values
            for i in range(1, len(timestamps)):
                t_prev = pd.to_datetime(timestamps[i - 1])
                t_curr = pd.to_datetime(timestamps[i])
                delta = t_curr - t_prev

                # Intervalo > 15 min e menor que 12 horas (ignora desligamento noturno de linha)
                if delta > threshold and delta < pd.Timedelta(hours=12):
                    duracao_min = round(delta.total_seconds() / 60.0, 1)
                    total_paradas += 1
                    tempo_total_min += duracao_min

                    downtimes.append({
                        "bancada_id": bancada,
                        "inicio_parada": t_prev.strftime("%d/%m/%Y %H:%M:%S"),
                        "fim_parada": t_curr.strftime("%d/%m/%Y %H:%M:%S"),
                        "duracao_minutos": duracao_min,
                        "classificacao": "Parada Operacional (>15 min)",
                        "impacto_estimado": f"{int(duracao_min * 0.5)} peças não produzidas",
                    })

        summary[str(bancada)] = {
            "count": total_paradas,
            "total_minutes": tempo_total_min,
        }

    downtimes_df = pd.DataFrame(downtimes)
    logger.info("detect_idleness: %d paradas >%d min detectadas na fábrica.", len(downtimes_df), threshold_minutes)
    return downtimes_df, summary


# ─────────────────────────────────────────────────────────────────────────────
# Matriz Cruzada Horária (PCP Pivot Table - Seção 5.4)
# ─────────────────────────────────────────────────────────────────────────────

def aggregate_hourly_pivot(df: pd.DataFrame) -> pd.DataFrame:
    """
    Gera a Matriz Cruzada de Bancadas (Linhas) vs Horas (Colunas) com totalizadores.
    """
    if df.empty:
        return pd.DataFrame()

    prod_df = df[df["delta_pecas"] > 0].copy()
    if prod_df.empty:
        return pd.DataFrame()

    prod_df["hora_col"] = prod_df["hora"].apply(lambda h: f"{h:02d}:00")

    pivot = prod_df.pivot_table(
        index="bancada_id",
        columns="hora_col",
        values="delta_pecas",
        aggfunc="sum",
        fill_value=0,
    )

    # Ordena as colunas cronologicamente
    sorted_cols = sorted(list(pivot.columns))
    pivot = pivot[sorted_cols]

    # Total por bancada (coluna na direita)
    pivot["TOTAL BANCADA"] = pivot.sum(axis=1)

    # Total geral por hora (linha inferior)
    total_row = pivot.sum(axis=0)
    total_row.name = "TOTAL HORA"
    pivot.loc["TOTAL HORA"] = total_row

    return pivot.reset_index()


# ─────────────────────────────────────────────────────────────────────────────
# Agregações Complementares
# ─────────────────────────────────────────────────────────────────────────────

def aggregate_by_hour(df: pd.DataFrame) -> pd.DataFrame:
    """Agrega produção por bancada e hora do dia (visão tabular analítica)."""
    if df.empty:
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
    return grp


def aggregate_by_shift(df: pd.DataFrame) -> pd.DataFrame:
    """Agrega produção por bancada e turno com percentual de eficiência formatável."""
    if df.empty:
        return pd.DataFrame()

    META_PECAS_TURNO = 3200

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
    # Mantém valor decimal entre 0.0 e 1.0 para formatação nativa de porcentagem no Excel
    grp["eficiencia_decimal"] = (grp["total_pecas"] / META_PECAS_TURNO).round(4)
    grp["meta_turno"] = META_PECAS_TURNO
    grp = grp.sort_values(["bancada_id", "data", "turno"])
    return grp


def compute_kpis(df: pd.DataFrame, idleness_summary: Optional[Dict[str, Dict[str, Any]]] = None) -> pd.DataFrame:
    """Gera KPIs consolidados por bancada, incluindo métricas da regra RN-06."""
    if df.empty:
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

    # Uptime: % de mensagens com status OPERANDO
    operando_df = df[df["status_bancada"] == "OPERANDO"]
    operando_count = (
        operando_df.groupby("bancada_id")["id"]
        .count()
        .rename("msgs_operando")
        .reset_index()
    )
    kpis = kpis.merge(operando_count, on="bancada_id", how="left")
    kpis["msgs_operando"] = kpis["msgs_operando"].fillna(0)
    kpis["uptime_pct"] = ((kpis["msgs_operando"] / kpis["total_mensagens"]) * 100).round(1)
    kpis["media_delta"] = kpis["media_delta"].round(2)
    kpis = kpis.drop(columns=["msgs_operando"])

    # Incorpora métricas de paradas operacionais
    if idleness_summary:
        kpis["paradas"] = kpis["bancada_id"].apply(
            lambda b: idleness_summary.get(str(b), {}).get("count", 0)
        )
        kpis["tempo_parado_min"] = kpis["bancada_id"].apply(
            lambda b: idleness_summary.get(str(b), {}).get("total_minutes", 0.0)
        )
    else:
        kpis["paradas"] = 0
        kpis["tempo_parado_min"] = 0.0

    kpis["paradas_rn06"] = kpis["paradas"]
    kpis["tempo_ocioso_min"] = kpis["tempo_parado_min"]

    return kpis


# ─────────────────────────────────────────────────────────────────────────────
# Relatório Completo Estruturado
# ─────────────────────────────────────────────────────────────────────────────

def full_report(
    bancada_id: str | None = None,
    start_dt: str | None = None,
    end_dt: str | None = None,
) -> Dict[str, Any]:
    """Gera o pacote analítico completo com métricas globais e específicas."""
    df = load_telemetry_df(bancada_id=bancada_id, start_dt=start_dt, end_dt=end_dt)

    downtimes_df, idleness_summary = detect_idleness(df)

    # Métricas consolidadas no nível da Fábrica/Linha
    total_pecas = int(df["delta_pecas"].sum()) if not df.empty else 0
    bancadas_ativas = int(df["bancada_id"].nunique()) if not df.empty else 0
    horas_ativas = int(df["hora"].nunique()) if not df.empty else 1
    taxa_hora = round(total_pecas / max(horas_ativas, 1), 1)

    global_kpis = {
        "total_pecas": total_pecas,
        "bancadas_ativas": bancadas_ativas,
        "taxa_pecas_hora": taxa_hora,
        "total_paradas": len(downtimes_df),
        "total_paradas_rn06": len(downtimes_df),
        "primeiro_registro": df["timestamp_esp"].min().strftime("%d/%m/%Y %H:%M") if not df.empty else "—",
        "ultimo_registro": df["timestamp_esp"].max().strftime("%d/%m/%Y %H:%M") if not df.empty else "—",
    }

    return {
        "raw": df,
        "global_kpis": global_kpis,
        "kpis": compute_kpis(df, idleness_summary),
        "por_hora_pivot": aggregate_hourly_pivot(df),
        "por_hora": aggregate_by_hour(df),
        "por_turno": aggregate_by_shift(df),
        "ociosidade": downtimes_df,
    }
