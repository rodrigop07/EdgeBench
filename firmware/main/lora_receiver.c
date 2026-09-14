#include "lora_receiver.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "nvs_manager.h"
#include "wifi_manager.h"
#include <string.h>
#include <sys/time.h>

// tag para logs
static const char *TAG = "LORA_RX";
// handle do barramento SPI
static spi_device_handle_t s_lora_spi = NULL;
// handle para a ISR saber qual tarefa acordar
static TaskHandle_t s_lora_task_handle = NULL;

void lora_process_packet(const uint8_t *payload, size_t length) {
    if (payload == NULL || length < 2) {
        // pacote inválido ou vazio
        return;
    }

    // valdia se o pacote pertence ao EdgeBench
    if (payload[0] != LORA_ESPECIAL_BYTE) {
        ESP_LOGD(TAG, "Pacote ignorado: byte invalido (0x%02X)", payload[0]);
        return;
    }

    // tipo de mensagem
    uint8_t msg_type = payload[1];

    // trata pacote de sincronização de horário
    if (msg_type == LORA_MSG_TIME_BEACON) {
        // tamamnho esperado: 1 byte especial + 1 byte tipo + 8 bytes timestamp = 10 bytes
        if (length < 10) {
            ESP_LOGW(TAG, "Pacote Time Beacon com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        // copia o epoch do pacote
        uint64_t epoch = 0;
        memcpy(&epoch, &payload[2], sizeof(uint64_t));

        // valida se o epoch é válido (ano >= 2024 / 1704067200 epoch)
        if (epoch >= 1704067200ULL) {
            // estrutura de tempo
            struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};

            // ajusta o relógio interno do esp
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Horário sincronizado com sucesso via Beacon LoRa (Epoch: %llu)", (unsigned long long)epoch);
        } else {
            ESP_LOGW(TAG, "Beacon LoRa descartado: Epoch não calibrado pela Central (%llu)", (unsigned long long)epoch);
        }
    } else if (msg_type == LORA_MSG_SET_BROKER) {
        // trata pacote de atualização da URL do Broker
        // tamanho esperado: 1 byte especial + 1 byte tipo + 4 bytes token + 1 byte tamanho_url = 7 bytes mínimos
        if (length < 7) {
            ESP_LOGW(TAG, "Pacote Set Broker com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        // copia o token de segurança
        uint32_t token = 0;
        memcpy(&token, &payload[2], sizeof(uint32_t));

        // validação de segurança
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "Tentativa de configurar Broker rejeitada: Token inválido (0x%08lX)", (unsigned long)token);
            return;
        }

        // copia o tamanho da url
        uint8_t url_len = payload[6];
        // validação do tamanho da url
        if (length < (size_t)(7 + url_len) || url_len > 128) {
            ESP_LOGW(TAG, "Comprimento da URL inválido (%d)", url_len);
            return;
        }

        // cria a nova url
        char nova_url[129] = {0};
        memcpy(nova_url, &payload[7], url_len);
        nova_url[url_len] = '\0';

        ESP_LOGI(TAG, "Novo Broker recebido via LoRa: %s", nova_url);

        // grava a nova url na NVS
        esp_err_t err = nvs_manager_set_broker_url(nova_url);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Broker atualizado na NVS com sucesso, aplicando nova URL...");
            mqtt_manager_set_broker(nova_url);
        } else {
            ESP_LOGE(TAG, "Falha ao gravar URL Broker na NVS (%s)", esp_err_to_name(err));
        }
    } else if (msg_type == LORA_MSG_SET_WIFI) {
        // tamanho esperado: 1 byte especial + 1 byte tipo + 4 bytes token + 1 byte tamanho_ssid + 1 byte tamanho_pass =
        // 8 bytes
        if (length < 8) {
            ESP_LOGW(TAG, "Pacote Set Wi-Fi com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }
        // copia o token de segurança
        uint32_t token = 0;
        memcpy(&token, &payload[2], sizeof(uint32_t));

        // validação de segurança
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "Tentativa de configurar Wi-Fi rejeitada: Token inválido (0x%08lX)", (unsigned long)token);
            return;
        }

        // copia o tamanho do ssid
        uint8_t ssid_len = payload[6];
        // validação do tamanho do ssid (1 a 32 caracteres)
        if (ssid_len == 0 || ssid_len > 32 || length < (size_t)(7 + ssid_len + 1)) {
            ESP_LOGW(TAG, "Tamanho de SSID inválido (%d)", ssid_len);
            return;
        }

        // cria o novo ssid
        char novo_ssid[33] = {0};
        memcpy(novo_ssid, &payload[7], ssid_len);
        novo_ssid[ssid_len] = '\0';

        // posição do tamanho da senha
        size_t pass_offset = 7 + ssid_len;
        uint8_t pass_len = payload[pass_offset];

        // validação do tamanho da senha (0 a 64 caracteres)
        if (pass_len > 64 || length < (pass_offset + 1 + pass_len)) {
            ESP_LOGW(TAG, "Tamanho de senha Wi-Fi inválido (%d)", pass_len);
            return;
        }

        // cria a nova senha
        char nova_senha[65] = {0};
        if (pass_len > 0) {
            memcpy(nova_senha, &payload[pass_offset + 1], pass_len);
        }
        nova_senha[pass_len] = '\0';
        ESP_LOGI(TAG, "Novas credenciais Wi-Fi recebidas via LoRa, SSID: %s", novo_ssid);

        // grava na NVS e reconecta sem reiniciar
        esp_err_t err = nvs_manager_set_wifi_credentials(novo_ssid, nova_senha);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi atualizado na Flash, novo SSID: '%s'...", novo_ssid);
            wifi_manager_reconfigure(novo_ssid, nova_senha);
        } else {
            ESP_LOGE(TAG, "Falha ao gravar Wi-Fi na NVS (%s)", esp_err_to_name(err));
        }
    } else if (msg_type == LORA_MSG_SET_BENCH) {
        // tamanho esperado: 1 byte especial + 1 byte tipo + 4 bytes token + 2 bytes bench_id = 8 bytes
        if (length < 8) {
            ESP_LOGW(TAG, "Pacote Set Bench com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        uint32_t token = 0;
        memcpy(&token, &payload[2], sizeof(uint32_t));
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "Tentativa de configurar ID da Bancada rejeitada: Token inválido (0x%08lX)",
                     (unsigned long)token);
            return;
        }

        uint16_t novo_bench_id = 0;
        memcpy(&novo_bench_id, &payload[6], sizeof(uint16_t));
        ESP_LOGI(TAG, "Novo ID de Bancada recebido via LoRa: %u", novo_bench_id);

        // grava na NVS e atualiza topicos sem reiniciar
        esp_err_t err = nvs_manager_set_bench_id(novo_bench_id);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "ID da Bancada atualizado na NVS com sucesso! Aplicando ID %u...", novo_bench_id);
            mqtt_manager_set_bench_id(novo_bench_id);
        } else {
            ESP_LOGE(TAG, "Falha ao gravar bench_id na NVS (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Tipo de mensagem LoRa desconhecido (0x%02X)", msg_type);
    }
}

