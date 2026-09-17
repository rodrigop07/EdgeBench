# ── Stage 1: Builder ─────────────────────────────────────────────────────────
FROM python:3.11-slim AS builder

WORKDIR /build

# Instala dependências de compilação mínimas
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# Copia e instala dependências em um venv isolado
COPY requirements.txt .
RUN pip install --upgrade pip && \
    pip install --prefix=/install --no-cache-dir -r requirements.txt


# ── Stage 2: Runtime ─────────────────────────────────────────────────────────
FROM python:3.11-slim AS runtime

# Metadados da imagem
LABEL maintainer="EdgeBench Team" \
    description="EdgeBench Backend — MQTT Ingestor + PostgreSQL + Excel Reports" \
    version="1.0.0"

WORKDIR /app

# Dependências de runtime do psycopg2
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    && rm -rf /var/lib/apt/lists/*

# Copia pacotes instalados do stage builder
COPY --from=builder /install /usr/local

# Copia o código-fonte da aplicação
COPY *.py .env.example ./
COPY api/ ./api/
COPY external_integrations/ ./external_integrations/
COPY hardware_comunication/ ./hardware_comunication/
COPY services/ ./services/
COPY settings/ ./settings/

# Cria diretório de relatórios com permissão de escrita
RUN mkdir -p /app/reports && chmod 755 /app/reports

# Variáveis de ambiente padrão (podem ser sobrescritas via docker-compose)
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPATH=/app \
    LOG_LEVEL=INFO \
    REPORTS_DIR=/app/reports \
    POSTGRES_HOST=postgres \
    POSTGRES_PORT=5432 \
    POSTGRES_DB=edgebench \
    POSTGRES_USER=edgebench_user \
    POSTGRES_PASSWORD=edgebench_pass \
    MQTT_HOST=mosquitto \
    MQTT_PORT=1883 \
    MQTT_CLIENT_ID=edgebench_backend \
    MQTT_TOPIC_FILTER=fabrica/bancada_+/# \
    MQTT_QOS=1

# Expõe volume para persistência dos relatórios gerados
VOLUME ["/app/reports"]

# Healthcheck: verifica se o processo Python está vivo
HEALTHCHECK --interval=30s --timeout=10s --start-period=15s --retries=3 \
    CMD python -c "from settings.database import check_connection; exit(0 if check_connection() else 1)"

# Ponto de entrada padrão: modo ingestão MQTT contínua
CMD ["python", "main.py"]