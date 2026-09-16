import os
import sys
from typing import List, Dict, Any

# Adiciona o diretório app/ ao path para reaproveitar os módulos de banco de dados
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from fastapi import FastAPI, Depends, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy.orm import Session
from sqlalchemy import text

from database import engine, SessionLocal
from models import TelemetriaBancada
import logging

logger = logging.getLogger(__name__)

def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()

app = FastAPI(title="EdgeBench API", description="API para Dashboard de Produção OEE", version="1.0.0")

# Configurar CORS (permitir chamadas do Front-end)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Em produção, restringir para o domínio correto
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/")
def read_root():
    return {"status": "ok", "message": "EdgeBench API rodando"}

@app.get("/api/status")
def get_current_status(db: Session = Depends(get_db)):
    """
    Retorna o status atual de produção (resumo consolidado do turno aberto).
    Para MVP, usaremos uma query simplificada sobre a telemetria mais recente.
    """
    try:
        # Busca a última leitura
        ultima_leitura = db.query(TelemetriaBancada).order_by(TelemetriaBancada.timestamp_esp.desc()).first()
        
        # Agregações simples para o dia de hoje (simulando um turno)
        # O ideal aqui é chamar uma função similar ao analytics.py
        from analytics import full_report
        
        # O full_report processa os dados com base nos turnos no BD e retorna métricas
        data = full_report()
        
        # 1. Global KPIs
        global_raw = data.get("global_kpis", {})
        global_kpis = {
            "total_pecas": int(global_raw.get("total_pecas", 0)),
            "bancadas_ativas": int(global_raw.get("bancadas_ativas", 0)),
            "taxa_pecas_hora": float(global_raw.get("taxa_pecas_hora", 0.0)),
            "total_paradas": int(global_raw.get("total_paradas_rn06", 0))
        }
        
        # 2. KPIs por Bancada
        kpis_df = data.get("kpis")
        kpis_bancadas = []
        if kpis_df is not None and not kpis_df.empty:
            for _, row in kpis_df.iterrows():
                kpis_bancadas.append({
                    "bancada": row["bancada_id"],
                    "total_pecas": int(row.get("total_pecas", 0)),
                    "disponibilidade": float(row.get("uptime_pct", 0.0)),
                    "paradas": int(row.get("paradas", 0)),
                    "tempo_parado": float(row.get("tempo_ocioso_min", 0.0)),
                    "taxa_pecas_hora": float(row.get("taxa_pecas_hora", 0.0))
                })
                
        # 3. Gráfico por Hora (Global e Por Bancada)
        por_hora_df = data.get("por_hora")
        grafico_hora = []
        grafico_hora_bancadas = {}
        
        if por_hora_df is not None and not por_hora_df.empty:
            # Global
            hora_agg = por_hora_df.groupby("hora")["total_pecas"].sum().reset_index()
            for _, row in hora_agg.iterrows():
                hora_str = f"{int(row['hora']):02d}:00"
                grafico_hora.append({
                    "hora": hora_str,
                    "pecas": int(row["total_pecas"])
                })
                
            # Por Bancada
            for bancada, group in por_hora_df.groupby("bancada_id"):
                b_hora_agg = group.groupby("hora")["total_pecas"].sum().reset_index()
                g_list = []
                for _, row in b_hora_agg.iterrows():
                    g_list.append({
                        "hora": f"{int(row['hora']):02d}:00",
                        "pecas": int(row["total_pecas"])
                    })
                grafico_hora_bancadas[str(bancada)] = g_list
        
        return {
            "success": True,
            "data": {
                "global_kpis": global_kpis,
                "kpis_bancadas": kpis_bancadas,
                "grafico_hora": grafico_hora,
                "grafico_hora_bancadas": grafico_hora_bancadas
            },
            "ultima_leitura": ultima_leitura.timestamp_esp.isoformat() if ultima_leitura else None
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/api/reports")
def list_reports():
    """
    Lista todos os relatórios gerados salvos no Google Drive.
    """
    try:
        from google_sheets_sync import list_drive_reports
        reports = list_drive_reports()
        
        # Formata a lista para o front-end
        formatted_reports = []
        for r in reports:
            # O tamanho pode não vir no Sheets nativo, mas vem no .xlsx. Vamos tratar caso não exista.
            size = int(r.get("size", 0)) if "size" in r else 0
            
            formatted_reports.append({
                "id": r.get("id"),
                "filename": r.get("name"),
                "webViewLink": r.get("webViewLink"),
                "mimeType": r.get("mimeType"),
                "created_at": r.get("createdTime"), # Já vem em formato string ISO
                "size_bytes": size
            })
            
        return {"success": True, "reports": formatted_reports}
    except Exception as e:
        logger.error(f"Erro ao buscar relatórios do Drive: {e}")
        return {"success": False, "reports": []}

@app.get("/api/reports/download/{filename}")
def download_report(filename: str):
    """
    Baixa um relatório específico da pasta local.
    """
    # pyrefly: ignore [missing-import]
    from fastapi.responses import FileResponse
    
    reports_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'reports'))
    filepath = os.path.join(reports_dir, filename)
    
    if not os.path.exists(filepath) or not filepath.startswith(reports_dir):
        raise HTTPException(status_code=404, detail="Relatório não encontrado")
        
    return FileResponse(path=filepath, filename=filename, media_type='application/vnd.openxmlformats-officedocument.spreadsheetml.sheet')

if __name__ == "__main__":
    # pyrefly: ignore [missing-import]
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
