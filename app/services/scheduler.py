"""
Agendador de tarefas em segundo plano para backup e sincronização com Google Drive/Sheets.
"""

import logging
import os
import shutil
import sys
from datetime import datetime
from apscheduler.schedulers.background import BackgroundScheduler
from apscheduler.triggers.cron import CronTrigger

APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from settings.config import app_config
from services.excel_generator import generate_excel_report
from external_integrations.google_sheets_sync import upload_to_sheets

logger = logging.getLogger(__name__)


def perform_backup_and_sync():
    """Executa o backup periódico: gera o Excel, salva localmente e atualiza o Google Sheets e Drive."""
    logger.info("Iniciando rotina periódica de backup e sincronização com Google Drive...")

    from services.analytics import full_report
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
