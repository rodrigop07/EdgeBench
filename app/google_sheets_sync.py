"""
google_sheets_sync.py — Sincronização e upload de relatórios para o Google Drive / Sheets.

Funcionalidades:
    - Autenticação OAuth 2.0 resiliente com renovação automática de token.
    - Conversão automática de .xlsx para planilha nativa do Google Sheets (mimeType).
    - Criação automática da pasta 'EdgeBench_Relatorios' caso não esteja configurada.
    - Suporte a atualização de painel mestre ou arquivamento histórico versionado.
    - Listagem estruturada de relatórios existentes para futura interface web.
"""

import logging
import os
from typing import Any, Dict, List, Optional
from google.oauth2.credentials import Credentials
from google.auth.transport.requests import Request
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload

from config import google_config

logger = logging.getLogger(__name__)

SCOPES = ['https://www.googleapis.com/auth/drive']


def _resolve_token_path() -> str:
    """Identifica o caminho correto do token.json (dentro do container ou no host)."""
    # 1. Variável de configuração explícita
    if google_config.token_path and os.path.exists(google_config.token_path):
        return google_config.token_path

    # 2. Caminhos comuns
    candidates = [
        "/app/token.json",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "token.json"),
        "token.json",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c

    return google_config.token_path or "token.json"


def get_drive_service():
    """Autentica com OAuth 2.0 utilizando token.json e renova o token se necessário."""
    token_path = _resolve_token_path()

    if not os.path.exists(token_path) or os.path.getsize(token_path) <= 4:
        logger.warning(
            "Arquivo de token inválido ou não encontrado em '%s'. Execute 'python auth_google.py' para autenticar.",
            token_path,
        )
        return None

    try:
        creds = Credentials.from_authorized_user_file(token_path, SCOPES)

        # Renova o token automaticamente se estiver expirado
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
    """Localiza a pasta pelo nome ou cria uma nova na raiz do Google Drive."""
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
    """Procura um arquivo por nome dentro de uma pasta e retorna o ID, se existir."""
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
    """
    Realiza o upload de um arquivo .xlsx para o Google Drive com conversão para Google Sheets.

    Args:
        excel_filepath    : Caminho absoluto ou relativo do arquivo .xlsx local.
        sheet_name        : Nome de exibição da planilha no Google Drive.
        convert_to_sheets : Se True, converte para planilha editável nativa do Google Sheets.
        update_if_exists  : Se True, atualiza o arquivo se já existir com mesmo nome na pasta.

    Returns:
        Dicionário com metadados do arquivo criado/atualizado (id, name, webViewLink),
        ou None em caso de falha.
    """
    service = get_drive_service()
    if not service:
        logger.error("Serviço do Google Drive indisponível. Upload cancelado.")
        return None

    if not os.path.exists(excel_filepath):
        logger.error("Arquivo local não encontrado para upload: %s", excel_filepath)
        return None

    # Define a pasta destino: se GOOGLE_DRIVE_FOLDER_ID estiver configurado, usa; senão cria 'EdgeBench_Relatorios'
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
            # Esta propriedade instrui o Google Drive a converter o .xlsx em Google Sheets nativo
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
    """
    Lista os relatórios disponíveis na pasta do Google Drive.
    Útil para alimentação de painéis e páginas web de consulta.
    """
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