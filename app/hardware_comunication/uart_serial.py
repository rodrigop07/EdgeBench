"""
Ponte Serial (Computador <-> Placa Central).
Permite configurar as placas, ver o que está acontecendo e mandar comandos pelo ar (LoRa).
"""

import http.server
import json
import logging
import os
import queue
import socket
import socketserver
import sys
import threading
import time
from typing import Any, Dict, List, Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[AVISO] A biblioteca 'pyserial' nao foi encontrada no ambiente Python atual")
    print("Para instalar, execute: pip install pyserial\n")
    serial = None

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] (%(name)s) %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("EdgeBench_Serial")

# Garante que a gente consiga importar as configs da pasta app
APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

try:
    from settings.config import mqtt_config
    DEFAULT_MQTT_HOST = mqtt_config.host
    DEFAULT_MQTT_PORT = mqtt_config.port
except Exception:
    DEFAULT_MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
    DEFAULT_MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

DEFAULT_BUILD_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "firmware_bancada", "build")
)


class LocalOTAServer:
    """Servidor web simples que entrega o arquivo de atualização pras placas."""

    def __init__(self, directory: str = DEFAULT_BUILD_DIR, port: int = 8080):
        self.directory = directory
        self.port = port
        self.httpd = None
        self.thread = None
        self.is_running = False

    @staticmethod
    def get_local_ip() -> str:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"

    def start(self) -> bool:
        if self.is_running:
            return True

        if not os.path.exists(self.directory):
            logger.error(f"Diretório não encontrado: {self.directory}")
            return False

        server_dir = self.directory

        class CustomHandler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=server_dir, **kwargs)

            def log_message(self, fmt, *args):
                logger.info(f"[HTTP OTA] {self.client_address[0]} - {fmt % args}")

        try:
            socketserver.TCPServer.allow_reuse_address = True
            self.httpd = socketserver.TCPServer(("", self.port), CustomHandler)
            self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
            self.thread.start()
            self.is_running = True
            local_ip = self.get_local_ip()
            logger.info(f"Servidor HTTP OTA iniciado em http://{local_ip}:{self.port}")
            logger.info(f"Servindo pasta de build: {self.directory}")
            return True
        except Exception as e:
            logger.error(f"Erro ao iniciar servidor HTTP na porta {self.port}: {e}")
            self.httpd = None
            self.is_running = False
            return False

    def stop(self):
        if self.httpd and self.is_running:
            try:
                self.httpd.shutdown()
                self.httpd.server_close()
            except Exception as e:
                logger.warning(f"Erro ao encerrar servidor HTTP: {e}")
        self.httpd = None
        self.thread = None
        self.is_running = False
        logger.info("Servidor HTTP OTA encerrado")

    def get_firmware_url(self, filename: str = "firmware_bancada.bin") -> str:
        local_ip = self.get_local_ip()
        return f"http://{local_ip}:{self.port}/{filename}"


