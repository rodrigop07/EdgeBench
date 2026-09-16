"""
auth_google.py — Script para autenticação inicial do Google Drive via OAuth 2.0.

Executa o fluxo de autorização no navegador da máquina, permitindo que o usuário
faça login com sua conta Google e aprove o acesso ao Drive. Ao concluir, salva
automaticamente o `token.json` na pasta app/, que será utilizado pelo backend
para sincronização contínua.

Uso:
    python auth_google.py
"""

import os
import sys
import json
# pyrefly: ignore [missing-import]
from google_auth_oauthlib.flow import InstalledAppFlow
# pyrefly: ignore [missing-import]
from googleapiclient.discovery import build

SCOPES = [
    'https://www.googleapis.com/auth/drive',
]

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CREDENTIALS_FILE = os.path.join(SCRIPT_DIR, 'credentials.json')
TOKEN_FILE = os.path.join(SCRIPT_DIR, 'token.json')


def authenticate():
    print("=" * 60)
    print("      EdgeBench — Autenticação Google Drive OAuth 2.0")
    print("=" * 60)

    if not os.path.exists(CREDENTIALS_FILE):
        print(f"\n[ERRO] Arquivo de credenciais não encontrado em:")
        print(f"       {CREDENTIALS_FILE}")
        print("\nPor favor, certifique-se de que o arquivo 'credentials.json' está na pasta 'app/'.")
        sys.exit(1)

    print(f"\n1. Carregando credenciais de: {CREDENTIALS_FILE}")
    flow = InstalledAppFlow.from_client_secrets_file(CREDENTIALS_FILE, SCOPES)

    print("2. Abrindo o navegador para autorização...")
    print("   (Faça login com a conta Google que gerenciará os relatórios)")
    creds = flow.run_local_server(port=0, prompt='consent')

    # Salva o token.json com o refresh token
    print(f"\n3. Salvando credenciais de acesso em: {TOKEN_FILE}")
    with open(TOKEN_FILE, 'w', encoding='utf-8') as token:
        token.write(creds.to_json())

    # Valida a conexão chamando a API do Drive
    try:
        service = build('drive', 'v3', credentials=creds)
        about = service.about().get(fields="user").execute()
        user_info = about.get('user', {})
        email = user_info.get('emailAddress', 'Desconhecido')
        name = user_info.get('displayName', 'Usuário')

        print("\n" + "=" * 60)
        print(" [SUCESSO] Autenticação realizada com sucesso!")
        print(f" Conectado como : {name} ({email})")
        print("=" * 60)

        # Pergunta ou ajuda a identificar a pasta de relatórios
        print("\nVerificando pastas no seu Google Drive...")
        results = service.files().list(
            q="mimeType='application/vnd.google-apps.folder' and trashed=false",
            fields="files(id, name)",
            pageSize=10,
        ).execute()
        folders = results.get('files', [])

        if folders:
            print("\nPastas encontradas no seu Google Drive:")
            for idx, folder in enumerate(folders, 1):
                print(f"  [{idx}] {folder['name']} (ID: {folder['id']})")
        else:
            print("Nenhuma pasta existente encontrada.")

        print("\nPara usar uma pasta específica, configure no seu arquivo .env:")
        print("GOOGLE_DRIVE_FOLDER_ID=<ID_DA_PASTA>")

    except Exception as e:
        print(f"[AVISO] Token gerado, mas ocorreu erro ao testar API: {e}")


if __name__ == '__main__':
    authenticate()
