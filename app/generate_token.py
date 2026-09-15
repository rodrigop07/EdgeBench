"""
generate_token.py — Gera token.json OAuth 2.0 via fluxo de autenticação no navegador.

Execute UMA VEZ localmente (fora do Docker) para gerar o token.json com refresh_token.
Depois, o token.json será usado automaticamente pelo container para renovar o acesso.

Uso:
    python generate_token.py
"""

import json
import os
from google_auth_oauthlib.flow import InstalledAppFlow

SCOPES = ['https://www.googleapis.com/auth/drive']
CREDENTIALS_FILE = 'credentials.json'
TOKEN_OUTPUT = 'token.json'


def main():
    if not os.path.exists(CREDENTIALS_FILE):
        print(f"ERRO: Arquivo '{CREDENTIALS_FILE}' não encontrado.")
        print("Baixe o credentials.json do Google Cloud Console (OAuth 2.0 Client ID).")
        return

    print("Abrindo navegador para autenticação Google...")
    print("Faça login com a conta que tem acesso à pasta do Drive.\n")

    flow = InstalledAppFlow.from_client_secrets_file(CREDENTIALS_FILE, SCOPES)
    creds = flow.run_local_server(port=0)

    token_data = json.loads(creds.to_json())
    with open(TOKEN_OUTPUT, 'w') as f:
        json.dump(token_data, f, indent=2)

    print(f"\n✅ token.json gerado com sucesso em: {os.path.abspath(TOKEN_OUTPUT)}")
    print("Agora reconstrua o container: docker compose up -d --build backend")


if __name__ == '__main__':
    main()