class EdgeBenchGateway:
    """Gerencia a conversa via cabo USB com a placa central."""

    def __init__(self, port: Optional[str] = None, baudrate: int = 115200, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None
        self._rx_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._rx_thread = None
        self._cmd_resp_queue = queue.Queue()
        self._pairing_queue = queue.Queue()
        self._pong_queue = queue.Queue()
        self._bench_info_queue = queue.Queue()
        self._monitor_queue = queue.Queue()
        self._monitoring = False

    @staticmethod
    def list_available_ports() -> List[str]:
        # Lista os cabos seriais conectados
        if serial is None:
            return []
        ports = serial.tools.list_ports.comports()
        return [p.device for p in ports]

    def auto_detect_port(self) -> Optional[str]:
        # Acha a placa central sozinha
        if serial is None:
            return None

        ports = serial.tools.list_ports.comports()
        for p in ports:
            desc = (p.description or "").lower()
            hwid = (p.hwid or "").lower()
            # Nomes comuns de placas ESP32
            if any(k in desc or k in hwid for k in ["cp210", "ch340", "ch341", "usb-serial", "esp32", "jtag"]):
                logger.info(f"Porta do ESP32 detectada: {p.device} ({p.description})")
                return p.device

        if ports:
            logger.info(f"Usando primeira porta disponivel: {ports[0].device} ({ports[0].description})")
            return ports[0].device

        return None

    def connect(self) -> bool:
        # Liga o cabo
        if serial is None:
            logger.error("pyserial nao instalado. Impossivel abrir porta serial.")
            return False

        if not self.port:
            self.port = self.auto_detect_port()

        if not self.port:
            logger.error("Nenhuma porta serial informada ou encontrada no sistema.")
            return False

        try:
            logger.info(f"Conectando ao ESP32 em {self.port} a {self.baudrate} bps...")
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=0.1,
                write_timeout=self.timeout,
            )
            time.sleep(1.5)
            self.ser.reset_input_buffer()
            
            self._stop_event.clear()
            self._rx_thread = threading.Thread(target=self._background_rx_loop, daemon=True)
            self._rx_thread.start()

            logger.info(f"Conexao estabelecida com sucesso na porta {self.port}")
            # Já acerta o relógio logo de cara
            logger.info("Sincronizando horario da Central automaticamente...")
            self.sync_time()
            return True
        except Exception as e:
            logger.error(f"Falha ao abrir porta serial {self.port}: {e}")
            self.ser = None
            return False


    def _background_rx_loop(self):
        while not self._stop_event.is_set():
            has_data = False
            raw_line = ""
            try:
                with self._rx_lock:
                    if self.ser and self.ser.is_open and self.ser.in_waiting > 0:
                        has_data = True
                        try:
                            raw_line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                        except Exception:
                            raw_line = ""

                if raw_line:
                    logger.debug(f"RX Serial <- {raw_line}")
                    if self._monitoring:
                        self._monitor_queue.put(raw_line)

                    if raw_line.startswith("{") and raw_line.endswith("}"):
                        try:
                            j = json.loads(raw_line)
                            msg_type = j.get("type")
                            event = j.get("event")
                            status = j.get("status")
                            msg = j.get("msg")

                            if msg_type == "telemetry":
                                # Manda a mensagem pro banco sem travar a leitura do cabo
                                threading.Thread(target=self._publish_telemetry, args=(j,), daemon=True).start()
                            elif event == "req_time":
                                logger.info("[EVENT] Central solicitou horario (bancada ou boot). Enviando timestamp do PC...")
                                self.sync_time(wait_response=False)
                            elif status == "pairing":
                                self._pairing_queue.put(j)
                            elif msg == "pong":
                                self._pong_queue.put(j)
                            elif msg == "bench_info":
                                self._bench_info_queue.put(j)
                            elif "status" in j:
                                self._cmd_resp_queue.put(j)
                        except Exception as e:
                            logger.error(f"Erro no parse de JSON da serial: {e}")
            except Exception as e:
                logger.error(f"Exceção inesperada na leitura serial: {e}")
                if self._monitoring:
                    self._monitor_queue.put(f"[ALERTA SERIAL] Erro de I/O na porta: {e}")
                time.sleep(0.5)

            if not has_data:
                time.sleep(0.02)

    def _publish_telemetry(self, data):
        if mqtt is None:
            logger.warning("[MQTT] Biblioteca paho-mqtt nao disponível, impossível publicar telemetria")
            return

        bench_id = data.get("bench_id", 1)
        count = data.get("count", 0)
        ts = data.get("timestamp", int(time.time()))

        # Arruma a data
        from datetime import datetime
        try:
            time_str = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")
        except Exception:
            time_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        # Tópico padrão da bancada
        topic = f"fabrica/bancada_{bench_id}/producao"

        payload_dict = {
            "bancada": bench_id,
            "contagem": count,
            "horario": time_str,
            "timestamp": ts,
            "quantidade": 1,
            "modo_offline": True,
            "fonte": "central_lora"
        }
        payload = json.dumps(payload_dict)

        host = getattr(self, "broker_host", DEFAULT_MQTT_HOST)
        port = getattr(self, "broker_port", DEFAULT_MQTT_PORT)

        try:
            import paho.mqtt.publish as publish
            try:
                publish.single(
                    topic,
                    payload=payload,
                    qos=1,
                    hostname=host,
                    port=port,
                    client_id=f"edgebench_bridge_{bench_id}",
                )
                logger.info(f"[MQTT] Telemetria LoRa da Bancada {bench_id} publicada com sucesso em '{topic}' ({host}:{port}): {payload}")
            except Exception as conn_err:
                local_ip = LocalOTAServer.get_local_ip()
                if host in ("localhost", "127.0.0.1") and local_ip not in ("localhost", "127.0.0.1"):
                    logger.warning(f"[MQTT] Falha conectando em {host}, tentando IP local {local_ip}:{port}...")
                    publish.single(
                        topic,
                        payload=payload,
                        qos=1,
                        hostname=local_ip,
                        port=port,
                        client_id=f"edgebench_bridge_{bench_id}",
                    )
                    self.broker_host = local_ip
                    logger.info(f"[MQTT] Telemetria LoRa da Bancada {bench_id} publicada com sucesso em '{topic}' ({local_ip}:{port}): {payload}")
                else:
                    raise conn_err
        except Exception as e:
            logger.error(f"Falha ao repassar telemetria LoRa para MQTT ({host}:{port}): {e}")

    def disconnect(self):
        self._stop_event.set()
        if self._rx_thread:
            self._rx_thread.join(timeout=1.0)
        # Desliga o cabo
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
                logger.info("Porta serial fechada")
            except Exception as e:
                logger.warning(f"Erro ao fechar porta: {e}")
        self.ser = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()

    def send_command(self, cmd_dict: Dict[str, Any], wait_response: bool = True) -> Optional[Dict[str, Any]]:
        # Manda um comando e espera a placa responder
        if not self.ser or not self.ser.is_open:
            logger.error("Porta serial nao esta aberta")
            return None

        # Limpa o lixo da fila
        while not self._cmd_resp_queue.empty():
            try:
                self._cmd_resp_queue.get_nowait()
            except queue.Empty:
                break

        line_to_send = json.dumps(cmd_dict) + "\n"
        try:
            with self._rx_lock:
                self.ser.write(line_to_send.encode("utf-8"))
                self.ser.flush()
            logger.debug(f"TX Serial -> {line_to_send.strip()}")
        except Exception as e:
            logger.error(f"Erro ao transmitir comando pela serial: {e}")
            return None

        if not wait_response:
            return None

        try:
            resp = self._cmd_resp_queue.get(timeout=self.timeout)
            return resp
        except queue.Empty:
            logger.warning(f"Timeout aguardando resposta JSON para o comando '{cmd_dict.get('cmd')}'")
            return None

    # Comandos

    def ping(self) -> Optional[Dict[str, Any]]:
        # Verifica se a placa conectada está viva
        logger.info("Enviando comando PING (Gateway Local)")
        return self.send_command({"cmd": "ping"})

    def ping_broadcast(self, wait_seconds: float = 3.5) -> List[Dict[str, Any]]:
        """Grita pra todo mundo na fábrica e anota quem respondeu (Ping)."""
        logger.info("Disparando PING Broadcast via LoRa...")
        while not self._pong_queue.empty():
            try:
                self._pong_queue.get_nowait()
            except queue.Empty:
                break

        resp = self.send_command({"cmd": "ping_broadcast"})
        if not resp or resp.get("status") != "ok":
            logger.error(f"Falha ao enviar comando ping_broadcast: {resp}")
            return []

        nodes = []
        seen_macs = set()
        start_t = time.time()
        while (time.time() - start_t) < wait_seconds:
            try:
                data = self._pong_queue.get(timeout=0.1)
                mac = data.get("mac")
                if mac and mac not in seen_macs:
                    seen_macs.add(mac)
                    nodes.append(data)
                    b_id = data.get("bench_id", 0)
                    status_str = "Configurada" if b_id > 0 else "NÃO CONFIGURADA"
                    logger.info(f"Resposta PONG -> Bancada ID: {b_id}, MAC: {mac} ({status_str})")
                    print(f"  [+] Resposta #{len(nodes)} recebida: Bancada ID {b_id:<4} | MAC: {mac} ({status_str})")
            except queue.Empty:
                pass

        return nodes

    def is_alive(self) -> bool:
        """Verifica se o cabo USB ainda está funcionando."""
        return (
            self.ser is not None
            and self.ser.is_open
            and self._rx_thread is not None
            and self._rx_thread.is_alive()
        )

    def sync_time(self, timestamp: Optional[int] = None, wait_response: bool = True) -> Optional[Dict[str, Any]]:
        # sincroniza o relógio do ESP32 com o timestamp Epoch Unix informado
        # se nenhum timestamp for passado, utiliza o horário atual do computador
        ts = int(time.time()) if timestamp is None else int(timestamp)
        logger.info(f"Sincronizando horario do Gateway com Epoch {ts}...")
        return self.send_command({"cmd": "sync_time", "timestamp": ts}, wait_response=wait_response)

    def set_broker(self, broker_url: str) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para atualizar a URL do Broker MQTT em todas as bancadas
        logger.info(f"Disparando comando LoRa para reconfigurar Broker: {broker_url}")
        return self.send_command({"cmd": "set_broker", "url": broker_url})

    def set_wifi(self, ssid: str, password: str) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para atualizar as credenciais Wi-Fi em todas as bancadas
        logger.info(f"Disparando comando LoRa para reconfigurar Wi-Fi (SSID: {ssid})...")
        return self.send_command({"cmd": "set_wifi", "ssid": ssid, "pass": password})

    def set_bench(
        self, new_bench_id: int, target_bench_id: int = 0, target_mac: Optional[str] = None
    ) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para reconfigurar o ID numérico da bancada
        payload = {"cmd": "set_bench", "bench_id": new_bench_id, "target_id": target_bench_id}
        if target_mac:
            payload["mac"] = target_mac
            logger.info(
                f"Disparando comando LoRa direcionado (Alvo ID: {target_bench_id}, MAC: {target_mac}) para novo ID: {new_bench_id}"
            )
        else:
            logger.info(
                f"Disparando comando LoRa (Alvo ID: {target_bench_id}) para novo ID da bancada: {new_bench_id}"
            )
        return self.send_command(payload)

    def get_bench_info(self, target_bench_id: int = 0) -> Optional[Dict[str, Any]]:
        # consulta o endereço MAC do ESP32 associado a um ID de bancada via rádio LoRa
        logger.info(f"Consultando MAC da bancada ID {target_bench_id} via LoRa...")
        while not self._bench_info_queue.empty():
            try:
                self._bench_info_queue.get_nowait()
            except queue.Empty:
                break
        return self.send_command({"cmd": "get_bench_info", "bench_id": target_bench_id})

    def beacon_now(self) -> Optional[Dict[str, Any]]:
        # força a emissão imediata de um Beacon LoRa com o horário atual
        logger.info("Solicitando emissao imediata de Beacon LoRa...")
        return self.send_command({"cmd": "beacon_now"})

    def trigger_ota_lora(self, url: str, target_bench_id: int = 0) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para iniciar atualização OTA nas bancadas
        logger.info(f"Disparando comando LoRa de OTA (Alvo ID: {target_bench_id or 'Todas'}) para URL: {url}")
        return self.send_command({"cmd": "trigger_ota", "url": url, "target_id": target_bench_id})

    def set_debounce(
        self, debounce_ms: int, target_bench_id: int = 0, target_mac: Optional[str] = None
    ) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para reconfigurar o tempo de debounce do sensor (10 a 5000 ms)
        payload = {"cmd": "set_debounce", "debounce_ms": int(debounce_ms), "target_id": target_bench_id}
        if target_mac:
            payload["mac"] = target_mac
            logger.info(
                f"Disparando comando LoRa direcionado (Alvo ID: {target_bench_id}, MAC: {target_mac}) para novo Debounce: {debounce_ms} ms"
            )
        else:
            logger.info(
                f"Disparando comando LoRa (Alvo ID: {target_bench_id or 'Todas'}) para novo Debounce: {debounce_ms} ms"
            )
        return self.send_command(payload)

    def reboot_bench(
        self, target_bench_id: int = 0, target_mac: Optional[str] = None
    ) -> Optional[Dict[str, Any]]:
        # emite comando via rádio LoRa para reiniciar remotamente o ESP32 da bancada
        payload = {"cmd": "reboot", "target_id": target_bench_id}
        if target_mac:
            payload["mac"] = target_mac
            logger.info(
                f"Disparando comando LoRa direcionado (Alvo ID: {target_bench_id}, MAC: {target_mac}) para REBOOT do ESP32..."
            )
        else:
            logger.info(
                f"Disparando comando LoRa (Alvo ID: {target_bench_id or 'Todas'}) para REBOOT do ESP32..."
            )
        return self.send_command(payload)

    def publish_ota_mqtt(
        self,
        url: str,
        target_bench_id: int = 0,
        broker_host: Optional[str] = None,
        broker_port: Optional[int] = None,
    ) -> bool:
        # publica comando de atualização OTA via Broker MQTT
        if mqtt is None:
            logger.error("Biblioteca paho-mqtt não disponível no ambiente Python")
            return False

        host = broker_host or DEFAULT_MQTT_HOST
        port = broker_port or DEFAULT_MQTT_PORT
        topic = f"fabrica/bancada_{target_bench_id}/ota" if target_bench_id > 0 else "fabrica/todas/ota"
        payload = json.dumps({"cmd": "update", "url": url})

        try:
            logger.info(f"Conectando ao broker MQTT Mosquitto {host}:{port}...")
            client = mqtt.Client()
            client.connect(host, port, 60)
            logger.info(f"Publicando comando OTA no tópico MQTT '{topic}'...")
            client.publish(topic, payload, qos=1)
            time.sleep(0.5)
            client.disconnect()
            logger.info("Comando OTA publicado com sucesso via Mosquitto!")
            return True
        except Exception as e:
            logger.error(f"Falha ao publicar comando OTA via Mosquitto ({host}:{port}): {e}")
            return False


