"""
scheduler.py — Agendador de tarefas em segundo plano.

Gerencia backups diários das planilhas e a sincronização com o Google Sheets.
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
    1. Gera a planilha Excel com dados atualizados.
    2. Faz backup local da planilha.
    3. Sincroniza com o Google Sheets.
    """
    logger.info("Iniciando rotina de backup e sincronização do Sheets...")
    
    # 1. Gera o relatório
    # Para o backup diário geral, não passaremos filtros de bancada, pegará tudo
    from analytics import full_report
    data = full_report()
    excel_path = generate_excel_report(report_data=data)
    if not excel_path:
        logger.error("Falha ao gerar relatório para backup.")
        return

    # 2. Faz o backup local
    backups_dir = os.path.join(app_config.reports_dir, "backups")
    os.makedirs(backups_dir, exist_ok=True)
    
    # Formato solicitado: mes, dia e ano no nome do relatorio
    timestamp = datetime.now().strftime("%m-%d-%Y_%H%M%S")
    backup_filename = f"EdgeBench_Backup_{timestamp}"
    backup_path = os.path.join(backups_dir, f"{backup_filename}.xlsx")
    
    try:
        shutil.copy2(excel_path, backup_path)
        logger.info(f"Backup local salvo em: {backup_path}")
    except Exception as e:
        logger.error(f"Erro ao salvar backup local: {e}")

    # 3. Sincroniza com o Google Sheets, criando um novo arquivo com o nome de backup
    upload_to_sheets(excel_path, sheet_name=backup_filename)
    
    logger.info("Rotina de backup e sincronização finalizada.")


def start_scheduler():
    """Inicia o agendador em segundo plano."""
    scheduler = BackgroundScheduler()

    # Agenda a tarefa para rodar duas vezes ao dia: às 12:00 e às 23:50
    # O usuário pode ajustar os horários conforme necessário
    scheduler.add_job(
        perform_backup_and_sync,
        CronTrigger(hour='12,23', minute='0,50'),
        id='backup_sheets_job',
        name='Backup and Sync to Google Sheets',
        replace_existing=True
    )
    
    scheduler.start()
    logger.info("Scheduler iniciado. Tarefa de backup agendada para 12:00 e 23:50.")
    return scheduler
