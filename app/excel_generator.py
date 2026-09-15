"""
excel_generator.py — Gerador de relatório executivo `.xlsx` via OpenPyXL.

Arquitetura do Relatório Executivo (Padrão Indústria 4.0 / PCP):
    Aba 1 "KPI Cards"      : Painel Executivo com KPIs Globais da Linha + Cartões Detalhados por Bancada.
    Aba 2 "Por Hora"       : Matriz Cruzada (Bancada x Hora) com Totais de Linha/Bancada + Gráfico de Barras.
    Aba 3 "Por Turno"      : Acompanhamento de Turnos com Eficiência (%) e Totalizador Consolidado.
    Aba 4 "Ociosidade RN06": Auditoria de Paradas Operacionais Não Programadas (>15 minutos sem peça).
    Aba 5 "Dados Brutos"   : Registros completos com formatação de data/hora amigável.
"""

import logging
import os
from datetime import datetime, timezone
from typing import Any, Dict, Optional

import pandas as pd
from openpyxl import Workbook
from openpyxl.chart import BarChart, Reference
from openpyxl.styles import (
    Alignment,
    Border,
    Font,
    PatternFill,
    Side,
)
from openpyxl.utils import get_column_letter

from config import app_config

logger = logging.getLogger(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Paleta de Estilos Corporativa (PCP / Indústria 4.0)
# ─────────────────────────────────────────────────────────────────────────────

C_NAVY_DARK     = "1F4E78"   # Azul Escuro Executivo (Cabeçalhos principais)
C_BLUE_MID      = "2E75B6"   # Azul Médio (Sub-cabeçalhos e cartões)
C_BLUE_LIGHT    = "D9E1F2"   # Azul Suave (Totais e destaques de linha)
C_ZEBRA_EVEN    = "F2F5F9"   # Cinza-azulado muito claro para linhas pares
C_ZEBRA_ODD     = "FFFFFF"   # Branco para linhas ímpares
C_BORDER        = "BDD7EE"   # Bordas sutis
C_BORDER_THICK  = "1F4E78"   # Bordas de fechamento
C_CARD_BG       = "F2F5F9"   # Fundo de caixas de KPI
C_TEXT_LIGHT    = "FFFFFF"   # Texto sobre fundos escuros
C_TEXT_DARK     = "1A1A2E"   # Texto padrão escuro
C_TEXT_MUTED    = "595959"   # Texto secundário/rótulos
C_ALERT_ORANGE  = "ED7D31"   # Alerta de parada / baixa eficiência
C_ALERT_BG      = "FCE4D6"   # Fundo suave para alertas
C_GREEN_DARK    = "385723"   # Indicador positivo
C_GREEN_BG      = "E2EFDA"   # Fundo suave positivo

FONT_TITLE      = Font(name="Calibri", bold=True, size=16, color=C_NAVY_DARK)
FONT_SUBTITLE   = Font(name="Calibri", italic=True, size=9, color="7F7F7F")
FONT_SECTION    = Font(name="Calibri", bold=True, size=11, color=C_NAVY_DARK)
FONT_HEADER     = Font(name="Calibri", bold=True, size=10, color=C_TEXT_LIGHT)
FONT_TOTAL      = Font(name="Calibri", bold=True, size=10, color=C_NAVY_DARK)
FONT_BODY       = Font(name="Calibri", size=9, color=C_TEXT_DARK)
FONT_BODY_B     = Font(name="Calibri", bold=True, size=9, color=C_TEXT_DARK)
FONT_KPI_NUM    = Font(name="Calibri", bold=True, size=20, color=C_NAVY_DARK)
FONT_KPI_LBL    = Font(name="Calibri", bold=True, size=8, color=C_TEXT_MUTED)

FILL_HEADER     = PatternFill("solid", fgColor=C_NAVY_DARK)
FILL_SUBHEADER  = PatternFill("solid", fgColor=C_BLUE_MID)
FILL_TOTAL      = PatternFill("solid", fgColor=C_BLUE_LIGHT)
FILL_ZEBRA_EVEN = PatternFill("solid", fgColor=C_ZEBRA_EVEN)
FILL_ZEBRA_ODD  = PatternFill("solid", fgColor=C_ZEBRA_ODD)
FILL_CARD_BG    = PatternFill("solid", fgColor=C_CARD_BG)
FILL_ALERT_BG   = PatternFill("solid", fgColor=C_ALERT_BG)
FILL_GREEN_BG   = PatternFill("solid", fgColor=C_GREEN_BG)

ALIGN_CENTER    = Alignment(horizontal="center", vertical="center", wrap_text=True)
ALIGN_LEFT      = Alignment(horizontal="left",   vertical="center", wrap_text=True)
ALIGN_RIGHT     = Alignment(horizontal="right",  vertical="center")

_thin_side      = Side(style="thin", color=C_BORDER)
_thick_side     = Side(style="medium", color=C_BORDER_THICK)
_double_side    = Side(style="double", color=C_BORDER_THICK)

BORDER_ALL      = Border(left=_thin_side, right=_thin_side, top=_thin_side, bottom=_thin_side)
BORDER_HEADER   = Border(left=_thin_side, right=_thin_side, top=_thick_side, bottom=_thick_side)
BORDER_TOTAL    = Border(left=_thin_side, right=_thin_side, top=_thin_side, bottom=_double_side)


# ─────────────────────────────────────────────────────────────────────────────
# Funções Auxiliares
# ─────────────────────────────────────────────────────────────────────────────

def _auto_column_width(ws, min_width: int = 12, max_width: int = 50) -> None:
    """Ajusta automaticamente a largura das colunas mantendo espaçamento agradável."""
    for col_cells in ws.columns:
        col_letter = get_column_letter(col_cells[0].column)
        max_len = 0
        for cell in col_cells:
            if cell.value is not None:
                cell_len = len(str(cell.value))
                max_len = max(max_len, cell_len)
        ws.column_dimensions[col_letter].width = max(min_width, min(max_len + 4, max_width))


# ─────────────────────────────────────────────────────────────────────────────
# Aba 1: KPI Cards (Visão Geral da Fábrica + Cartões por Posto)
# ─────────────────────────────────────────────────────────────────────────────

def _build_kpi_sheet(wb: Workbook, global_kpis: Dict[str, Any], kpis_df: pd.DataFrame) -> None:
    ws = wb.active
    ws.title = "KPI Cards"
    ws.sheet_view.showGridLines = False

    # 1. Título Executivo
    ws["B2"] = "EdgeBench — Painel de Produção"
    ws["B2"].font = FONT_TITLE
    ws["B3"] = f"Relatório gerado em: {datetime.now(tz=timezone.utc).strftime('%d/%m/%Y às %H:%M')} UTC"
    ws["B3"].font = FONT_SUBTITLE
    ws.row_dimensions[2].height = 24
    ws.row_dimensions[3].height = 16

    # 2. Caixas de KPIs Globais da Linha (Linha 5 e 6)
    cards = [
        ("PRODUÇÃO TOTAL", f"{global_kpis.get('total_pecas', 0):,}".replace(",", ".") + " peças", "B", "C"),
        ("BANCADAS ATIVAS", f"{global_kpis.get('bancadas_ativas', 0)} bancadas", "D", "E"),
        ("RITMO MÉDIO", f"{global_kpis.get('taxa_pecas_hora', 0.0)} peças/h", "F", "G"),
        ("PARADAS (>15 MIN)", f"{global_kpis.get('total_paradas_rn06', 0)} detectadas", "H", "I"),
    ]

    for title, value, col_start, col_end in cards:
        # Mescla título (linha 5)
        ws.merge_cells(f"{col_start}5:{col_end}5")
        t_cell = ws[f"{col_start}5"]
        t_cell.value = title
        t_cell.font = FONT_KPI_LBL
        t_cell.fill = FILL_CARD_BG
        t_cell.alignment = ALIGN_CENTER

        # Mescla valor (linha 6)
        ws.merge_cells(f"{col_start}6:{col_end}6")
        v_cell = ws[f"{col_start}6"]
        v_cell.value = value
        v_cell.font = FONT_KPI_NUM
        v_cell.fill = FILL_CARD_BG
        v_cell.alignment = ALIGN_CENTER

        # Bordas
        for r in [5, 6]:
            for c_letter in [col_start, col_end]:
                ws[f"{c_letter}{r}"].border = BORDER_ALL

    ws.row_dimensions[5].height = 18
    ws.row_dimensions[6].height = 30

    # 3. Detalhamento Individual por Bancada
    ws["B8"] = "DESEMPENHO POR BANCADA"
    ws["B8"].font = FONT_SECTION
    ws.row_dimensions[8].height = 20

    current_row = 10
    if kpis_df.empty:
        ws[f"B{current_row}"] = "Nenhuma atividade registrada no período selecionado."
        ws[f"B{current_row}"].font = FONT_BODY
        return

    cols = ["B", "C", "D", "E", "F", "G", "H", "I"]

    for _, row in kpis_df.iterrows():
        bancada = row["bancada_id"]
        total_pecas = int(row.get("total_pecas", 0))
        max_cont = int(row.get("max_contagem", 0))
        uptime = f"{float(row.get('uptime_pct', 100.0)):.1f}%"
        media_delta = f"{float(row.get('media_delta', 1.0)):.1f}"
        raw_p = row.get("paradas_rn06", 0)
        paradas_rn06 = int(raw_p) if pd.notna(raw_p) and str(raw_p).isdigit() else 0
        tempo_ocioso = f"{float(row.get('tempo_ocioso_min', 0.0)):.1f} min"
        
        p_raw = row.get("primeiro_registro")
        u_raw = row.get("ultimo_registro")
        try:
            primeiro = pd.to_datetime(p_raw).strftime("%d/%m/%Y %H:%M:%S") if pd.notna(p_raw) else "—"
        except Exception:
            primeiro = str(p_raw)[:19] if pd.notna(p_raw) else "—"
        try:
            ultimo = pd.to_datetime(u_raw).strftime("%d/%m/%Y %H:%M:%S") if pd.notna(u_raw) else "—"
        except Exception:
            ultimo = str(u_raw)[:19] if pd.notna(u_raw) else "—"

        # Header da Bancada (Linha R)
        ws.merge_cells(f"B{current_row}:I{current_row}")
        hdr = ws[f"B{current_row}"]
        hdr.value = f"  BANCADA: {bancada}"
        hdr.font = Font(name="Calibri", bold=True, size=11, color=C_TEXT_LIGHT)
        hdr.fill = FILL_SUBHEADER
        hdr.alignment = ALIGN_LEFT
        for c in cols:
            ws[f"{c}{current_row}"].border = BORDER_HEADER
        ws.row_dimensions[current_row].height = 20
        current_row += 1

        # Rótulos (Linha R+1)
        labels = [
            ("B", "TOTAL PEÇAS"),
            ("C", "CONTAGEM MÁXIMA"),
            ("D", "DISPONIBILIDADE"),
            ("E", "MÉDIA/LEITURA"),
            ("F", "PARADAS (>15 MIN)"),
            ("G", "TEMPO PARADO"),
            ("H", "PRIMEIRO REGISTRO"),
            ("I", "ÚLTIMO REGISTRO"),
        ]
        for col_letter, lbl in labels:
            c_cell = ws[f"{col_letter}{current_row}"]
            c_cell.value = lbl
            c_cell.font = FONT_KPI_LBL
            c_cell.fill = FILL_CARD_BG
            c_cell.alignment = ALIGN_CENTER
            c_cell.border = BORDER_ALL
        ws.row_dimensions[current_row].height = 15
        current_row += 1

        # Valores (Linha R+2)
        values = [
            ("B", total_pecas, True),
            ("C", max_cont, True),
            ("D", uptime, False),
            ("E", media_delta, False),
            ("F", paradas_rn06, True),
            ("G", tempo_ocioso, False),
            ("H", primeiro, False),
            ("I", ultimo, False),
        ]
        for col_letter, val, is_bold in values:
            v_cell = ws[f"{col_letter}{current_row}"]
            v_cell.value = val
            v_cell.font = FONT_BODY_B if is_bold else FONT_BODY
            v_cell.fill = FILL_ZEBRA_ODD
            v_cell.alignment = ALIGN_CENTER
            v_cell.border = BORDER_ALL
            if col_letter == "F" and int(val) > 0:
                v_cell.fill = FILL_ALERT_BG
                v_cell.font = Font(name="Calibri", bold=True, color=C_ALERT_ORANGE)

        ws.row_dimensions[current_row].height = 20
        current_row += 2  # Espaçamento para o próximo cartão

    # Larguras equilibradas
    ws.column_dimensions["A"].width = 3
    for c in ["B", "C", "D", "E", "F", "G"]:
        ws.column_dimensions[c].width = 17
    for c in ["H", "I"]:
        ws.column_dimensions[c].width = 22


# ─────────────────────────────────────────────────────────────────────────────
# Aba 2: Matriz Cruzada Por Hora (Bancada x Hora) + Gráfico de Barras
# ─────────────────────────────────────────────────────────────────────────────

def _build_hourly_sheet(wb: Workbook, pivot_df: pd.DataFrame, flat_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Por Hora")
    ws.sheet_view.showGridLines = False

    # Título
    ws["A1"] = "Produção Horária por Bancada"
    ws["A1"].font = FONT_TITLE
    ws["A2"] = "Distribuição da quantidade de peças produzidas por hora em cada bancada"
    ws["A2"].font = FONT_SUBTITLE
    ws.row_dimensions[1].height = 22
    ws.row_dimensions[2].height = 14

    if pivot_df.empty:
        ws["A4"] = "Sem registros de produção no período selecionado."
        ws["A4"].font = FONT_BODY
        return

    start_row = 4
    n_cols = len(pivot_df.columns)

    # 1. Cabeçalho da Matriz
    for col_idx, col_name in enumerate(pivot_df.columns, start=1):
        cell = ws.cell(row=start_row, column=col_idx)
        header_text = str(col_name).replace("_", " ").upper()
        if header_text == "BANCADA ID":
            header_text = "BANCADA"
        cell.value = header_text
        cell.font = FONT_HEADER
        cell.fill = FILL_HEADER
        cell.alignment = ALIGN_CENTER
        cell.border = BORDER_HEADER

    ws.row_dimensions[start_row].height = 22

    # 2. Linhas de Dados da Matriz
    current_row = start_row + 1
    total_col_idx = n_cols  # Última coluna é TOTAL BANCADA

    for _, row in pivot_df.iterrows():
        is_total_row = (str(row.iloc[0]) == "TOTAL HORA")
        row_fill = FILL_TOTAL if is_total_row else (FILL_ZEBRA_EVEN if current_row % 2 == 0 else FILL_ZEBRA_ODD)
        row_font = FONT_TOTAL if is_total_row else FONT_BODY

        for col_idx in range(1, n_cols + 1):
            val = row.iloc[col_idx - 1]
            cell = ws.cell(row=current_row, column=col_idx)
            cell.value = val
            cell.font = row_font
            cell.fill = row_fill
            cell.border = BORDER_TOTAL if is_total_row else BORDER_ALL

            if col_idx == 1:
                cell.alignment = ALIGN_CENTER
            elif col_idx == total_col_idx:
                cell.alignment = ALIGN_RIGHT
                cell.font = FONT_BODY_B
                if not is_total_row:
                    cell.fill = FILL_TOTAL
            else:
                cell.alignment = ALIGN_RIGHT

        ws.row_dimensions[current_row].height = 18
        current_row += 1

    matrix_last_row = current_row - 1
    _auto_column_width(ws, min_width=12)

    # 3. Gráfico de Barras Nativo do Excel (Produção por Hora por Posto)
    try:
        if len(pivot_df) > 1 and n_cols > 2:
            chart = BarChart()
            chart.type = "col"
            chart.style = 10
            chart.title = "Distribuição da Produção Horária por Bancada"
            chart.y_axis.title = "Peças Produzidas"
            chart.x_axis.title = "Horário de Produção"
            chart.width = 20
            chart.height = 11

            # Dados das bancadas: linhas start_row+1 até matrix_last_row-1 (exclui cabeçalho e TOTAL GERAL)
            # min_col=1 inclui o nome da bancada para titles_from_data=True
            data_ref = Reference(
                ws,
                min_col=1,
                min_row=start_row + 1,
                max_col=n_cols - 1,
                max_row=matrix_last_row - 1,
            )
            # Categorias: Horas (linha de cabeçalho start_row, colunas 2 até n_cols - 1)
            cats_ref = Reference(
                ws,
                min_col=2,
                min_row=start_row,
                max_col=n_cols - 1,
                max_row=start_row,
            )

            chart.add_data(data_ref, titles_from_data=True, from_rows=True)
            chart.set_categories(cats_ref)

            # Posiciona o gráfico logo abaixo da matriz
            chart_cell = f"A{matrix_last_row + 3}"
            ws.add_chart(chart, chart_cell)
    except Exception as exc:
        logger.warning("Não foi possível gerar gráfico OpenPyXL: %s", exc)

    ws.freeze_panes = "B5"


# ─────────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────────
# Aba 3: Produção por Turno
# ─────────────────────────────────────────────────────────────────────────────

def _build_shift_sheet(wb: Workbook, turno_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Por Turno")
    ws.sheet_view.showGridLines = False

    ws["A1"] = "Produção por Turno"
    ws["A1"].font = FONT_TITLE
    ws["A2"] = "Turno 1 (06h–14h) | Turno 2 (14h–22h) | Turno 3 (22h–06h)"
    ws["A2"].font = FONT_SUBTITLE
    ws.row_dimensions[1].height = 22
    ws.row_dimensions[2].height = 14

    if turno_df.empty:
        ws["A4"] = "Sem registros de turnos no período."
        ws["A4"].font = FONT_BODY
        return

    headers = [
        ("bancada_id", "Bancada"),
        ("data", "Data"),
        ("turno", "Turno"),
        ("total_pecas", "Total de Peças"),
        ("mensagens", "Leituras"),
        ("contagem_final", "Contador Final"),
        ("status_mais_frequente", "Status"),
        ("meta_turno", "Meta"),
        ("eficiencia_decimal", "Eficiência (%)"),
    ]

    start_row = 4
    for c_idx, (_, h_text) in enumerate(headers, start=1):
        cell = ws.cell(row=start_row, column=c_idx, value=h_text)
        cell.font = FONT_HEADER
        cell.fill = FILL_HEADER
        cell.alignment = ALIGN_CENTER
        cell.border = BORDER_HEADER

    ws.row_dimensions[start_row].height = 20

    current_row = start_row + 1
    for _, row in turno_df.iterrows():
        fill = FILL_ZEBRA_EVEN if current_row % 2 == 0 else FILL_ZEBRA_ODD
        for c_idx, (col_key, _) in enumerate(headers, start=1):
            val = row.get(col_key, "")
            
            # Formata carimbo de data para DD/MM/AAAA
            if col_key == "data" and val:
                try:
                    val = pd.to_datetime(val).strftime("%d/%m/%Y")
                except Exception:
                    val = str(val)[:10]

            cell = ws.cell(row=current_row, column=c_idx, value=val)
            cell.font = FONT_BODY
            cell.fill = fill
            cell.border = BORDER_ALL

            if col_key in ["total_pecas", "mensagens", "contagem_final", "meta_turno"]:
                cell.alignment = ALIGN_RIGHT
                cell.number_format = "#,##0"
            elif col_key == "eficiencia_decimal":
                cell.alignment = ALIGN_RIGHT
                cell.number_format = "0.0%"
                cell.font = FONT_BODY_B
                # Alerta suave se eficiência for inferior a 80% da meta
                if isinstance(val, (int, float)) and val < 0.8:
                    cell.fill = FILL_ALERT_BG
                    cell.font = Font(name="Calibri", bold=True, size=9, color=C_ALERT_ORANGE)
            else:
                cell.alignment = ALIGN_CENTER

        ws.row_dimensions[current_row].height = 18
        current_row += 1

    # Linha de Totalização
    ws.merge_cells(f"A{current_row}:C{current_row}")
    tot_label = ws[f"A{current_row}"]
    tot_label.value = "TOTAL CONSOLIDADO"
    tot_label.font = FONT_TOTAL
    tot_label.fill = FILL_TOTAL
    tot_label.alignment = ALIGN_CENTER

    tot_pecas = turno_df["total_pecas"].sum()
    tot_msgs = turno_df["mensagens"].sum()
    tot_meta = len(turno_df) * 3200
    avg_eff = tot_pecas / tot_meta if tot_meta > 0 else 0.0

    ws.cell(row=current_row, column=4, value=tot_pecas).number_format = "#,##0"
    ws.cell(row=current_row, column=5, value=tot_msgs).number_format = "#,##0"
    ws.cell(row=current_row, column=8, value=tot_meta).number_format = "#,##0"
    eff_total_cell = ws.cell(row=current_row, column=9, value=avg_eff)
    eff_total_cell.number_format = "0.0%"

    for c in range(1, len(headers) + 1):
        c_cell = ws.cell(row=current_row, column=c)
        c_cell.fill = FILL_TOTAL
        c_cell.font = FONT_TOTAL
        c_cell.border = BORDER_TOTAL
        if c in [4, 5, 8, 9]:
            c_cell.alignment = ALIGN_RIGHT

    ws.row_dimensions[current_row].height = 20
    _auto_column_width(ws, min_width=14)
    ws.freeze_panes = "A5"


# ─────────────────────────────────────────────────────────────────────────────
# Aba 4: Dados Brutos
# ─────────────────────────────────────────────────────────────────────────────

def _build_raw_sheet(wb: Workbook, raw_df: pd.DataFrame) -> None:
    ws = wb.create_sheet(title="Dados Brutos")
    ws.sheet_view.showGridLines = False

    ws["A1"] = "Registros de Produção"
    ws["A1"].font = FONT_TITLE
    ws["A2"] = "Histórico detalhado de leituras e carimbos de data/hora registrados pelos postos"
    ws["A2"].font = FONT_SUBTITLE
    ws.row_dimensions[1].height = 22
    ws.row_dimensions[2].height = 14

    if raw_df.empty:
        ws["A4"] = "Sem registros brutos."
        ws["A4"].font = FONT_BODY
        return

    cols = ["id", "bancada_id", "contagem_total", "delta_pecas", "timestamp_esp", "timestamp_servidor", "status_bancada"]
    headers = ["ID", "Bancada", "Contador", "Peças Produzidas", "Data/Hora Envio", "Data/Hora Registro", "Status"]

    start_row = 4
    for c_idx, h_text in enumerate(headers, start=1):
        cell = ws.cell(row=start_row, column=c_idx, value=h_text)
        cell.font = FONT_HEADER
        cell.fill = FILL_HEADER
        cell.alignment = ALIGN_CENTER
        cell.border = BORDER_HEADER
    ws.row_dimensions[start_row].height = 20

    current_row = start_row + 1
    for _, row in raw_df.iterrows():
        fill = FILL_ZEBRA_EVEN if current_row % 2 == 0 else FILL_ZEBRA_ODD
        for c_idx, col_name in enumerate(cols, start=1):
            val = row.get(col_name, "")
            # Formata timestamps para o padrão legível brasileiro
            if col_name in ["timestamp_esp", "timestamp_servidor"] and pd.notnull(val):
                try:
                    val = pd.to_datetime(val).strftime("%d/%m/%Y %H:%M:%S")
                except Exception:
                    val = str(val)

            cell = ws.cell(row=current_row, column=c_idx, value=val)
            cell.font = FONT_BODY
            cell.fill = fill
            cell.border = BORDER_ALL

            if col_name in ["id", "contagem_total", "delta_pecas"]:
                cell.alignment = ALIGN_RIGHT
            elif col_name in ["bancada_id", "timestamp_esp", "timestamp_servidor", "status_bancada"]:
                cell.alignment = ALIGN_CENTER
            else:
                cell.alignment = ALIGN_LEFT

        ws.row_dimensions[current_row].height = 16
        current_row += 1

    _auto_column_width(ws, min_width=14)
    ws.freeze_panes = "A5"


# ─────────────────────────────────────────────────────────────────────────────
# Função Pública de Geração Executiva
# ─────────────────────────────────────────────────────────────────────────────

def generate_excel_report(
    report_data: Dict[str, Any],
    output_path: Optional[str] = None,
) -> str:
    """
    Gera o relatório executivo .xlsx com arquitetura visual corporativa limpa.
    """
    os.makedirs(app_config.reports_dir, exist_ok=True)

    if output_path is None:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = os.path.join(
            app_config.reports_dir,
            f"edgebench_relatorio_{timestamp}.xlsx",
        )

    logger.info("Gerando relatório executivo de produção → %s", output_path)

    wb = Workbook()

    # 1. Aba KPI Cards
    _build_kpi_sheet(
        wb,
        report_data.get("global_kpis", {}),
        report_data.get("kpis", pd.DataFrame()),
    )

    # 2. Aba Por Hora (Matriz Cruzada + Gráfico)
    _build_hourly_sheet(
        wb,
        report_data.get("por_hora_pivot", pd.DataFrame()),
        report_data.get("por_hora", pd.DataFrame()),
    )

    # 3. Aba Por Turno
    _build_shift_sheet(
        wb,
        report_data.get("por_turno", pd.DataFrame()),
    )

    # 4. Aba Dados Brutos
    _build_raw_sheet(
        wb,
        report_data.get("raw", pd.DataFrame()),
    )

    wb.save(output_path)
    logger.info("Relatório executivo salvo com sucesso: %s", output_path)
    return os.path.abspath(output_path)