def _dispatch_ota(gw: EdgeBenchGateway, url: str):
    target_str = input("Digite o ID da bancada alvo (1 a 65535, ou 0 para TODAS) [0]: ").strip()
    target_id = int(target_str) if target_str.isdigit() else 0

    print("\nEscolha o canal de transmissão do comando:")
    print("  [1] Apenas via Rádio LoRa (Central USB -> Bancadas)")
    print(f"  [2] Apenas via Broker MQTT Mosquitto local ({DEFAULT_MQTT_HOST}:{DEFAULT_MQTT_PORT})")
    print("  [3] Ambos (LoRa + MQTT Mosquitto - Máxima Redundância) [Padrão]")
    channel = input("Opcao [3]: ").strip() or "3"

    print(f"\n[INFO] Disparando OTA para URL: {url} (Alvo ID: {target_id or 'Todas'})...")

    if channel in ("1", "3"):
        res = gw.trigger_ota_lora(url, target_bench_id=target_id)
        print(f"-> Resposta Gateway LoRa: {res}")

    if channel in ("2", "3"):
        target_topic = f"fabrica/bancada_{target_id}/ota" if target_id != 0 else "fabrica/todas/ota"
        print(f"\n[MQTT] Publicando ordem de OTA no tópico: '{target_topic}' via Mosquitto ({DEFAULT_MQTT_HOST}:{DEFAULT_MQTT_PORT})...")
        res_mqtt = gw.publish_ota_mqtt(url, target_bench_id=target_id)
        print(f"-> Envio via Broker Mosquitto ({DEFAULT_MQTT_HOST}:{DEFAULT_MQTT_PORT}): {'Sucesso' if res_mqtt else 'Falha'}")

    print("\n[INFO] Comando enviado! As bancadas iniciarão o download via Wi-Fi")
    print("Monitore as requisições HTTP do download nos logs da aplicação\n")


