"""
Módulo de Comunicação Serial entre um computador e um ESP32

Permite:
1. Sincronizar o relógio do ESP32 em uma bancada com o horário do PC
2. Enviar comandos pelo ar (mudança de Wi-Fi, Broker MQTT e ID da Bancada)
3. Testar a conectividade e monitorar a o status do ESP32 via porta serial
"""

import json
import logging
import sys
import time
from typing import Any, Dict, List, Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[AVISO] A biblioteca 'pyserial' nao foi encontrada no ambiente Python atual")
    print("Para instalar, execute: pip install pyserial\n")
    serial = None

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] (%(name)s) %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("EdgeBench_Serial")


class EdgeBenchGateway:
    """classe responsável por gerenciar a comunicação serial com o ESP32"""

    def __init__(self, port: Optional[str] = None, baudrate: int = 115200, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None

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
            # pausa para estabilização
            time.sleep(1.5)
            self.ser.reset_input_buffer()
            logger.info(f"Conexao estabelecida com sucesso na porta {self.port}")
            return True
        except Exception as e:
            logger.error(f"Falha ao abrir porta serial {self.port}: {e}")
            self.ser = None
            return False

    def disconnect(self):
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

        line_to_send = json.dumps(cmd_dict) + "\n"
        try:
            if hasattr(self.ser, "reset_input_buffer"):
                self.ser.reset_input_buffer()
            self.ser.write(line_to_send.encode("utf-8"))
            self.ser.flush()
            logger.debug(f"TX Serial -> {line_to_send.strip()}")
        except Exception as e:
            logger.error(f"Erro ao transmitir comando pela serial: {e}")
            return None

        if not wait_response:
            return None

        # aguarda a resposta JSON do firmware ignorando logs de depuração do IDF
        start_time = time.time()
        while (time.time() - start_time) < self.timeout:
            try:
                raw_line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                if not raw_line:
                    continue

                logger.debug(f"RX Serial <- {raw_line}")

                # verifica se a linha recebida é um JSON válido
                if raw_line.startswith("{") and raw_line.endswith("}"):
                    try:
                        resp_json = json.loads(raw_line)
                        return resp_json
                    except json.JSONDecodeError:
                        continue
            except Exception as e:
                logger.warning(f"Erro ao ler linha da serial: {e}")
                break

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
                url = input("Digite a nova URL do broker (ex: mqtt://192.168.1.100:1883): ").strip()
                if url:
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
                        if gw.ser and gw.ser.in_waiting > 0:
                            line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
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
                        if gw.ser and gw.ser.in_waiting > 0:
                            line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
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
                        if gw.ser and gw.ser.in_waiting > 0:
                            line = gw.ser.readline().decode("utf-8", errors="ignore").strip()
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
            elif choice == "0":
                print("Encerrando...")
                break
            else:
                print("Opcao invalida.")
    finally:
        gw.disconnect()


if __name__ == "__main__":
    # ermite passar a porta como argumento: python uart_serial.py COM3
    selected_port = sys.argv[1] if len(sys.argv) > 1 else None
    interactive_menu(port=selected_port)