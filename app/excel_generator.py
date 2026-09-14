"""
excel_generator.py — Gerador de relatório executivo `.xlsx` via Openpyxl.

Layout do relatório:
    Aba 1 "KPI Cards"     : Cartões visuais de KPI por bancada.
    Aba 2 "Por Hora"      : Tabela zebrada de produção por hora.
    Aba 3 "Por Turno"     : Tabela zebrada de produção por turno com eficiência%.
    Aba 4 "Dados Brutos"  : Exportação completa dos registros.

Paleta PCP:
    Azul Escuro  #1F4E78  (cabeçalhos)
    Azul Médio   #2E75B6  (KPI cards header)
    Azul Claro   #BDD7EE  (zebra par)
    Branco       #FFFFFF  (zebra ímpar)
    Cinza Borda  #8EA9C1  (bordas)
    Texto Claro  #FFFFFF
    Texto Escuro #1A1A2E
"""

import logging
import os
from datetime import datetime, timezone
from typing import Any

import pandas as pd
from openpyxl import Workbook
from openpyxl.styles import (
    Alignment,
    Border,
    Font,
    GradientFill,
    PatternFill,
    Side,
)
from openpyxl.utils import get_column_letter
from openpyxl.utils.dataframe import dataframe_to_rows

from config import app_config

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Constantes de estilo (paleta PCP)
# ─────────────────────────────────────────────────────────────────────────────

C_HEADER_DARK    = "1F4E78"   # Azul escuro PCP — cabeçalhos
C_HEADER_MID     = "2E75B6"   # Azul médio — sub-cabeçalhos / KPI cards
C_ZEBRA_EVEN     = "BDD7EE"   # Azul claro — linhas pares
C_ZEBRA_ODD      = "FFFFFF"   # Branco — linhas ímpares
C_BORDER         = "8EA9C1"   # Cinza-azulado — bordas
C_TEXT_LIGHT     = "FFFFFF"   # Texto sobre fundos escuros
C_TEXT_DARK      = "1A1A2E"   # Texto sobre fundos claros
C_KPI_BG         = "EEF3FA"   # Fundo dos blocos KPI
C_ACCENT_GREEN   = "70AD47"   # Verde para valores positivos
C_ACCENT_ORANGE  = "ED7D31"   # Laranja para alertas

FONT_TITLE  = Font(name="Calibri", bold=True, size=16, color=C_HEADER_DARK)
FONT_HEADER = Font(name="Calibri", bold=True, size=10, color=C_TEXT_LIGHT)
FONT_KPI_VAL= Font(name="Calibri", bold=True, size=20, color=C_HEADER_MID)
FONT_KPI_LBL= Font(name="Calibri", bold=False, size=9, color="5A5A5A")
FONT_BODY   = Font(name="Calibri", size=9, color=C_TEXT_DARK)
FONT_BODY_B = Font(name="Calibri", bold=True, size=9, color=C_TEXT_DARK)

FILL_HEADER_DARK = PatternFill("solid", fgColor=C_HEADER_DARK)
FILL_HEADER_MID  = PatternFill("solid", fgColor=C_HEADER_MID)
FILL_ZEBRA_EVEN  = PatternFill("solid", fgColor=C_ZEBRA_EVEN)
FILL_KPI_BG      = PatternFill("solid", fgColor=C_KPI_BG)

ALIGN_CENTER = Alignment(horizontal="center", vertical="center", wrap_text=True)
ALIGN_LEFT   = Alignment(horizontal="left",   vertical="center", wrap_text=True)
ALIGN_RIGHT  = Alignment(horizontal="right",  vertical="center")

_border_side = Side(style="thin", color=C_BORDER)
BORDER_ALL   = Border(left=_border_side, right=_border_side, top=_border_side, bottom=_border_side)
_thick_side  = Side(style="medium", color=C_HEADER_DARK)
BORDER_HEADER= Border(left=_border_side, right=_border_side, top=_thick_side, bottom=_thick_side)


# ─────────────────────────────────────────────────────────────────────────────
# Funções auxiliares de formatação
# ─────────────────────────────────────────────────────────────────────────────

def _auto_column_width(ws, min_width: int = 10, max_width: int = 50) -> None:
    """Ajusta automaticamente a largura de todas as colunas da planilha."""
    for col_cells in ws.columns:
        col_letter = get_column_letter(col_cells[0].column)
        max_len = 0
        for cell in col_cells:
            if cell.value is not None:
                cell_len = len(str(cell.value))
                max_len = max(max_len, cell_len)
        ws.column_dimensions[col_letter].width = max(min_width, min(max_len + 4, max_width))


def _style_header_row(ws, row_idx: int, n_cols: int, fill: PatternFill = FILL_HEADER_DARK) -> None:
    """Aplica estilo de cabeçalho a uma linha inteira."""
    for col in range(1, n_cols + 1):
        cell = ws.cell(row=row_idx, column=col)
        cell.font = FONT_HEADER
        cell.fill = fill
        cell.alignment = ALIGN_CENTER
        cell.border = BORDER_HEADER


