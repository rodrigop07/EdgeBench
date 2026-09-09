# Projeto PNAAT: Sistema Embarcado IoT para Apontamento Automático de Produção

> **Solução Ciber-Física de Baixo Custo para Digitalização do Chão de Fábrica (Cenário 6)**  
> *Sensoriamento não-invasivo por barreira óptica, persistência offline de alta densidade (90+ dias) e telemetria industrial resiliente via MQTT.*

---

## Visão Geral do Projeto

Nas indústrias com linhas de montagem manuais, os operadores perdem frequentemente de **5% a 10% do seu tempo útil** registrando contagens de peças em pranchetas e formulários de papel. Esse processo tradicional gera três problemas críticos:
1. **Drenagem da capacidade produtiva** devido a constantes pausas operacionais;
2. **Erros humanos de contagem**, anotações retroativas e números aproximados;
3. **Invisibilidade em tempo real** para o Planejamento e Controle da Produção (PCP), ocultando gargalos e paradas não programadas.

O **Projeto PNAAT** resolve esse problema por meio de uma abordagem **100% passiva e não invasiva**: um dispositivo de borda microcontrolado instalado na calha de escoamento de cada bancada. Cada peça montada que desliza pela rampa de gravidade corta um feixe infravermelho modulado, sendo contabilizada instantaneamente por interrupção de hardware, sem demandar nenhuma intervenção do trabalhador.

```
                  +----------------------------------------------+
                  |           CHÃO DE FÁBRICA (BANCADA)          |
                  |                                              |
                  |   [Operador]  --> Monta a peça               |
                  |        |                                     |
                  |        v                                     |
                  |   [Calha de Saída]                           |
                  |        |                                     |
                  |   (Corte do feixe óptico E18-D80NK)          |
                  |        |                                     |
                  |        v                                     |
                  |   [ESP32: ISR + Debounce + LittleFS]         |
                  +-----------------------+----------------------+
                                          |
                      (Wi-Fi 2.4 GHz / Protocolo MQTT)
                                          |
                                          v
                  +----------------------------------------------+
                  |         INFRAESTRUTURA & BACKEND             |
                  |                                              |
                  |   [Broker MQTT: Eclipse Mosquitto]           |
                  |        |                                     |
                  |        v                                     |
                  |   [Ingestor Python 3 + SQLAlchemy]           |
                  |        |                                     |
                  |        v                                     |
                  |   [Banco de Dados Relacional]                |
                  |        |                                     |
                  |        v                                     |
                  |   [Geração Automática de Planilhas .XLSX]    |
                  |        |                                     |
                  |        v                                     |
                  |   [Gestão PCP / Dashboards Executivos]       |
                  +----------------------------------------------+
```

---

## Principais Diferenciais de Engenharia

* **Soberania da Borda (Offline-First):** Se o Wi-Fi ou a rede fabril caírem, o microcontrolador armazena localmente os registros em uma partição **LittleFS** protegida contra cortes bruscos de energia (*power-loss recovery*).
* **Buffer Binário de Alta Densidade:** Cada registro ocupa apenas **8 bytes** (struct compacta em C), viabilizando armazenar mais de **131.000 eventos** em apenas 1 MB de Flash (mais de **90 dias de autonomia** contínua sem conexão).
* **Imunidade a Trepidações (Debounce Digital de 300 ms):** O tratamento de sinal rejeita repiques elétricos e oscilações da peça na descida da calha.
* **Mensageria com Garantia de Entrega (MQTT QoS 1):** Os dados acumulados durante quedas de rede são transmitidos em ordem cronológica estrita (*FIFO*) e só são expurgados da Flash após a confirmação expressa (*PUBACK*) do Broker.
* **Relatórios Automatizados (.xlsx):** O backend central consolida a produção por hora e por turno (06h-14h, 14h-22h, 22h-06h), gerando planilhas prontas para análise da engenharia industrial.

---

## Pilha Tecnológica

