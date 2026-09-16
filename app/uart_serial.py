"""
Módulo de Comunicação Serial entre um computador e um ESP32

Permite:
1. Sincronizar o relógio do ESP32 em uma bancada com o horário do PC
2. Enviar comandos pelo ar (mudança de Wi-Fi, Broker MQTT e ID da Bancada)
3. Testar a conectividade e monitorar a o status do ESP32 via porta serial
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

# garante que o módulo config da pasta app seja importável
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from config import mqtt_config
    DEFAULT_MQTT_HOST = mqtt_config.host
    DEFAULT_MQTT_PORT = mqtt_config.port
except Exception:
    DEFAULT_MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
    DEFAULT_MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

DEFAULT_BUILD_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "firmware", "build")
)


class LocalOTAServer:
    """Servidor HTTP local em background para servir o binário firmware.bin para OTA"""

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

    def get_firmware_url(self) -> str:
        local_ip = self.get_local_ip()
        return f"http://{local_ip}:{self.port}/firmware.bin"


class EdgeBenchGateway:
    """classe responsável por gerenciar a comunicação serial com o ESP32"""

    def __init__(self, port: Optional[str] = None, baudrate: int = 115200, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None
        self._rx_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._rx_thread = None
        self._cmd_resp_queue = queue.Queue()

    @staticmethod
    def list_available_ports() -> List[str]:
        # lista todas as portas seriais disponíveis no sistema operacional
        if serial is None:
            return []
        ports = serial.tools.list_ports.comports()
        return [p.device for p in ports]

    def auto_detect_port(self) -> Optional[str]:
        # tenta identificar a porta serial conectada ao ESP32
        if serial is None:
            return None

        ports = serial.tools.list_ports.comports()
        for p in ports:
            desc = (p.description or "").lower()
            hwid = (p.hwid or "").lower()
            # padrões comuns de identificação do ESP32 (CP210x, CH340, USB JTAG/serial)
            if any(k in desc or k in hwid for k in ["cp210", "ch340", "ch341", "usb-serial", "esp32", "jtag"]):
                logger.info(f"Porta do ESP32 detectada: {p.device} ({p.description})")
                return p.device

        if ports:
            logger.info(f"Usando primeira porta disponivel: {ports[0].device} ({ports[0].description})")
            return ports[0].device

        return None

    def connect(self) -> bool:
        # abre a conexão com a porta serial configurada
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
                timeout=self.timeout,
                write_timeout=self.timeout,
            )
            time.sleep(1.5)
            self.ser.reset_input_buffer()
            
            self._stop_event.clear()
            self._rx_thread = threading.Thread(target=self._background_rx_loop, daemon=True)
            self._rx_thread.start()

            logger.info(f"Conexao estabelecida com sucesso na porta {self.port}")
            # sincroniza o relógio da Central automaticamente ao conectar
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
            with self._rx_lock:
                if self.ser and self.ser.is_open and self.ser.in_waiting > 0:
                    has_data = True
                    try:
                        raw_line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                    except Exception:
                        raw_line = ""

            if raw_line:
                logger.debug(f"RX Serial <- {raw_line}")
                if raw_line.startswith("{") and raw_line.endswith("}"):
                    try:
                        j = json.loads(raw_line)
                        if j.get("type") == "telemetry":
                            self._publish_telemetry(j)
                        elif j.get("event") == "req_time":
                            logger.info("[EVENT] Central solicitou horario (bancada ou boot). Enviando timestamp do PC...")
                            self.sync_time()
                        elif "status" in j:
                            self._cmd_resp_queue.put(j)
                    except Exception as e:
                        logger.error(f"Erro no parse de JSON da serial: {e}")
            if not has_data:
                time.sleep(0.02)

    def _publish_telemetry(self, data):
        if mqtt is None:
            logger.warning("[MQTT] Biblioteca paho-mqtt nao disponível, impossível publicar telemetria")
            return

        bench_id = data.get("bench_id", 1)
        count = data.get("count", 0)
        ts = data.get("timestamp", int(time.time()))

        # Formata data legível compatível com o padrão do EdgeBench
        from datetime import datetime
        try:
            time_str = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")
        except Exception:
            time_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        # Tópico padrão de produção do EdgeBench (escutado pelo backend e dashboard)
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
        # fecha a porta serial com segurança
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
        # envia um comando formatado em JSON para o ESP32 e aguarda a resposta JSON
        if not self.ser or not self.ser.is_open:
            logger.error("Porta serial nao esta aberta")
            return None

        # esvazia respostas residuais anteriores da fila
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

    # comandos específicos do EdgeBench

    def ping(self) -> Optional[Dict[str, Any]]:
        # testa se o ESP32 está online
        logger.info("Enviando comando PING")
        return self.send_command({"cmd": "ping"})

    def sync_time(self, timestamp: Optional[int] = None) -> Optional[Dict[str, Any]]:
        # sincroniza o relógio do ESP32 com o timestamp Epoch Unix informado
        # se nenhum timestamp for passado, utiliza o horário atual do computador
        ts = int(time.time()) if timestamp is None else int(timestamp)
        logger.info(f"Sincronizando horario do Gateway com Epoch {ts}...")
        return self.send_command({"cmd": "sync_time", "timestamp": ts})

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
        bin_path = os.path.join(ota_server.directory, "firmware.bin")
        has_bin = os.path.exists(bin_path)
        bin_size_kb = os.path.getsize(bin_path) // 1024 if has_bin else 0
        server_status = f"ATIVO em http://{local_ip}:{ota_server.port}" if ota_server.is_running else "PARADO"

        print("\n" + "=" * 60)
        print("    EdgeBench - Gerenciador de Atualização OTA")
        print("=" * 60)
        print(f" IP Local da Máquina  : {local_ip}")
        print(f" Arquivo firmware.bin : {'ENCONTRADO (' + str(bin_size_kb) + ' KB)' if has_bin else 'NÃO ENCONTRADO em ' + bin_path}")
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
                print("Execute 'idf.py build' na pasta firmware primeiro")
                continue

            if not ota_server.is_running:
                if not ota_server.start():
                    print("[ERRO] Falha ao iniciar servidor HTTP local")
                    continue

            url = ota_server.get_firmware_url()
            _dispatch_ota(gw, url)

        elif sub_choice == "2":
            url = input("\nDigite a URL completa do firmware.bin: ").strip()
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
                    print(f"-> Servidor HTTP iniciado em http://{local_ip}:{ota_server.port}/firmware.bin")
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
            print("  [1] Testar conexao (Ping)")
            print("  [2] Sincronizar Horario do PC (Emitir Beacon de Horario)")
            print("  [3] Reconfigurar Wi-Fi das bancadas via LoRa")
            print("  [4] Reconfigurar Broker MQTT das bancadas via LoRa")
            print("  [5] Reconfigurar ID de Bancada via LoRa (identificando pelo ID atual)")
            print("  [6] Consultar MAC de uma Bancada via LoRa")
            print("  [7] Monitorar logs contínuos da Serial")
            print("  [8] Modo de Pareamento Rápido (Aguardando botão físico da bancada...)")
            print("  [9] Gerenciar Atualização OTA de Firmware (Servidor Local / LoRa / MQTT)")
            print("  [10] Reconfigurar Tempo de Debounce do Sensor via LoRa")
            print("  [0] Sair")

            choice = input("\nOpcao: ").strip()

            if choice == "1":
                res = gw.ping()
                print(f"-> Resposta: {res}")
            elif choice == "2":
                res = gw.sync_time()
                print(f"-> Resposta: {res}")
            elif choice == "3":
                ssid = input("Digite o novo SSID da rede Wi-Fi: ").strip()
                pwd = input("Digite a nova Senha: ").strip()
                if ssid:
                    res = gw.set_wifi(ssid, pwd)
                    print(f"-> Resposta: {res}")
            elif choice == "4":
                local_ip = LocalOTAServer.get_local_ip()
                default_broker = f"mqtt://{local_ip}:{DEFAULT_MQTT_PORT}"
                url = input(
                    f"Digite a nova URL do broker para as bancadas [Enter para Mosquitto local: {default_broker}]: "
                ).strip()
                url = url or default_broker
                res = gw.set_broker(url)
                print(f"-> Resposta: {res}")
            elif choice == "5":
                target_str = input(
                    "Digite o ID ATUAL da bancada que deseja alterar (1 a 65535, ou 0 para qualquer): "
                ).strip()
                new_str = input("Digite o NOVO ID da bancada (1 a 65535): ").strip()
                mac_str = input("Digite o MAC do ESP32 alvo (opcional, Enter para pular): ").strip()
                if new_str.isdigit():
                    tid = int(target_str) if target_str.isdigit() else 0
                    res = gw.set_bench(int(new_str), target_bench_id=tid, target_mac=mac_str if mac_str else None)
                    print(f"-> Resposta: {res}")
            elif choice == "6":
                target_str = input("Digite o ID da bancada a consultar (1 a 65535, ou 0 para todas): ").strip()
                if target_str.isdigit():
                    res = gw.get_bench_info(int(target_str))
                    print(f"-> Resposta do Gateway: {res}")
                    print("[INFO] Aguardando resposta LoRa da bancada...")
                    start_t = time.time()
                    while (time.time() - start_t) < 2.0:
                        with gw._rx_lock:
                            if gw.ser and gw.ser.in_waiting > 0:
                                line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
                            else:
                                line = None
                            if line.startswith("{") and "bench_info" in line:
                                print(f"[DESCOBERTA] -> {line}")
                                break
                            elif line:
                                print(f"[ESP32] {line}")
                        time.sleep(0.05)
            elif choice == "7":
                print("\n[INFO] Monitorando porta serial... Pressione Ctrl+C para voltar ao menu.")
                try:
                    while True:
                        with gw._rx_lock:
                            if gw.ser and gw.ser.in_waiting > 0:
                                line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
                            else:
                                line = None
                            if line:
                                print(f"[ESP32] {line}")
                        time.sleep(0.05)
                except KeyboardInterrupt:
                    print("\n[INFO] Retornando ao menu")
            elif choice == "8":
                print("\n" + "=" * 60)
                print(" [MODO PAREAMENTO] Aguardando acionamento do botão físico (3s)...")
                print(" Vá até a bancada física e segure o botão PRG por 3 segundos")
                print(" Pressione Ctrl+C a qualquer momento para cancelar e voltar ao menu")
                print("=" * 60 + "\n")
                try:
                    while True:
                        with gw._rx_lock:
                            if gw.ser and gw.ser.in_waiting > 0:
                                line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
                            else:
                                line = None
                            if line.startswith("{") and "pairing" in line:
                                try:
                                    pairing_data = json.loads(line)
                                    mac = pairing_data.get("mac")
                                    current_id = pairing_data.get("bench_id")
                                    print("\n" + "*" * 60)
                                    print(" [NOVA BANCADA DETECTADA VIA BOTÃO]")
                                    print(f" Endereço MAC : {mac}")
                                    print(f" ID Atual     : {current_id}")
                                    print("*" * 60)
                                    new_id_str = input(
                                        f"\nDigite o NOVO ID para esta bancada (Enter para manter {current_id}): "
                                    ).strip()
                                    if new_id_str.isdigit():
                                        new_id = int(new_id_str)
                                        res = gw.set_bench(new_id, target_bench_id=0, target_mac=mac)
                                        print(f"-> Resposta da Central: {res}")
                                        print(f"-> Bancada {mac} configurada com sucesso com ID {new_id}!\n")
                                    else:
                                        print("Nenhuma alteração realizada")
                                    print("Continuando no modo pareamento... (Aguardando próxima bancada)")
                                except Exception as e:
                                    print(f"Erro ao processar anúncio de pareamento: {e}")
                            elif line:
                                print(f"[ESP32] {line}")
                        time.sleep(0.05)
                except KeyboardInterrupt:
                    print("\n[INFO] Modo de pareamento encerrado, retornando ao menu")
            elif choice == "9":
                ota_management_menu(gw, ota_server)
            elif choice == "10":
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