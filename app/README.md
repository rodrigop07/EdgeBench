# EdgeBench Backend

O EdgeBench Backend é responsável por ingerir a telemetria via MQTT publicada pelo hardware EdgeBench (ESP32), armazenar os dados no PostgreSQL e gerar relatórios em formato Excel (e Google Sheets) com gráficos automáticos para acompanhamento da produção.

## 🚀 Como Executar o Projeto

O backend é 100% conteinerizado usando Docker.

Para subir a infraestrutura completa (Mosquitto, PostgreSQL e o Backend Python):
```bash
cd app
docker compose up -d --build
```

Para acompanhar os logs do backend em tempo real:
```bash
docker compose logs -f backend
```

## 📊 Como Gerar Planilhas Sob Demanda

Você pode gerar relatórios em formato Excel usando a interface de linha de comando (`CLI`) que foi construída no `main.py`, rodando-a de dentro do container do backend.

**Opções disponíveis:**
* `--export` : Ativa o modo de geração de relatório.
* `--bancada <ID>` : Filtra a geração para uma bancada específica (Ex: `BC-01`).
* `--start <DATA>` : Filtra o relatório a partir de uma data específica (ISO 8601, ex: `2026-09-01T00:00:00`).
* `--end <DATA>` : Filtra o relatório até uma data específica (ISO 8601).
* `--output <CAMINHO>` : Define onde o arquivo será salvo.

### Exemplos de uso

Gerar o relatório completo de tudo que está no banco de dados:
```bash
docker compose exec backend python main.py --export
```

Gerar relatório de uma bancada num intervalo específico:
```bash
docker compose exec backend python main.py --export --bancada BC-01 --start 2026-09-01T00:00:00 --end 2026-09-14T23:59:59
```

*Nota: Os relatórios gerados via comando ou via scheduler são salvos na pasta local `app/reports/` na sua máquina Host (via volume do Docker).*


## ☁️ Integração com Google Sheets (Sincronização e Backup)

O sistema possui uma rotina em background (`scheduler.py`) que roda automaticamente 2 vezes ao dia (às 12h00 e 23h50).
Essa rotina faz:
1. Um backup do banco gerando um Excel com a data/hora no nome dentro da pasta `reports/backups/`.
2. Sincroniza a versão mais recente para uma pasta no seu **Google Drive**, convertendo-a automaticamente para **Google Sheets** nativo.

### Passo a Passo para Configurar o Google Sheets

Para que a sincronização funcione, você precisa criar uma conta de serviço no Google Cloud e dar permissão na sua pasta do Google Drive. Siga os passos:

#### 1. Criar a Service Account e Obter a Chave (JSON)
1. Acesse o [Google Cloud Console](https://console.cloud.google.com/).
2. Crie um novo projeto (ou selecione um existente).
3. Vá em "APIs e Serviços" > "Biblioteca" e pesquise por **Google Drive API** e **Google Sheets API**. Ative as duas.
4. Vá em "APIs e Serviços" > "Credenciais".
5. Clique em **Criar Credenciais** > **Conta de Serviço**. Preencha o nome (ex: `edgebench-sync`) e conclua.
6. Na lista de Contas de Serviço, clique na que você acabou de criar. Note que ela tem um e-mail longo (ex: `edgebench-sync@projeto-id.iam.gserviceaccount.com`). Copie esse e-mail!
7. Vá na aba "Chaves" > "Adicionar Chave" > "Criar nova chave".
8. Escolha o formato **JSON** e clique em Criar. O arquivo será baixado para o seu computador.
9. Renomeie esse arquivo para `google-credentials.json` e cole-o dentro da pasta `/app/` no servidor/Host.

#### 2. Configurar a Pasta no Google Drive
1. Abra seu [Google Drive](https://drive.google.com/).
2. Crie uma nova pasta (ex: `Relatórios EdgeBench`).
3. Clique com o botão direito na pasta > Compartilhar.
4. Cole o e-mail longo da Conta de Serviço que você copiou no passo 6 e dê permissão de **Editor**.
5. Abra a pasta. Olhe a URL no navegador, ela será algo como `drive.google.com/drive/folders/1A2b3C4d5E_xyz`. A string após `/folders/` é o **ID da Pasta**.

#### 3. Inserir no Backend
Abra o arquivo `/app/.env` (crie-o a partir do `.env.example` se não tiver) e preencha as duas novas variáveis:

```env
GOOGLE_APPLICATION_CREDENTIALS=google-credentials.json
GOOGLE_DRIVE_FOLDER_ID=seu_folder_id_copiado_do_passo_anterior
```

Após fazer isso, basta reiniciar o container para que ele leia as novas variáveis:
```bash
docker compose restart backend
```

Feito isso! Na próxima vez que der o horário agendado, o backend fará o upload e a planilha "EdgeBench_Relatorio" aparecerá na sua pasta do Google Drive, magicamente convertida em Google Sheets.