def ota_management_menu(gw: EdgeBenchGateway, ota_server: LocalOTAServer):
    while True:
        local_ip = LocalOTAServer.get_local_ip()
        # Suporta tanto firmware_bancada.bin quanto firmware.bin gerado pelo ESP-IDF
        bin_name = "firmware_bancada.bin" if os.path.exists(os.path.join(ota_server.directory, "firmware_bancada.bin")) else "firmware.bin"
        bin_path = os.path.join(ota_server.directory, bin_name)
        has_bin = os.path.exists(bin_path)
        bin_size_kb = os.path.getsize(bin_path) // 1024 if has_bin else 0
        server_status = f"ATIVO em http://{local_ip}:{ota_server.port}" if ota_server.is_running else "PARADO"

        print("\n" + "=" * 60)
        print("    EdgeBench - Gerenciador de Atualização OTA")
        print("=" * 60)
        print(f" IP Local da Máquina  : {local_ip}")
        print(f" Arquivo ({bin_name}) : {'ENCONTRADO (' + str(bin_size_kb) + ' KB)' if has_bin else 'NÃO ENCONTRADO em ' + bin_path}")
        print(f" Servidor HTTP Local  : [{server_status}]")
        print("-" * 60)
        print("  [1] Disparo Rápido com Servidor Local Automático (Recomendado)")
        print("  [2] Disparar OTA informando URL personalizada")
        print("  [3] Alternar Servidor HTTP Local (Iniciar / Parar)")
        print("  [0] Voltar ao menu principal")

        sub_choice = input("\nOpcao: ").strip()
        if sub_choice == "1":
            if not has_bin:
                print(f"\n[ERRO] Arquivo {bin_path} não encontrado")
                print("Execute 'idf.py build' na pasta firmware_bancada primeiro")
                continue

            if not ota_server.is_running:
                if not ota_server.start():
                    print("[ERRO] Falha ao iniciar servidor HTTP local")
                    continue

            url = ota_server.get_firmware_url(bin_name)
            _dispatch_ota(gw, url)

        elif sub_choice == "2":
            url = input("\nDigite a URL completa do firmware_bancada.bin: ").strip()
            if not url:
                print("URL vazia, cancelando")
                continue
            _dispatch_ota(gw, url)

        elif sub_choice == "3":
            if ota_server.is_running:
                ota_server.stop()
                print("-> Servidor HTTP parado")
            else:
                if ota_server.start():
                    print(f"-> Servidor HTTP iniciado em {ota_server.get_firmware_url(bin_name)}")
                else:
                    print("-> Falha ao iniciar servidor HTTP")

        elif sub_choice == "0":
            break
        else:
            print("Opcao invalida")