// opcodes de comando do transceptor SX1262
#define SX126X_CMD_SET_STANDBY 0x80
#define SX126X_CMD_SET_RX 0x82
#define SX126X_CMD_SET_PACKET_TYPE 0x8A
#define SX126X_CMD_SET_RF_FREQUENCY 0x86
#define SX126X_CMD_SET_REGULATOR_MODE 0x96
#define SX126X_CMD_SET_BUFFER_BASE_ADDR 0x8F
#define SX126X_CMD_SET_MODULATION_PARAMS 0x8B
#define SX126X_CMD_SET_PACKET_PARAMS 0x8C
#define SX126X_CMD_SET_DIO_IRQ_PARAMS 0x08
#define SX126X_CMD_GET_IRQ_STATUS 0x12
#define SX126X_CMD_CLEAR_IRQ_STATUS 0x02
#define SX126X_CMD_SET_DIO2_AS_RF_SWITCH 0x9D
#define SX126X_CMD_SET_DIO3_AS_TCXO_CTRL 0x97
#define SX126X_CMD_CALIBRATE 0x89
#define SX126X_CMD_CALIBRATE_IMAGE 0x98
#define SX126X_CMD_GET_RX_BUFFER_STATUS 0x13
#define SX126X_CMD_READ_BUFFER 0x1E
#define LORA_PIN_VEXT 36