| Camada | Tecnologia Adotada | Finalidade |
| :--- | :--- | :--- |
| **Microcontrolador** | **ESP32-WROOM-32** (Dual-Core 240 MHz) | Gestão da interrupção de contagem no Core 1 e rede/Flash no Core 0. |
| **Sensoriamento** | **Sensor Fotoelétrico E18-D80NK** (Infravermelho) | Detecção de passagem sem contato físico, ajustável de 3 a 80 cm. |
| **Sistema Operacional de Borda** | **FreeRTOS / C++ (ESP-IDF)** | Multithreading preemptivo, filas assíncronas e timers de hardware. |
| **Sistema de Arquivos Local** | **LittleFS** | Tolerância a quedas de energia e nivelamento de desgaste da Flash (*wear leveling*). |
| **Conectividade e Protocolo** | **Wi-Fi 802.11 b/g/n + MQTT v3.1.1 (QoS 1)** | Telemetria leve em tempo real com confirmação de entrega. |
| **Broker de Mensageria** | **Eclipse Mosquitto / EMQX** | Roteamento assíncrono de mensagens entre bancadas e o servidor. |
| **Serviço de Ingestão e Processamento** | **Python 3 (Paho-MQTT, SQLAlchemy)** | Consumo de filas, controle de idempotência e persistência relacional. |
| **Banco de Dados** | **SQLite / PostgreSQL** | Armazenamento de séries temporais de apontamento. |
| **Exportação e Relatórios** | **Pandas + OpenPyXL** | Consolidação por turno e formatação visual das planilhas gerenciais. |

---

## Resumo de Requisitos e Regras de Negócio

### Requisitos Funcionais em Destaque
* **RF-01:** Detecção por interrupção externa de hardware (*ISR*) no pino digital.
* **RF-02:** Filtro digital de *debounce* por temporização (mínimo de 300 ms).
* **RF-03:** Persistência binária compacta de 8 bytes em LittleFS durante modo offline.
* **RF-04:** Publicação dinâmica MQTT (payload JSON) em tópicos segregados por bancada.
* **RF-06:** Descarregamento ordenado (*FIFO*) com expurgo condicionado a *ACK*.
* **RF-08:** Geração agendada de relatórios tabulares em formato Excel (`.xlsx`).

### Requisitos Não Funcionais em Destaque
* **RNF-01:** Ocupação máxima de 8 bytes por registro (capacidade > 131.000 eventos/MB).
* **RNF-02:** Integridade de dados resistente a desligamento abrupto de energia (LittleFS).
* **RNF-03:** Latência de disparo do tratador de interrupção inferior a 10 µs.
* **RNF-05:** Acurácia global de contagem igual ou superior a 99,5%.

### Regras de Negócio Fundamentais
* **RN-01 (Takt Time Mínimo):** Pulsos com intervalo inferior a 300 ms são sumariamente descartados como vibração espúria.
* **RN-02 (Janelas de Turno):** Fechamento de planilhas nos limites de turno (06h, 14h, 22h) e fechamento parcial por hora cheia.
* **RN-04 (Soberania de Contagem):** Tarefas de rede ou escrita lenta jamais podem bloquear ou descartar pulsos físicos da bancada.
* **RN-07 (Idempotência):** Chave composta `(bancada_id, timestamp)` impede que retransmissões de rede dupliquem registros no banco de dados.

---

## Estrutura de Documentação do Repositório

Optou-se por consolidar a documentação técnica profunda em um documento mestre rigoroso, garantindo fácil consulta e avaliação:

```
projeto-pnaat/
├── README.md                                  # Visão executiva e guia do projeto (este arquivo)
├── LICENSE                                    # Licença do projeto
├── requisitos/
│   ├── especificacao_tecnica.md               #  DOCUMENTO MESTRE: Requisitos RF/RNF/RN, Escopo,
│   │                                          #    Levantamento Técnico, IoT vs Visão Computacional,
│   │                                          #    Análise Físico-Mecânica e Visão Crítica Fabril
│   └── req.md                                 # Documento base de requisitos original e notas de engenharia
```

Para a especificação técnica aprofundada, com todos os diagramas de arquitetura, memória de cálculo dimensional, comparativo aprofundado com Visão Computacional e análise da dor do chão de fábrica, consulte:
 **[especificacao_tecnica.md](/requisitos/especificacao_tecnica.md)**.