def _write_dataframe(
    ws,
    df: pd.DataFrame,
    start_row: int = 1,
    start_col: int = 1,
    header_fill: PatternFill = FILL_HEADER_DARK,
) -> int:
    """
    Escreve um DataFrame na planilha com estilo zebrado.

    Retorna a última linha escrita.
    """
    n_cols = len(df.columns)

    # Cabeçalhos
    for col_idx, col_name in enumerate(df.columns, start=start_col):
        cell = ws.cell(row=start_row, column=col_idx, value=str(col_name).replace("_", " ").title())
        cell.font = FONT_HEADER
        cell.fill = header_fill
        cell.alignment = ALIGN_CENTER
        cell.border = BORDER_HEADER
    ws.row_dimensions[start_row].height = 20

    # Dados com zebra
    for row_offset, row_data in enumerate(dataframe_to_rows(df, index=False, header=False), start=1):
        current_row = start_row + row_offset
        is_even = row_offset % 2 == 0
        fill = FILL_ZEBRA_EVEN if is_even else PatternFill("solid", fgColor=C_ZEBRA_ODD)

        for col_idx, value in enumerate(row_data, start=start_col):
            cell = ws.cell(row=current_row, column=col_idx, value=value)
            cell.font = FONT_BODY
            cell.fill = fill
            cell.alignment = ALIGN_LEFT
            cell.border = BORDER_ALL

            # Alinhamento numérico à direita
            if isinstance(value, (int, float)):
                cell.alignment = ALIGN_RIGHT

        ws.row_dimensions[current_row].height = 16

    return start_row + len(df)


# ─────────────────────────────────────────────────────────────────────────────
# Aba 1: KPI Cards
# ─────────────────────────────────────────────────────────────────────────────