// função auxiliar para aguardar o rádio terminar de processar operações internas
static void sx1262_wait_busy(void) {
    int timeout_ms = 1000;
    while (gpio_get_level(LORA_PIN_BUSY) == 1 && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }
    if (timeout_ms <= 0) {
        ESP_LOGE(TAG, "Timeout aguardando pino BUSY do SX1262");
    }
}

/**
 * @brief envia comando SPI para o chip SX1262
 */
static esp_err_t sx1262_write_command(uint8_t opcode, const uint8_t *data, size_t length) {
    sx1262_wait_busy();

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    uint8_t tx_buf[64];
    tx_buf[0] = opcode;
    if (data != NULL && length > 0) {
        if (length + 1 > sizeof(tx_buf)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&tx_buf[1], data, length);
    }

    t.length = (1 + length) * 8;
    t.tx_buffer = tx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
    return ret;
}

/**
 * @brief consulta o status de interrupções (IRQ) do SX1262
 */
static uint16_t sx1262_get_irq_status(void) {
    sx1262_wait_busy();

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    uint8_t tx_buf[4] = {SX126X_CMD_GET_IRQ_STATUS, 0x00, 0x00, 0x00};
    uint8_t rx_buf[4] = {0};

    t.length = 4 * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
    if (ret != ESP_OK) {
        return 0;
    }
    return ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
}

/**
 * @brief limpa flags de interrupção no SX1262
 */
static esp_err_t sx1262_clear_irq_status(uint16_t irq_mask) {
    uint8_t data[2] = {(uint8_t)(irq_mask >> 8), (uint8_t)(irq_mask & 0xFF)};
    return sx1262_write_command(SX126X_CMD_CLEAR_IRQ_STATUS, data, 2);
}

/**
 * @brief coloca o SX1262 em modo de recepção contínua ou temporizada
 */
static esp_err_t sx1262_set_rx(uint32_t timeout) {
    uint8_t data[3] = {(uint8_t)((timeout >> 16) & 0xFF), (uint8_t)((timeout >> 8) & 0xFF), (uint8_t)(timeout & 0xFF)};
    return sx1262_write_command(SX126X_CMD_SET_RX, data, 3);
}

/**
 * @brief obtém comprimento e offset do pacote recebido no buffer FIFO
 */
static esp_err_t sx1262_get_rx_buffer_status(uint8_t *out_payload_len, uint8_t *out_rx_start_ptr) {
    sx1262_wait_busy();

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    uint8_t tx_buf[4] = {SX126X_CMD_GET_RX_BUFFER_STATUS, 0x00, 0x00, 0x00};
    uint8_t rx_buf[4] = {0};

    t.length = 4 * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
    if (ret != ESP_OK) {
        return ret;
    }

    if (out_payload_len != NULL) {
        *out_payload_len = rx_buf[2];
    }
    if (out_rx_start_ptr != NULL) {
        *out_rx_start_ptr = rx_buf[3];
    }
    return ESP_OK;
}

/**
 * @brief lê dados da FIFO do SX1262 via SPI
 */
