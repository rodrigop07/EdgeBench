"""
scheduler.py — Agendador de tarefas em segundo plano.

Gerencia backups diários das planilhas e a sincronização com o Google Sheets/Drive.
"""

import logging
import os
import shutil
from datetime import datetime
from apscheduler.schedulers.background import BackgroundScheduler
from apscheduler.triggers.cron import CronTrigger

from config import app_config
from excel_generator import generate_excel_report
from google_sheets_sync import upload_to_sheets

logger = logging.getLogger(__name__)


def perform_backup_and_sync():
    """
    Rotina periódica de consolidação:
    1. Gera a planilha Excel com todos os dados acumulados.
    2. Salva uma cópia local de backup histórico.
    3. Atualiza a planilha Mestre ('EdgeBench_Painel_Producao') no Google Sheets.
    4. Grava o fechamento histórico ('EdgeBench_Fechamento_<timestamp>') no Google Drive.
    """
    logger.info("Iniciando rotina periódica de backup e sincronização com Google Drive...")

    from analytics import full_report
    data = full_report()
    excel_path = generate_excel_report(report_data=data)
    if not excel_path:
        logger.error("Falha ao gerar relatório para backup.")
        return

    # Backup local no host
    backups_dir = os.path.join(app_config.reports_dir, "backups")
    os.makedirs(backups_dir, exist_ok=True)

    timestamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    backup_filename = f"EdgeBench_Fechamento_{timestamp}"
    backup_path = os.path.join(backups_dir, f"{backup_filename}.xlsx")

    try:
        shutil.copy2(excel_path, backup_path)
        logger.info("Backup local salvo em: %s", backup_path)
    except Exception as exc:
        logger.error("Erro ao salvar backup local: %s", exc)

    # 1. Atualiza a planilha MESTRE (link fixo de acompanhamento online)
    logger.info("Atualizando planilha Mestre no Google Sheets...")
    upload_to_sheets(
        excel_filepath=excel_path,
        sheet_name="EdgeBench_Painel_Producao",
        convert_to_sheets=True,
        update_if_exists=True,
    )

    # 2. Registra o fechamento histórico imutável
    logger.info("Salvando fechamento histórico no Google Drive...")
    upload_to_sheets(
        excel_filepath=excel_path,
        sheet_name=backup_filename,
        convert_to_sheets=True,
        update_if_exists=False,
    )

    logger.info("Rotina de backup e sincronização com Google Drive finalizada.")


def start_scheduler():
    """Inicia o agendador em segundo plano."""
    scheduler = BackgroundScheduler()

    # Agenda a tarefa para rodar nos fechamentos de turno (06:01, 14:01, 22:01)
    scheduler.add_job(
        perform_backup_and_sync,
        CronTrigger(hour='6,14,22', minute='1'),
        id='shift_backup_sheets_job',
        name='Backup e Sincronizacao Google Sheets (Fechamento de Turnos)',
        replace_existing=True,
    )

    scheduler.start()
    logger.info("Scheduler iniciado. Tarefas de fechamento agendadas para 06:01, 14:01 e 22:01.")
    return scheduler
