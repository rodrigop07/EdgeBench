import os
import sys
from typing import List, Dict, Any

# Garante que possamos importar os módulos da raiz da aplicação
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from fastapi import FastAPI, Depends, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy.orm import Session
from sqlalchemy import text

from settings.database import engine, SessionLocal
from settings.models import TelemetriaBancada
import logging

logger = logging.getLogger(__name__)

def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()

app = FastAPI(title="EdgeBench API", description="API para Dashboard de Produção OEE", version="1.0.0")

# Permite que o frontend se comunique com esta API (CORS)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # TODO: Restringir isso em produção
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/")
def read_root():
    return {"status": "ok", "message": "EdgeBench API rodando"}

@app.get("/api/status")
def get_current_status(db: Session = Depends(get_db)):
    """Retorna um resumo de como está a produção agora."""
    try:
        # Pega a leitura mais recente do banco
        ultima_leitura = db.query(TelemetriaBancada).order_by(TelemetriaBancada.timestamp_esp.desc()).first()
        
        # Gera as métricas usando a lógica do nosso painel
        from services.analytics import full_report
        data = full_report()
        
        # 1. Resumo Global
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
    """Lista os relatórios já salvos lá no Google Drive."""
    try:
        from external_integrations.google_sheets_sync import list_drive_reports
        reports = list_drive_reports()
        
        # Arruma os dados para entregar direitinho pro front
        formatted_reports = []
        for r in reports:
            # Verifica o tamanho do arquivo, se tiver
            size = int(r.get("size", 0)) if "size" in r else 0
            
            formatted_reports.append({
                "id": r.get("id"),
                "filename": r.get("name"),
                "webViewLink": r.get("webViewLink"),
                "mimeType": r.get("mimeType"),
                "created_at": r.get("createdTime"), # Já vai como texto pronto
                "size_bytes": size
            })
            
        return {"success": True, "reports": formatted_reports}
    except Exception as e:
        logger.error(f"Erro ao buscar relatórios do Drive: {e}")
        return {"success": False, "reports": []}

@app.get("/api/reports/download/{filename}")
def download_report(filename: str):
    """Envia um arquivo local de relatório para o usuário baixar."""
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
