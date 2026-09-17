"""
Sincroniza os relatórios com o Google Drive/Sheets.
Faz o login, cria a pasta se não existir e envia o arquivo do Excel para a nuvem.
"""

import logging
import os
from typing import Any, Dict, List, Optional
# pyrefly: ignore [missing-import]
from google.oauth2.credentials import Credentials
# pyrefly: ignore [missing-import]
from google.auth.transport.requests import Request
# pyrefly: ignore [missing-import]
from googleapiclient.discovery import build
# pyrefly: ignore [missing-import]
from googleapiclient.http import MediaFileUpload

import sys

APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from settings.config import google_config

logger = logging.getLogger(__name__)

SCOPES = ['https://www.googleapis.com/auth/drive']


def _resolve_token_path() -> str:
    """Descobre onde está o arquivo de login (token.json)."""
    # 1. Variável de configuração explícita
    if google_config.token_path and os.path.exists(google_config.token_path):
        return google_config.token_path

    # 2. Caminhos comuns
    candidates = [
        "/app/token.json",
        os.path.join(APP_DIR, "token.json"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "token.json"),
        "token.json",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c

    return google_config.token_path or os.path.join(APP_DIR, "token.json")


def get_drive_service():
    """Faz o login no Google Drive usando o token."""
    token_path = _resolve_token_path()

    if not os.path.exists(token_path) or os.path.getsize(token_path) <= 4:
        logger.warning(
            "Arquivo de token inválido ou não encontrado em '%s'. Execute 'python auth_google.py' para autenticar.",
            token_path,
        )
        return None

    try:
        creds = Credentials.from_authorized_user_file(token_path, SCOPES)

        # Se o login venceu, pega um novo
        if creds and creds.expired and creds.refresh_token:
            logger.info("Renovando token OAuth expirado do Google Drive...")
            creds.refresh(Request())
            with open(token_path, 'w', encoding='utf-8') as token_file:
                token_file.write(creds.to_json())
            logger.info("Token renovado com sucesso.")

        return build('drive', 'v3', credentials=creds)
    except Exception as exc:
        logger.error("Falha na autenticação OAuth Google Drive: %s", exc)
        return None


def get_or_create_folder(service, folder_name: str = "EdgeBench_Relatorios") -> str:
    """Procura a pasta de relatórios. Se não existir, cria uma nova."""
    try:
        query = f"mimeType='application/vnd.google-apps.folder' and name='{folder_name}' and trashed=false"
        results = service.files().list(q=query, fields="files(id, name)").execute()
        folders = results.get('files', [])
        if folders:
            return folders[0]['id']

        # Cria nova pasta
        metadata = {
            'name': folder_name,
            'mimeType': 'application/vnd.google-apps.folder',
        }
        folder = service.files().create(body=metadata, fields='id').execute()
        folder_id = folder.get('id')
        logger.info("Pasta '%s' criada no Google Drive com ID: %s", folder_name, folder_id)
        return folder_id
    except Exception as exc:
        logger.error("Erro ao obter/criar pasta '%s' no Drive: %s", folder_name, exc)
        raise


def find_file_in_folder(service, filename: str, folder_id: str) -> Optional[str]:
    """Tenta encontrar uma planilha já existente na pasta."""
    query = f"'{folder_id}' in parents and name='{filename}' and trashed=false"
    try:
        results = service.files().list(q=query, fields="files(id, name)").execute()
        files = results.get('files', [])
        if files:
            return files[0]['id']
    except Exception as exc:
        logger.error("Erro ao buscar arquivo '%s': %s", filename, exc)
    return None


def upload_to_sheets(
    excel_filepath: str,
    sheet_name: str = "EdgeBench_Relatorio",
    convert_to_sheets: bool = True,
    update_if_exists: bool = False,
) -> Optional[Dict[str, Any]]:
    """Envia o arquivo do Excel pro Google Drive e, se quiser, transforma em Google Sheets nativo."""
    service = get_drive_service()
    if not service:
        logger.error("Serviço do Google Drive indisponível. Upload cancelado.")
        return None

    if not os.path.exists(excel_filepath):
        logger.error("Arquivo local não encontrado para upload: %s", excel_filepath)
        return None

    # Descobre ou cria a pasta onde o arquivo vai ficar
    folder_id = google_config.folder_id
    if not folder_id:
        folder_id = get_or_create_folder(service, "EdgeBench_Relatorios")

    clean_name = sheet_name.replace(".xlsx", "")

    try:
        file_id = None
        if update_if_exists:
            file_id = find_file_in_folder(service, clean_name, folder_id)

        file_metadata: Dict[str, Any] = {'name': clean_name}
        if convert_to_sheets:
            # Faz o Google transformar o arquivo em uma planilha editável na nuvem
            file_metadata['mimeType'] = 'application/vnd.google-apps.spreadsheet'

        media = MediaFileUpload(
            excel_filepath,
            mimetype='application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
            resumable=True,
        )

        if file_id:
            logger.info("Atualizando planilha existente no Google Drive (ID: %s)...", file_id)
            file = service.files().update(
                fileId=file_id,
                body=file_metadata,
                media_body=media,
                fields='id, name, webViewLink, createdTime, modifiedTime',
            ).execute()
        else:
            logger.info("Criando nova planilha no Google Drive (Pasta: %s)...", folder_id)
            file_metadata['parents'] = [folder_id]
            file = service.files().create(
                body=file_metadata,
                media_body=media,
                fields='id, name, webViewLink, createdTime, modifiedTime',
            ).execute()

        web_link = file.get('webViewLink')
        logger.info("Upload concluído com sucesso! Link: %s", web_link)

        return {
            "file_id": file.get("id"),
            "name": file.get("name"),
            "web_view_link": web_link,
            "created_time": file.get("createdTime"),
            "modified_time": file.get("modifiedTime"),
        }

    except Exception as exc:
        logger.error("Erro durante upload para o Google Drive: %s", exc, exc_info=True)
        return None


def list_drive_reports(folder_id: Optional[str] = None) -> List[Dict[str, Any]]:
    """Traz a lista de todas as planilhas que já estão no Google Drive."""
    service = get_drive_service()
    if not service:
        return []

    target_folder = folder_id or google_config.folder_id
    if not target_folder:
        try:
            target_folder = get_or_create_folder(service)
        except Exception:
            return []

    try:
        query = f"'{target_folder}' in parents and trashed=false"
        results = service.files().list(
            q=query,
            fields="files(id, name, webViewLink, mimeType, createdTime, modifiedTime, size)",
            orderBy="modifiedTime desc",
        ).execute()
        return results.get("files", [])
    except Exception as exc:
        logger.error("Erro ao listar arquivos do Google Drive: %s", exc)
        return []