def _build_kpi_sheet(wb: Workbook, kpis_df: pd.DataFrame) -> None:
    """Constrói a aba de cartões KPI por bancada."""
    ws = wb.active
    ws.title = "KPI Cards"
    ws.sheet_view.showGridLines = False

    # ── Título principal ──────────────────────────────────────────────────────
    ws.merge_cells("B2:J2")
    title_cell = ws["B2"]
    title_cell.value = "⚙  EdgeBench — Relatório Executivo de Produção"
    title_cell.font = FONT_TITLE
    title_cell.alignment = ALIGN_LEFT
    ws.row_dimensions[2].height = 28

    # Subtítulo com timestamp
    ws.merge_cells("B3:J3")
    sub_cell = ws["B3"]
    sub_cell.value = f"Gerado em: {datetime.now(tz=timezone.utc).strftime('%d/%m/%Y %H:%M')} UTC"
    sub_cell.font = Font(name="Calibri", size=9, italic=True, color="888888")
    sub_cell.alignment = ALIGN_LEFT
    ws.row_dimensions[3].height = 14

    if kpis_df.empty:
        ws["B5"].value = "Nenhum dado disponível para o período selecionado."
        return

    # ── Cartões KPI (grid 2 × N por bancada) ─────────────────────────────────
    current_row = 5
    for _, row in kpis_df.iterrows():
        bancada = row["bancada_id"]

        # Header do cartão
        ws.merge_cells(f"B{current_row}:J{current_row}")
        hdr = ws.cell(row=current_row, column=2)
        hdr.value = f"  Bancada: {bancada}"
        hdr.font = Font(name="Calibri", bold=True, size=11, color=C_TEXT_LIGHT)
        hdr.fill = FILL_HEADER_MID
        hdr.alignment = ALIGN_LEFT
        hdr.border = BORDER_HEADER
        ws.row_dimensions[current_row].height = 22
        current_row += 1

        # Métricas
        kpi_items: list[tuple[str, Any, str]] = [
            ("Total de Peças",      int(row.get("total_pecas", 0)),           "#"),
            ("Total de Mensagens",  int(row.get("total_mensagens", 0)),        "#"),
            ("Média Delta/msg",     float(row.get("media_delta", 0)),          "un"),
            ("Contagem Máxima",     int(row.get("max_contagem", 0)),           "#"),
            ("Uptime OPERANDO",     f"{row.get('uptime_pct', 0):.1f}%",        ""),
            ("Primeiro Registro",   str(row.get("primeiro_registro", "—")),    ""),
            ("Último Registro",     str(row.get("ultimo_registro", "—")),      ""),
        ]

        # Layout em duas colunas (B-E | F-I)
        col_positions = [(2, 5), (6, 9)]  # (label_col, value_end_col)
        for i, (label, value, unit) in enumerate(kpi_items):
            sub_row = current_row + (i // 2)
            col_start = col_positions[i % 2][0]
            col_end = col_positions[i % 2][1]

            ws.merge_cells(
                start_row=sub_row, start_column=col_start,
                end_row=sub_row, end_column=col_end,
            )
            cell = ws.cell(row=sub_row, column=col_start)
            display = f"{label}:  {value} {unit}".strip()
            cell.value = display
            cell.font = FONT_BODY_B if isinstance(value, (int, float)) else FONT_BODY
            cell.fill = FILL_KPI_BG
            cell.alignment = ALIGN_LEFT
            cell.border = BORDER_ALL
            ws.row_dimensions[sub_row].height = 18

        rows_used = (len(kpi_items) + 1) // 2
        current_row += rows_used + 2  # Espaçamento entre cartões

    # Larguras fixas para o layout de cartões
    ws.column_dimensions["A"].width = 2
    for col_letter in ["B", "C", "D", "E", "F", "G", "H", "I", "J"]:
        ws.column_dimensions[col_letter].width = 18


# ─────────────────────────────────────────────────────────────────────────────
# Aba 2: Produção por Hora
# ─────────────────────────────────────────────────────────────────────────────

def _build_hourly_sheet(wb: Workbook, hora_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Por Hora")
    ws.sheet_view.showGridLines = False

    ws.merge_cells("A1:G1")
    title = ws["A1"]
    title.value = "Produção por Hora — EdgeBench"
    title.font = FONT_TITLE
    title.alignment = ALIGN_LEFT
    ws.row_dimensions[1].height = 26

    if not hora_df.empty:
        _write_dataframe(ws, hora_df, start_row=3)
    else:
        ws["A3"].value = "Sem dados para o período."

    _auto_column_width(ws)
    ws.freeze_panes = "A4"


# ─────────────────────────────────────────────────────────────────────────────
# Aba 3: Produção por Turno
# ─────────────────────────────────────────────────────────────────────────────

def _build_shift_sheet(wb: Workbook, turno_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Por Turno")
    ws.sheet_view.showGridLines = False

    ws.merge_cells("A1:H1")
    title = ws["A1"]
    title.value = "Produção por Turno — EdgeBench"
    title.font = FONT_TITLE
    title.alignment = ALIGN_LEFT
    ws.row_dimensions[1].height = 26

    if turno_df.empty:
        ws["A3"].value = "Sem dados para o período."
        return

    last_row = _write_dataframe(ws, turno_df, start_row=3)

    # Formatação condicional manual: eficiência < 80% em laranja
    if "eficiencia_pct" in turno_df.columns:
        efic_col_idx = list(turno_df.columns).index("eficiencia_pct") + 1
        for r in range(4, last_row + 2):
            cell = ws.cell(row=r, column=efic_col_idx)
            if isinstance(cell.value, (int, float)) and cell.value < 80:
                cell.font = Font(name="Calibri", bold=True, size=9, color=C_ACCENT_ORANGE)

    _auto_column_width(ws)
    ws.freeze_panes = "A4"


# ─────────────────────────────────────────────────────────────────────────────
# Aba 4: Dados Brutos
# ─────────────────────────────────────────────────────────────────────────────

def _build_raw_sheet(wb: Workbook, raw_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Dados Brutos")
    ws.sheet_view.showGridLines = False

    # Remove colunas auxiliares geradas pelo analytics antes de exportar
    export_cols = [c for c in raw_df.columns if c not in {"hora", "data", "turno"}]
    df_export = raw_df[export_cols].copy() if not raw_df.empty else raw_df

    ws.merge_cells("A1:H1")
    title = ws["A1"]
    title.value = "Dados Brutos de Telemetria — EdgeBench"
    title.font = FONT_TITLE
    title.alignment = ALIGN_LEFT
    ws.row_dimensions[1].height = 26

    if not df_export.empty:
        _write_dataframe(ws, df_export, start_row=3, header_fill=FILL_HEADER_MID)
    else:
        ws["A3"].value = "Sem dados disponíveis."

    _auto_column_width(ws)
    ws.freeze_panes = "A4"


# ─────────────────────────────────────────────────────────────────────────────
# Função pública principal
# ─────────────────────────────────────────────────────────────────────────────

def generate_excel_report(
    report_data: dict[str, "pd.DataFrame"],
    output_path: str | None = None,
) -> str:
    """
    Gera o relatório executivo `.xlsx` completo.

    Args:
        report_data  : Dicionário retornado por `analytics.full_report()`.
                       Chaves esperadas: 'raw', 'por_hora', 'por_turno', 'kpis'.
        output_path  : Caminho de saída do arquivo. Se None, usa o diretório
                       definido em `app_config.reports_dir`.

    Returns:
        Caminho absoluto do arquivo gerado.
    """
    os.makedirs(app_config.reports_dir, exist_ok=True)

    if output_path is None:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = os.path.join(
            app_config.reports_dir,
            f"edgebench_relatorio_{timestamp}.xlsx",
        )

    logger.info("Gerando relatório Excel → %s", output_path)

    wb = Workbook()

    # Aba 1: KPI Cards (planilha ativa padrão)
    _build_kpi_sheet(wb, report_data.get("kpis", pd.DataFrame()))

    # Aba 2: Por Hora
    _build_hourly_sheet(wb, report_data.get("por_hora", pd.DataFrame()))

    # Aba 3: Por Turno
    _build_shift_sheet(wb, report_data.get("por_turno", pd.DataFrame()))

    # Aba 4: Dados Brutos
    _build_raw_sheet(wb, report_data.get("raw", pd.DataFrame()))

    wb.save(output_path)
    logger.info("Relatório salvo com sucesso: %s", output_path)
    return os.path.abspath(output_path)