def interactive_menu(port: Optional[str] = None):
    # interface de linha de comando
    print("=" * 60)
    print("    EdgeBench - Painel Serial Central")
    print("=" * 60)

    gw = EdgeBenchGateway(port=port)
    if not gw.connect():
        print(f"\n[ERRO] Nao foi possivel conectar na porta serial {gw.port or '(nenhuma detectada)'}")
        print("Verifique se o ESP32 Central esta conectado via cabo USB")
        sys.exit(1)

    ota_server = LocalOTAServer(DEFAULT_BUILD_DIR, port=8080)

    try:
        while True:
            print("\nSelecione uma operacao:")
            print("  [1] Testar conexao com Gateway (Ping local)")
            print("  [2] Ping Broadcast LoRa (Descobrir todas as bancadas online: MAC e ID)")
            print("  [3] Sincronizar Horario do PC (Emitir Beacon de Horario)")
            print("  [4] Reconfigurar Wi-Fi das bancadas via LoRa")
            print("  [5] Reconfigurar Broker MQTT das bancadas via LoRa")
            print("  [6] Reconfigurar ID de Bancada via LoRa (identificando pelo ID atual)")
            print("  [7] Consultar MAC de uma Bancada via LoRa")
            print("  [8] Monitorar logs contínuos da Serial")
            print("  [9] Modo de Pareamento Rápido (Aguardando botão físico da bancada...)")
            print("  [10] Gerenciar Atualização OTA de Firmware (Servidor Local / LoRa / MQTT)")
            print("  [11] Reconfigurar Tempo de Debounce do Sensor via LoRa")
            print("  [12] Reiniciar ESP32 de Bancada remotamente via LoRa (Reboot)")
            print("  [0] Sair")

            choice = input("\nOpcao: ").strip()

            if choice == "1":
                res = gw.ping()
                print(f"-> Resposta: {res}")
            elif choice == "2":
                print("\n[INFO] Disparando Ping Broadcast via LoRa... Aguardando respostas das bancadas (3.5s)...")
                nodes = gw.ping_broadcast(wait_seconds=3.5)
                print("\n" + "=" * 65)
                print(f" RESUMO DA REDE LORA ({len(nodes)} bancada(s) detectada(s)):")
                print("=" * 65)
                if nodes:
                    print(f"{'Bancada ID':<14} | {'Endereço MAC':<20} | {'Status'}")
                    print("-" * 65)
                    for n in nodes:
                        b_id = n.get("bench_id", 0)
                        mac = n.get("mac", "N/A")
                        status_str = "Configurada" if b_id > 0 else "NÃO CONFIGURADA (Setup pendente)"
                        print(f"{b_id:<14} | {mac:<20} | {status_str}")
                else:
                    print("  Nenhuma bancada respondeu ao ping no tempo limite.")
                print("=" * 65 + "\n")
            elif choice == "3":
                res = gw.sync_time()
                print(f"-> Resposta: {res}")
            elif choice == "4":
                ssid = input("Digite o novo SSID da rede Wi-Fi: ").strip()
                pwd = input("Digite a nova Senha: ").strip()
                if ssid:
                    res = gw.set_wifi(ssid, pwd)
                    print(f"-> Resposta: {res}")
            elif choice == "5":
                local_ip = LocalOTAServer.get_local_ip()
                default_broker = f"mqtt://{local_ip}:{DEFAULT_MQTT_PORT}"
                url = input(
                    f"Digite a nova URL do broker para as bancadas [Enter para Mosquitto local: {default_broker}]: "
                ).strip()
                url = url or default_broker
                res = gw.set_broker(url)
                print(f"-> Resposta: {res}")
            elif choice == "6":
                target_str = input(
                    "Digite o ID ATUAL da bancada que deseja alterar (1 a 65535, ou 0 para qualquer): "
                ).strip()
                new_str = input("Digite o NOVO ID da bancada (1 a 65535): ").strip()
                mac_str = input("Digite o MAC do ESP32 alvo (opcional, Enter para pular): ").strip()
                if new_str.isdigit():
                    new_val = int(new_str)
                    if new_val < 1:
                        print("[ERRO] O novo ID da bancada deve ser maior ou igual a 1 (1 a 65535).")
                    else:
                        tid = int(target_str) if target_str.isdigit() else 0
                        res = gw.set_bench(new_val, target_bench_id=tid, target_mac=mac_str if mac_str else None)
                        print(f"-> Resposta: {res}")
            elif choice == "7":
                target_str = input("Digite o ID da bancada a consultar (1 a 65535, ou 0 para todas): ").strip()
                if target_str.isdigit():
                    tid = int(target_str)
                    res = gw.get_bench_info(tid)
                    print(f"-> Resposta do Gateway: {res}")
                    if tid == 0:
                        print("[INFO] Aguardando respostas LoRa de todas as bancadas (3.0s)...")
                        seen_macs = set()
                        start_t = time.time()
                        count = 0
                        while (time.time() - start_t) < 3.0:
                            try:
                                info = gw._bench_info_queue.get(timeout=0.1)
                                mac = info.get("mac")
                                if mac and mac not in seen_macs:
                                    seen_macs.add(mac)
                                    count += 1
                                    print(f"  [+] [DESCOBERTA #{count}] -> Bancada ID: {info.get('bench_id')}, MAC: {mac}")
                            except queue.Empty:
                                pass
                        if count == 0:
                            print("[AVISO] Nenhuma resposta recebida no tempo limite.")
                    else:
                        print(f"[INFO] Aguardando resposta LoRa da bancada ID {tid} (2.5s)...")
                        try:
                            info = gw._bench_info_queue.get(timeout=2.5)
                            print(f"[DESCOBERTA] -> Bancada ID: {info.get('bench_id')}, MAC: {info.get('mac')}")
                        except queue.Empty:
                            print(f"[AVISO] Nenhuma resposta recebida da bancada ID {tid} no tempo limite")
            elif choice == "8":
                print("\n" + "=" * 65)
                print(f" [MONITOR SERIAL CONTÍNUO] Escutando porta {gw.port or 'serial'}")
                print(" -> Logs do ESP32 e eventos LoRa aparecerão abaixo em tempo real")
                print(" -> Quando nao ha comunicacao, o rádio LoRa fica em silencio")
                print(" -> O monitor emitira um batimento (heartbeat) a cada 10s de inatividade")
                print(" -> Pressione Ctrl+C a qualquer momento para retornar ao menu")
                print("=" * 65 + "\n")
                gw._monitoring = True
                while not gw._monitor_queue.empty():
                    try:
                        gw._monitor_queue.get_nowait()
                    except queue.Empty:
                        break

                import datetime
                last_activity = time.time()
                last_heartbeat = time.time()

                try:
                    while True:
                        if not gw.is_alive():
                            print("\n[ALERTA CRÍTICO] Conexão serial com o ESP32 Central foi interrompida")
                            break

                        try:
                            line = gw._monitor_queue.get(timeout=0.2)
                            now_str = datetime.datetime.now().strftime("%H:%M:%S")
                            print(f"[{now_str}] [ESP32] {line}")
                            last_activity = time.time()
                            last_heartbeat = time.time()
                        except queue.Empty:
                            now = time.time()
                            # Emite heartbeat periódico após 10 segundos sem mensagens
                            if now - last_heartbeat >= 10.0:
                                now_str = datetime.datetime.now().strftime("%H:%M:%S")
                                idle_sec = int(now - last_activity)
                                print(f"[{now_str}] [STATUS] Monitor ativo em {gw.port} | Em espera (sem tráfego há {idle_sec}s)")
                                last_heartbeat = now
                except KeyboardInterrupt:
                    print("\n[INFO] Monitor serial encerrado. Retornando ao menu...")
                finally:
                    gw._monitoring = False
            elif choice == "9":
                print("\n" + "=" * 60)
                print(" [MODO PAREAMENTO] Aguardando acionamento do botão físico (3s)...")
                print(" Vá até a bancada física e segure o botão PRG por 3 segundos")
                print(" Pressione Ctrl+C a qualquer momento para cancelar e voltar ao menu")
                print("=" * 60 + "\n")
                while not gw._pairing_queue.empty():
                    try:
                        gw._pairing_queue.get_nowait()
                    except queue.Empty:
                        break
                try:
                    while True:
                        try:
                            pairing_data = gw._pairing_queue.get(timeout=0.1)
                        except queue.Empty:
                            pairing_data = None

                        if pairing_data:
                            try:
                                mac = pairing_data.get("mac")
                                current_id = pairing_data.get("bench_id")
                                id_display = f"{current_id} (Não configurado)" if current_id == 0 else f"{current_id}"
                                print("\n" + "*" * 60)
                                print(" [NOVA BANCADA DETECTADA VIA BOTÃO]")
                                print(f" Endereço MAC : {mac}")
                                print(f" ID Atual     : {id_display}")
                                print("*" * 60)
                                new_id_str = input(
                                    f"\nDigite o NOVO ID para esta bancada (Enter para manter {current_id}): "
                                ).strip()
                                if new_id_str.isdigit():
                                    new_id = int(new_id_str)
                                    if new_id < 1:
                                        print("[ERRO] O ID da bancada deve ser maior ou igual a 1 (0 é reservado para não configurado).")
                                    else:
                                        res = gw.set_bench(new_id, target_bench_id=0, target_mac=mac)
                                        print(f"-> Resposta da Central: {res}")
                                        print(f"-> Bancada {mac} configurada com sucesso com ID {new_id}!\n")
                                else:
                                    print("Nenhuma alteração realizada")
                                print("Continuando no modo pareamento... (Aguardando próxima bancada)")
                            except Exception as e:
                                print(f"Erro ao processar anúncio de pareamento: {e}")
                except KeyboardInterrupt:
                    print("\n[INFO] Modo de pareamento encerrado, retornando ao menu")
            elif choice == "10":
                ota_management_menu(gw, ota_server)
            elif choice == "11":
                deb_str = input("Digite o novo tempo de debounce em milissegundos (10 a 5000 ms) [300]: ").strip() or "300"
                if deb_str.isdigit():
                    debounce_val = int(deb_str)
                    if debounce_val < 10 or debounce_val > 5000:
                        print("[ERRO] O tempo de debounce deve estar entre 10 e 5000 ms.")
                    else:
                        target_str = input(
                            "Digite o ID da bancada alvo (1 a 65535, ou 0 para TODAS) [0]: "
                        ).strip()
                        tid = int(target_str) if target_str.isdigit() else 0
                        mac_str = input("Digite o MAC do ESP32 alvo (opcional, Enter para pular): ").strip()
                        res = gw.set_debounce(debounce_val, target_bench_id=tid, target_mac=mac_str if mac_str else None)
                        print(f"-> Resposta: {res}")
                else:
                    print("[ERRO] Valor numérico inválido")
            elif choice == "12":
                target_str = input(
                    "Digite o ID da bancada a reiniciar (1 a 65535, ou 0 para TODAS) [0]: "
                ).strip()
                tid = int(target_str) if target_str.isdigit() else 0
                mac_str = input("Digite o MAC do ESP32 alvo (opcional, Enter para pular): ").strip()
                confirm = input(f"Confirma envio de comando REBOOT para bancada(s) (Alvo ID: {tid or 'Todas'})? [s/N]: ").strip().lower()
                if confirm == "s":
                    res = gw.reboot_bench(target_bench_id=tid, target_mac=mac_str if mac_str else None)
                    print(f"-> Resposta: {res}")
                else:
                    print("Operação cancelada")
            elif choice == "0":
                print("Encerrando...")
                break
            else:
                print("Opcao invalida")
    finally:
        ota_server.stop()
        gw.disconnect()


if __name__ == "__main__":
    # ermite passar a porta como argumento: python uart_serial.py COM3
    selected_port = sys.argv[1] if len(sys.argv) > 1 else None
    interactive_menu(port=selected_port)