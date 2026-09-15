"""
google_sheets_sync.py — Módulo para sincronização com Google Drive via OAuth 2.0.
"""

import json
import logging
import os
from google.oauth2.credentials import Credentials
from google.auth.transport.requests import Request
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload

from config import google_config

logger = logging.getLogger(__name__)

SCOPES = ['https://www.googleapis.com/auth/drive']
DEFAULT_TOKEN_PATH = '/app/token.json'


def get_drive_service():
    """Autentica com OAuth 2.0 utilizando token.json e renova se necessário."""
    token_path = google_config.credentials_path or DEFAULT_TOKEN_PATH

    if not os.path.exists(token_path):
        logger.error(f"Arquivo de token não encontrado em: {token_path}")
        return None

    try:
        creds = Credentials.from_authorized_user_file(token_path, SCOPES)

        # Renova o token automaticamente se estiver expirado
        if creds and creds.expired and creds.refresh_token:
            logger.info("Token expirado, renovando automaticamente...")
            creds.refresh(Request())
            with open(token_path, 'w') as token_file:
                token_file.write(creds.to_json())

        return build('drive', 'v3', credentials=creds)
    except Exception as e:
        logger.error(f"Falha ao autenticar com Google Drive OAuth: {e}")
        return None


def find_file_in_folder(service, filename: str, folder_id: str) -> str | None:
    """Procura um arquivo por nome dentro de uma pasta e retorna o ID, se existir."""
    query = f"'{folder_id}' in parents and name='{filename}' and trashed=false"
    try:
        results = service.files().list(q=query, fields="files(id, name)").execute()
        files = results.get('files', [])
        if files:
            return files[0]['id']
    except Exception as e:
        logger.error(f"Erro ao buscar arquivo '{filename}': {e}")
    return None


def upload_to_sheets(excel_filepath: str, sheet_name: str = "EdgeBench_Relatorio") -> bool:
    """Upload ou atualização de arquivo .xlsx no Google Drive."""
    service = get_drive_service()
    if not service:
        return False

    folder_id = google_config.folder_id
    if not folder_id:
        logger.error("GOOGLE_DRIVE_FOLDER_ID não configurado.")
        return False

    if not os.path.exists(excel_filepath):
        logger.error(f"Arquivo Excel não encontrado: {excel_filepath}")
        return False

    target_filename = f"{sheet_name}.xlsx"

    try:
        file_id = find_file_in_folder(service, target_filename, folder_id)
        file_metadata = {'name': target_filename}

        media = MediaFileUpload(
            excel_filepath,
            mimetype='application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
            resumable=True
        )

        if file_id:
            logger.info(f"Atualizando arquivo existente no Google Drive (ID: {file_id})...")
            file = service.files().update(
                fileId=file_id,
                body=file_metadata,
                media_body=media,
                fields='id, webViewLink'
            ).execute()
        else:
            logger.info("Criando novo arquivo no Google Drive...")
            file_metadata['parents'] = [folder_id]
            file = service.files().create(
                body=file_metadata,
                media_body=media,
                fields='id, webViewLink'
            ).execute()

        logger.info(f"Sucesso! Link do arquivo: {file.get('webViewLink')}")
        return True

    except Exception as e:
        logger.error(f"Erro ao fazer upload para o Google Drive: {e}")
        return False