static esp_err_t sx1262_read_buffer(uint8_t offset, uint8_t *data, size_t length) {
    if (data == NULL || length == 0 || length > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    sx1262_wait_busy();

    size_t total_len = 3 + length;
    uint8_t tx_buf[259];
    uint8_t rx_buf[259];
    memset(tx_buf, 0, total_len);
    memset(rx_buf, 0, total_len);

    tx_buf[0] = SX126X_CMD_READ_BUFFER;
    tx_buf[1] = offset;
    tx_buf[2] = 0x00; // NOP dummy

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = total_len * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
    if (ret == ESP_OK) {
        memcpy(data, &rx_buf[3], length);
    }
    return ret;
}

// rotina de interrupção disparada na borda de subida do pino DIO1 (GPIO 14)
static void IRAM_ATTR lora_dio1_isr_handler(void *arg) {
    BaseType_t high_task_wakeup = pdFALSE;

    // envia a notificação para a tarefa lora_rx_task acordar
    if (s_lora_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_lora_task_handle, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t lora_receiver_init(void) {
    ESP_LOGI(TAG, "Inicializando perifericos do radio LoRa (SX1262)...");

    // configura o pino DIO1 para disparar interrupção quando for para nível alto
    gpio_config_t io_conf_dio = {
        .pin_bit_mask = (1ULL << LORA_PIN_DIO1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf_dio);

    // registra a ISR para o pino DIO1
    gpio_isr_handler_add(LORA_PIN_DIO1, lora_dio1_isr_handler, NULL);

    // configura os pinos GPIO de controle: RST (saída), VEXT (saída), BUSY (entrada)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LORA_PIN_RST) | (1ULL << LORA_PIN_VEXT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // ativa alimentação de periféricos/rádio no Heltec V3 (Vext ativo em nível baixo)
    gpio_set_level(LORA_PIN_VEXT, 0);

    io_conf.pin_bit_mask = (1ULL << LORA_PIN_BUSY);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // configura o barramento SPI
    spi_bus_config_t buscfg = {
        .miso_io_num = LORA_PIN_MISO,
        .mosi_io_num = LORA_PIN_MOSI,
        .sclk_io_num = LORA_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao inicializar barramento SPI (%s)", esp_err_to_name(ret));
        return ret;
    }

    // adiciona o dispositivo SX1262 ao barramento SPI
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2000000, // 2 MHz
        .mode = 0,                 // SPI Modo 0
        .spics_io_num = LORA_PIN_NSS,
        .queue_size = 7,
    };

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_lora_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar dispositivo SPI (%s)", esp_err_to_name(ret));
        return ret;
    }

    // sequência de reset físico do chip SX1262
    gpio_set_level(LORA_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    // aguarda o chip sair do estado ocupado
    sx1262_wait_busy();

    // coloca em Standby RC
    uint8_t standby_mode = 0x00;
    sx1262_write_command(SX126X_CMD_SET_STANDBY, &standby_mode, 1);

    // ativa regulador interno DC-DC
    uint8_t reg_mode = 0x01;
    sx1262_write_command(SX126X_CMD_SET_REGULATOR_MODE, &reg_mode, 1);

    // configura DIO3 para alimentar o oscilador TCXO a 1.8V com delay de 10ms (CRÍTICO para Heltec V3!)
    uint8_t tcxo_params[4] = {0x02, 0x00, 0x02, 0x80};
    sx1262_write_command(SX126X_CMD_SET_DIO3_AS_TCXO_CTRL, tcxo_params, 4);

    // executa calibração interna de todos os blocos com o clock TCXO ativo
    uint8_t calib_param = 0x7F;
    sx1262_write_command(SX126X_CMD_CALIBRATE, &calib_param, 1);

    // calibra rejeição de imagem para a faixa 902-928 MHz (faixa do 915 MHz)
    uint8_t calib_img[2] = {0xE1, 0xE9};
    sx1262_write_command(SX126X_CMD_CALIBRATE_IMAGE, calib_img, 2);

    // configura DIO2 para chavear a antena RF
    uint8_t dio2_switch = 0x01;
    sx1262_write_command(SX126X_CMD_SET_DIO2_AS_RF_SWITCH, &dio2_switch, 1);

    // tipo de pacote: LoRa (0x01)
    uint8_t pkt_type = 0x01;
    sx1262_write_command(SX126X_CMD_SET_PACKET_TYPE, &pkt_type, 1);

    // frequência RF: 915 MHz (0x39300000)
    uint8_t rf_freq[4] = {0x39, 0x30, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_RF_FREQUENCY, rf_freq, 4);

    // buffer base: TxBase=0x00, RxBase=0x00
    uint8_t buf_base[2] = {0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_BUFFER_BASE_ADDR, buf_base, 2);

    // modulação: SF7 (0x07), BW 125kHz (0x04), CR 4/5 (0x01), LDRO off (0x00)
    uint8_t mod_params[4] = {0x07, 0x04, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_MODULATION_PARAMS, mod_params, 4);

    // parâmetros do pacote: preâmbulo 8 (0x0008), header explícito (0x00), max len 255 (0xFF), CRC on (0x01), invert IQ
    // padrão (0x00)
    uint8_t pkt_params[6] = {0x00, 0x08, 0x00, 0xFF, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // configura interrupção DIO1 para RxDone (0x0002)
    uint8_t irq_params[8] = {0x03, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq_params, 8);

    // limpa flags residuais
    sx1262_clear_irq_status(0x03FF);

    // coloca em modo de recepção contínua (0xFFFFFF)
    sx1262_set_rx(0xFFFFFF);

    ESP_LOGI(TAG, "Hardware do SX1262 inicializado e em modo de escuta continua (915 MHz, SF7, BW125)!");
    return ESP_OK;
}

// tarefa executada em segundo plano no Core 0
static void lora_rx_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de recepcao LoRa iniciada no Core %d", xPortGetCoreID());

    uint8_t rx_buffer[256];

    while (1) {
        // aguarda notificação da ISR (DIO1) com timeout de 1 segundo para segurança
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        uint16_t irq = sx1262_get_irq_status();
        if (irq & 0x0002) { // RxDone
            uint8_t payload_len = 0;
            uint8_t rx_start_ptr = 0;
            if (sx1262_get_rx_buffer_status(&payload_len, &rx_start_ptr) == ESP_OK && payload_len > 0) {
                if (sx1262_read_buffer(rx_start_ptr, rx_buffer, payload_len) == ESP_OK) {
                    ESP_LOGI(TAG, "Pacote LoRa recebido via RF! Tamanho: %u bytes", payload_len);
                    lora_process_packet(rx_buffer, payload_len);
                }
            }
            sx1262_clear_irq_status(0x03FF);
            // garante retorno ao modo de escuta contínua
            sx1262_set_rx(0xFFFFFF);
        } else if (irq & 0x0040) { // CrcErr
            ESP_LOGW(TAG, "Pacote recebido descartado: Erro de CRC de RF");
            sx1262_clear_irq_status(0x03FF);
            sx1262_set_rx(0xFFFFFF);
        } else if (irq != 0) {
            sx1262_clear_irq_status(irq);
        }
    }
}

esp_err_t lora_receiver_start_task(void) {
    BaseType_t ret = xTaskCreatePinnedToCore(lora_rx_task, "lora_rx_task", 4096, NULL, 4, &s_lora_task_handle, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa lora_rx_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Tarefa lora_rx_task criada com sucesso no Core 0!");
    return ESP_OK;
}

void lora_receiver_test(void) {
    ESP_LOGI(TAG, "=== INICIANDO TESTE DO PROTOCOLO LORA ===");

    // simula pacote de Beacon de Horário (Timestamp de 12/09/2026)
    uint64_t fake_timestamp = 1789228800ULL;
    uint8_t beacon_pkt[10];
    beacon_pkt[0] = LORA_ESPECIAL_BYTE;
    beacon_pkt[1] = LORA_MSG_TIME_BEACON;
    memcpy(&beacon_pkt[2], &fake_timestamp, sizeof(uint64_t));

    ESP_LOGI(TAG, "[TESTE 1] Injetando pacote Beacon de Horário...");
    lora_process_packet(beacon_pkt, sizeof(beacon_pkt));

    // simula pacote de Alteração de Broker
    const char *nova_url = "mqtt://192.168.1.99:1883";
    uint8_t url_len = strlen(nova_url);
    uint32_t token = LORA_SECURITY_TOKEN;

    uint8_t broker_pkt[7 + url_len];
    broker_pkt[0] = LORA_ESPECIAL_BYTE;
    broker_pkt[1] = LORA_MSG_SET_BROKER;
    memcpy(&broker_pkt[2], &token, sizeof(uint32_t));
    broker_pkt[6] = url_len;
    memcpy(&broker_pkt[7], nova_url, url_len);

    ESP_LOGI(TAG, "[TESTE 2] Injetando pacote de Alteração de Broker...");
    lora_process_packet(broker_pkt, sizeof(broker_pkt));

    ESP_LOGI(TAG, "=== FIM DO TESTE ===");
}
