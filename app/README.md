# EdgeBench — Backend

O **EdgeBench Backend** é responsável por:

- **Ingerir telemetria** via MQTT publicada pelo hardware EdgeBench (ESP32)
- **Armazenar os dados** no PostgreSQL
- **Gerar relatórios Excel** com gráficos automáticos por bancada, turno e hora
- **Sincronizar automaticamente** o relatório mais recente para o Google Drive

Toda a infraestrutura é **100% conteinerizada com Docker**.

---

## 📦 Pré-requisitos

- [Docker](https://docs.docker.com/get-docker/) + [Docker Compose](https://docs.docker.com/compose/)
- Python 3.11+ *(apenas para gerar o token OAuth — etapa única)*
- Conta Google com acesso à pasta de destino no Drive

---

## 🚀 Subindo o Projeto

```bash
# Clone o repositório (branch com o backend)
git clone -b app git@github.com:rodrigop07/EdgeBench.git
cd EdgeBench/app

# Configure as variáveis de ambiente
cp .env.example .env
# Edite o .env se precisar mudar senhas ou IDs

# Suba todos os serviços (Mosquitto + PostgreSQL + Backend)
docker compose up -d --build
```

Acompanhe os logs em tempo real:

```bash
docker compose logs -f backend
```

---

## ☁️ Configurando o Google Drive (etapa única por máquina)

A sincronização usa **OAuth 2.0** com a sua conta Google pessoal.  
Você precisa gerar um `token.json` localmente **uma vez** e ele será renovado automaticamente depois disso.

### Passo 1 — Instale as dependências Python locais

```bash
# Dentro da pasta app/, ative o venv (ou instale globalmente)
pip install google-auth-oauthlib google-auth-httplib2 google-api-python-client
```

### Passo 2 — Obtenha o `credentials.json` do projeto

Peça ao responsável pelo projeto (`rodrigop07`) o arquivo `credentials.json`.  
Coloque-o dentro da pasta `app/`.

> ⚠️ **Nunca commite esse arquivo.** Ele já está no `.gitignore`.

### Passo 3 — Gere o seu `token.json`

```bash
python generate_token.py
```

Um navegador abrirá pedindo login com sua conta Google.  
Após autorizar, o arquivo `token.json` será criado na pasta `app/`.

> ⚠️ **Nunca commite esse arquivo.** Ele já está no `.gitignore`.

### Passo 4 — Suba o container

```bash
docker compose up -d --build backend
```

O backend usará o `token.json` para autenticar e fazer upload dos relatórios para a pasta configurada no Drive.

---

## 📊 Gerando Relatórios Manualmente

Você pode gerar e sincronizar um relatório sob demanda sem esperar o agendamento:

```bash
# Gera Excel + faz upload para o Drive agora
docker compose exec backend python -c "from scheduler import perform_backup_and_sync; perform_backup_and_sync()"
```

Ou gerar apenas o Excel localmente (sem sync):

```bash
# Relatório completo
docker compose exec backend python main.py --export

# Filtrado por bancada e intervalo de datas
docker compose exec backend python main.py --export \
  --bancada BC-01 \
  --start 2026-09-01T00:00:00 \
  --end 2026-09-14T23:59:59
```

Os arquivos são salvos em `app/reports/` na sua máquina (via volume Docker).

---

## ⏱️ Agendamento Automático

O scheduler roda automaticamente **2x por dia** (12h00 e 23h50) e executa:

1. Gera um Excel com timestamp e salva em `reports/backups/`
2. Faz upload da versão mais recente para a pasta do Google Drive configurada em `GOOGLE_DRIVE_FOLDER_ID`

---

## 🗂️ Estrutura dos Arquivos

```
app/
├── main.py               # CLI + inicialização do MQTT listener
├── mqtt_listener.py      # Subscrição e processamento de mensagens MQTT
├── models.py             # Modelos SQLAlchemy (tabelas do banco)
├── database.py           # Pool de conexão PostgreSQL
├── analytics.py          # Agregações e KPIs a partir dos dados
├── excel_generator.py    # Geração de relatórios Excel com gráficos
├── google_sheets_sync.py # Upload OAuth 2.0 para o Google Drive
├── scheduler.py          # Rotina agendada de backup e sync
├── config.py             # Configurações via variáveis de ambiente
├── generate_token.py     # Script único para gerar token OAuth local
├── Dockerfile            # Build multi-stage da imagem Python
├── docker-compose.yml    # Orquestra backend + postgres + mosquitto
├── .env.example          # Template de variáveis de ambiente
└── requirements.txt      # Dependências Python
```

---

## 🔐 Arquivos Sensíveis (não commitados)

| Arquivo | Descrição |
|---|---|
| `.env` | Variáveis de ambiente com senhas e IDs |
| `token.json` | Token OAuth gerado localmente — **pessoal, não compartilhe** |
| `credentials.json` | Segredo OAuth do app Google Cloud — **solicite ao responsável** |

Todos estão no `.gitignore` e **nunca devem ser commitados**.
