#include "lora_receiver.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "nvs_manager.h"
#include "ota_manager.h"
#include "sensor_manager.h"
#include "wifi_manager.h"
#include <string.h>
#include <sys/time.h>

// tag para logs
static const char *TAG = "LORA_RX";
// handle do barramento SPI
static spi_device_handle_t s_lora_spi = NULL;
// handle para a ISR saber qual tarefa acordar
static TaskHandle_t s_lora_task_handle = NULL;
// mutex recursivo para proteger o acesso concorrente ao barramento SPI e ao chip SX1262
static SemaphoreHandle_t s_lora_mutex = NULL;

#define LORA_LOCK()                                                                                                    \
    do {                                                                                                               \
        if (s_lora_mutex)                                                                                              \
            xSemaphoreTakeRecursive(s_lora_mutex, portMAX_DELAY);                                                      \
    } while (0)
#define LORA_UNLOCK()                                                                                                  \
    do {                                                                                                               \
        if (s_lora_mutex)                                                                                              \
            xSemaphoreGiveRecursive(s_lora_mutex);                                                                     \
    } while (0)

// endereço MAC (STA) deste ESP32 para identificação única
static uint8_t s_my_mac[6] = {0};

static bool is_target_mac_me(const uint8_t *target_mac) {
    static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(target_mac, broadcast_mac, 6) == 0) {
        return true;
    }
    if (memcmp(target_mac, s_my_mac, 6) == 0) {
        return true;
    }
    return false;
}

void lora_process_packet(const uint8_t *payload, size_t length) {
    if (payload == NULL || length < 2) {
        // pacote inválido ou vazio
        return;
    }

    // valida se o pacote pertence ao EdgeBench
    if (payload[0] != LORA_ESPECIAL_BYTE) {
        ESP_LOGD(TAG, "Pacote ignorado: byte invalido (0x%02X)", payload[0]);
        return;
    }

    // tipo de mensagem
    uint8_t msg_type = payload[1];

    if (msg_type == LORA_MSG_RESP_TIME) {
        // Formato: [0xEB, 0x11, TARGET_MAC(6B), EPOCH(8B)] = 16 bytes
        if (length < 16) {
            ESP_LOGW(TAG, "Pacote RESP_TIME com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        if (!is_target_mac_me(&payload[2])) {
            ESP_LOGD(TAG, "RESP_TIME direcionado a outro MAC, ignorando");
            return;
        }

        uint64_t epoch = 0;
        memcpy(&epoch, &payload[8], sizeof(uint64_t));

        // valida se o epoch é válido (ano >= 2024 / 1704067200 epoch)
        if (epoch >= 1704067200ULL) {
            struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Horário sincronizado com sucesso via LoRa (Epoch: %llu)", (unsigned long long)epoch);
        } else {
            ESP_LOGW(TAG, "Resposta LoRa de horario com Epoch invalido (%llu)", (unsigned long long)epoch);
        }
    } else if (msg_type == LORA_MSG_RESP_CONFIG) {
        // formato: [0xEB, 0x21, TARGET_MAC(6B), TOKEN(4B), SSID_LEN(1B), SSID, PASS_LEN(1B), PASS, BROKER_LEN(1B),
        // BROKER]
        if (length < 15) {
            ESP_LOGW(TAG, "Pacote RESP_CONFIG com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        if (!is_target_mac_me(&payload[2])) {
            ESP_LOGD(TAG, "RESP_CONFIG direcionado a outro MAC, ignorando");
            return;
        }

        uint32_t token = 0;
        memcpy(&token, &payload[8], sizeof(uint32_t));
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "RESP_CONFIG rejeitado: Token inválido (0x%08lX)", (unsigned long)token);
            return;
        }

        uint8_t ssid_len = payload[12];
        if (ssid_len > 32 || length < (size_t)(13 + ssid_len + 2)) {
            ESP_LOGW(TAG, "RESP_CONFIG com tamanho de SSID inválido (%u)", ssid_len);
            return;
        }

        char ssid[33] = {0};
        if (ssid_len > 0) {
            memcpy(ssid, &payload[13], ssid_len);
        }
        ssid[ssid_len] = '\0';

        size_t pass_offset = 13 + ssid_len;
        uint8_t pass_len = payload[pass_offset];
        if (pass_len > 64 || length < (pass_offset + 1 + pass_len + 1)) {
            ESP_LOGW(TAG, "RESP_CONFIG com tamanho de senha inválido (%u)", pass_len);
            return;
        }

        char pass[65] = {0};
        if (pass_len > 0) {
            memcpy(pass, &payload[pass_offset + 1], pass_len);
        }
        pass[pass_len] = '\0';

        size_t broker_offset = pass_offset + 1 + pass_len;
        uint8_t broker_len = payload[broker_offset];
        if (broker_len > 128 || length < (broker_offset + 1 + broker_len)) {
            ESP_LOGW(TAG, "RESP_CONFIG com tamanho de Broker inválido (%u)", broker_len);
            return;
        }

        char broker[129] = {0};
        if (broker_len > 0) {
            memcpy(broker, &payload[broker_offset + 1], broker_len);
        }
        broker[broker_len] = '\0';

        ESP_LOGI(TAG, "Configurações recebidas via LoRa, SSID: '%s', Broker: '%s'", ssid, broker);

        if (ssid_len > 0) {
            esp_err_t err = nvs_manager_set_wifi_credentials(ssid, pass);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi gravado na NVS, reconectando...");
                wifi_manager_reconfigure(ssid, pass);
            }
        }

        if (broker_len > 0) {
            esp_err_t err = nvs_manager_set_broker_url(broker);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Broker gravado na NVS, atualizando cliente MQTT...");
                mqtt_manager_set_broker(broker);
            }
        }
    } else if (msg_type == LORA_MSG_CMD_SET_BENCH) {
        // formato: [0xEB, 0x30, TARGET_MAC(6B), TARGET_BENCH_ID(2B), TOKEN(4B), NEW_BENCH_ID(2B)] = 16 bytes
        if (length < 16) {
            ESP_LOGW(TAG, "Pacote CMD_SET_BENCH com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        uint32_t token = 0;
        memcpy(&token, &payload[10], sizeof(uint32_t));
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "CMD_SET_BENCH rejeitado: Token inválido (0x%08lX)", (unsigned long)token);
            return;
        }

        uint16_t current_bench_id = 0;
        nvs_manager_get_bench_id(&current_bench_id);

        uint16_t target_bench_id = 0;
        memcpy(&target_bench_id, &payload[8], sizeof(uint16_t));

        static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        bool is_my_mac = (memcmp(&payload[2], s_my_mac, 6) == 0);
        bool is_broadcast_mac = (memcmp(&payload[2], broadcast_mac, 6) == 0);

        // regra de segurança essencial para o setup inicial:
        // 1. se o pacote for endereçado ao MAC desta placa física (is_my_mac):
        //    aceita com total segurança, mesmo se o nó estiver com ID 0 (ainda não configurado)
        // 2. se for MAC broadcast:
        //    target_bench_id DEVE ser >= 1 e coincidir com current_bench_id (que também deve ser >= 1)
        //    NUNCA aceita broadcast com target_bench_id == 0 nem permite configurar nó que esteja com ID 0 por
        //    broadcast, evitando que outras bancadas ainda não configuradas sejam sobrescritas simultaneamente
        if (is_my_mac) {
            // endereçamento individual e explícito por MAC (utilizado no pareamento do botão ou setup)
        } else if (is_broadcast_mac && target_bench_id >= 1 && current_bench_id >= 1 &&
                   target_bench_id == current_bench_id) {
            // ndereçamento por ID de uma bancada já configurada
        } else {
            ESP_LOGD(TAG, "CMD_SET_BENCH ignorado: não coincide com este nó (Meu ID: %u, Alvo ID: %u)",
                     current_bench_id, target_bench_id);
            return;
        }

        uint16_t novo_bench_id = 0;
        memcpy(&novo_bench_id, &payload[14], sizeof(uint16_t));

        // validação: IDs válidos de bancada DEVEM começar do 1 (0 é reservado para não configurado)
        if (novo_bench_id == 0 || novo_bench_id > 9999) {
            ESP_LOGW(TAG, "CMD_SET_BENCH rejeitado: novo_bench_id inválido (%u). Deve ser >= 1", novo_bench_id);
            return;
        }

        ESP_LOGI(TAG, "Novo ID de Bancada recebido via LoRa: %u (Anterior: %u)", novo_bench_id, current_bench_id);

        // grava na NVS e atualiza tópicos sem reiniciar
        esp_err_t err = nvs_manager_set_bench_id(novo_bench_id);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "ID da Bancada atualizado na NVS com sucesso! Aplicando ID %u...", novo_bench_id);
            mqtt_manager_set_bench_id(novo_bench_id);

            // responde confirmação com o novo ID e MAC para a Central registrar
            uint8_t resp[10];
            resp[0] = LORA_ESPECIAL_BYTE;
            resp[1] = LORA_MSG_RESP_BENCH_INFO;
            memcpy(&resp[2], s_my_mac, 6);
            memcpy(&resp[8], &novo_bench_id, sizeof(uint16_t));
            lora_send_packet(resp, sizeof(resp));
        } else {
            ESP_LOGE(TAG, "Falha ao gravar bench_id na NVS (%s)", esp_err_to_name(err));
        }
    } else if (msg_type == LORA_MSG_REQ_BENCH_INFO) {
        // formato: [0xEB, 0x31, TARGET_BENCH_ID(2B)] = 4 bytes
        if (length < 4) {
            return;
        }

        uint16_t req_id = 0;
        memcpy(&req_id, &payload[2], sizeof(uint16_t));

        uint16_t my_id = 0;
        nvs_manager_get_bench_id(&my_id);

        if (req_id == 0 || req_id == my_id) {
            ESP_LOGI(TAG, "Respondendo consulta de MAC para Bancada ID %u com meu MAC...", my_id);
            // formato de resposta: [0xEB, 0x32, MAC(6B), BENCH_ID(2B)] = 10 bytes
            uint8_t resp[10];
            resp[0] = LORA_ESPECIAL_BYTE;
            resp[1] = LORA_MSG_RESP_BENCH_INFO;
            memcpy(&resp[2], s_my_mac, 6);
            memcpy(&resp[8], &my_id, sizeof(uint16_t));
            lora_send_packet(resp, sizeof(resp));
        }
    } else if (msg_type == LORA_MSG_CMD_OTA) {
        // formato: [0xEB, 0x40, TARGET_BENCH_ID(2B), TOKEN(4B), URL_LEN(1B), URL(N_BYTES)] = 9 + N bytes
        if (length < 9) {
            ESP_LOGW(TAG, "Pacote CMD_OTA com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        uint32_t token = 0;
        memcpy(&token, &payload[4], sizeof(uint32_t));
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "CMD_OTA rejeitado: Token invalido (0x%08lX)", (unsigned long)token);
            return;
        }

        uint16_t target_id = 0;
        memcpy(&target_id, &payload[2], sizeof(uint16_t));

        uint16_t my_id = 0;
        nvs_manager_get_bench_id(&my_id);

        if (target_id != 0 && target_id != my_id) {
            ESP_LOGD(TAG, "CMD_OTA direcionado a outra bancada (Alvo: %u, Meu ID: %u), ignorando", target_id, my_id);
            return;
        }

        uint8_t url_len = payload[8];
        if (url_len == 0 || url_len > 180 || length < (size_t)(9 + url_len)) {
            ESP_LOGW(TAG, "CMD_OTA com tamanho de URL invalido (%u)", url_len);
            return;
        }

        char ota_url[192] = {0};
        memcpy(ota_url, &payload[9], url_len);
        ota_url[url_len] = '\0';

        ESP_LOGI(TAG, "Comando de OTA recebido via LoRa, URL: %s", ota_url);
        esp_err_t ret = ota_manager_start(ota_url);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Atualizacao OTA disparada com sucesso via LoRa");
        } else {
            ESP_LOGW(TAG, "Falha ao disparar OTA via LoRa: %s", esp_err_to_name(ret));
        }
    } else if (msg_type == LORA_MSG_CMD_SET_DEBOUNCE) {
        // formato: [0xEB, 0x60, TARGET_MAC(6B), TARGET_BENCH_ID(2B), TOKEN(4B), DEBOUNCE_MS(4B)] = 18 bytes
        if (length < 18) {
            ESP_LOGW(TAG, "Pacote CMD_SET_DEBOUNCE com tamanho insuficiente (%d bytes)", (int)length);
            return;
        }

        uint32_t token = 0;
        memcpy(&token, &payload[10], sizeof(uint32_t));
        if (token != LORA_SECURITY_TOKEN) {
            ESP_LOGW(TAG, "CMD_SET_DEBOUNCE rejeitado: Token invalido (0x%08lX)", (unsigned long)token);
            return;
        }

        uint16_t current_bench_id = 0;
        nvs_manager_get_bench_id(&current_bench_id);

        uint16_t target_bench_id = 0;
        memcpy(&target_bench_id, &payload[8], sizeof(uint16_t));

        bool mac_matches = is_target_mac_me(&payload[2]);
        bool bench_matches = (target_bench_id == 0 || target_bench_id == current_bench_id);

        if (!mac_matches || !bench_matches) {
            ESP_LOGD(TAG, "CMD_SET_DEBOUNCE ignorado: nao coincide com este no (Meu ID: %u, Alvo ID: %u)",
                     current_bench_id, target_bench_id);
            return;
        }

        uint32_t new_debounce_ms = 0;
        memcpy(&new_debounce_ms, &payload[14], sizeof(uint32_t));

        if (new_debounce_ms < 10 || new_debounce_ms > 5000) {
            ESP_LOGW(TAG, "CMD_SET_DEBOUNCE com valor fora do intervalo permitido (%lu ms, esperado 10..5000)",
                     (unsigned long)new_debounce_ms);
            return;
        }

        ESP_LOGI(TAG, "Novo tempo de debounce recebido via LoRa: %lu ms (Anterior: %lu ms)",
                 (unsigned long)new_debounce_ms, (unsigned long)sensor_manager_get_debounce_ms());

        // aplica dinamicamente na ISR
        sensor_manager_set_debounce_ms(new_debounce_ms);

        // persiste na Flash para manter apos reinicializacao
        esp_err_t err = nvs_manager_set_debounce_ms(new_debounce_ms);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Novo tempo de debounce (%lu ms) salvo na NVS com sucesso!", (unsigned long)new_debounce_ms);
        } else {
            ESP_LOGE(TAG, "Falha ao gravar debounce_ms na NVS (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGD(TAG, "Tipo de mensagem LoRa desconhecido ou ignorado (0x%02X)", msg_type);
    }
}

// opcodes de comando do transceptor SX1262
#define SX126X_CMD_SET_STANDBY 0x80
#define SX126X_CMD_SET_RX 0x82
#define SX126X_CMD_SET_TX 0x83
#define SX126X_CMD_SET_PACKET_TYPE 0x8A
#define SX126X_CMD_SET_RF_FREQUENCY 0x86
#define SX126X_CMD_SET_PA_CONFIG 0x95
#define SX126X_CMD_SET_TX_PARAMS 0x8E
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
#define SX126X_CMD_WRITE_BUFFER 0x0E
#define SX126X_CMD_READ_BUFFER 0x1E
#define LORA_PIN_VEXT 36

// função auxiliar para aguardar o rádio terminar de processar operações internas
static void sx1262_wait_busy(void) {
    int timeout_us = 100000; // 100 ms max
    while (gpio_get_level(LORA_PIN_BUSY) == 1 && timeout_us > 0) {
        esp_rom_delay_us(10);
        timeout_us -= 10;
    }
    if (timeout_us <= 0) {
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

/**
 * @brief escreve dados na FIFO do SX1262 via SPI
 */
static esp_err_t sx1262_write_buffer(uint8_t offset, const uint8_t *data, size_t length) {
    if (data == NULL || length == 0 || length > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    sx1262_wait_busy();

    size_t total_len = 2 + length;
    uint8_t tx_buf[258];
    tx_buf[0] = SX126X_CMD_WRITE_BUFFER;
    tx_buf[1] = offset;
    memcpy(&tx_buf[2], data, length);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = total_len * 8;
    t.tx_buffer = tx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
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

    if (s_lora_mutex == NULL) {
        s_lora_mutex = xSemaphoreCreateRecursiveMutex();
    }

    LORA_LOCK();

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
        LORA_UNLOCK();
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
        LORA_UNLOCK();
        return ret;
    }

    // sequência de reset físico do chip SX1262
    gpio_set_level(LORA_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    // aguarda o chip sair do estado ocupado
    sx1262_wait_busy();

    // lê o endereço MAC
    esp_err_t err_mac = esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    if (err_mac == ESP_OK) {
        ESP_LOGI(TAG, "Meu Endereco MAC (STA): %02X:%02X:%02X:%02X:%02X:%02X", s_my_mac[0], s_my_mac[1], s_my_mac[2],
                 s_my_mac[3], s_my_mac[4], s_my_mac[5]);
    } else {
        ESP_LOGW(TAG, "Falha ao obter MAC Address STA (%s)", esp_err_to_name(err_mac));
    }

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

    // PA config para +22 dBm
    uint8_t pa_cfg[4] = {0x04, 0x07, 0x00, 0x01};
    sx1262_write_command(SX126X_CMD_SET_PA_CONFIG, pa_cfg, 4);

    // parâmetros de TX: +22 dBm, ramp 200us
    uint8_t tx_params[2] = {0x16, 0x02};
    sx1262_write_command(SX126X_CMD_SET_TX_PARAMS, tx_params, 2);

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

    // configura interrupção DIO1 para TxDone (0x0001) e RxDone (0x0002)
    uint8_t irq_params[8] = {0x03, 0xFF, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq_params, 8);

    // limpa flags residuais
    sx1262_clear_irq_status(0xFFFF);

    // coloca em modo de recepção contínua (0xFFFFFF)
    sx1262_set_rx(0xFFFFFF);

    LORA_UNLOCK();

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

        LORA_LOCK();
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
        LORA_UNLOCK();
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

esp_err_t lora_send_packet(const uint8_t *payload, size_t length) {
    if (payload == NULL || length == 0 || length > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    LORA_LOCK();

    uint8_t standby = 0x00;
    sx1262_write_command(SX126X_CMD_SET_STANDBY, &standby, 1);

    sx1262_clear_irq_status(0xFFFF);

    // escreve os dados no buffer FIFO
    sx1262_write_buffer(0x00, payload, length);

    // configura tamanho do pacote
    uint8_t pkt_params[6] = {0x00, 0x08, 0x00, (uint8_t)length, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // dispara transmissão
    uint8_t tx_timeout[3] = {0x00, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_TX, tx_timeout, 3);

    // aguarda TxDone
    int64_t start_time = esp_timer_get_time();
    bool tx_done = false;
    while ((esp_timer_get_time() - start_time) < 1500000) { // 1.5 segundos
        uint16_t irq = sx1262_get_irq_status();
        if (irq & 0x0001) { // TxDone
            sx1262_clear_irq_status(0x0001);
            tx_done = true;
            break;
        }
        vTaskDelay(1); // 1 tick mínimo para yield (10ms se tick=100Hz)
    }

    // restaura parâmetros do pacote para recepção com tamanho máximo (0xFF)
    uint8_t rx_pkt_params[6] = {0x00, 0x08, 0x00, 0xFF, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_PACKET_PARAMS, rx_pkt_params, 6);

    if (!tx_done) {
        ESP_LOGE(TAG, "Timeout na transmissao do pacote LoRa");
        sx1262_set_rx(0xFFFFFF); // retorna para escuta contínua
        LORA_UNLOCK();
        return ESP_ERR_TIMEOUT;
    }

    // volta imediatamente para modo de escuta contínua (RX)
    sx1262_set_rx(0xFFFFFF);
    LORA_UNLOCK();
    return ESP_OK;
}

// envia requisição de sincronização de horário com o endereço MAC e ID deste nó
esp_err_t lora_send_req_time(void) {
    uint16_t my_bench_id = BENCH_ID_UNCONFIGURED;
    nvs_manager_get_bench_id(&my_bench_id);

    // formato: [0xEB, 0x10, SENDER_MAC(6B), BENCH_ID(2B)] = 10 bytes
    uint8_t pkt[10];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_REQ_TIME;
    memcpy(&pkt[2], s_my_mac, 6);
    memcpy(&pkt[8], &my_bench_id, sizeof(uint16_t));

    ESP_LOGI(TAG, "Enviando solicitacao de Horario (0x10) via LoRa [MAC: %02X:%02X:%02X:%02X:%02X:%02X, ID: %u]...",
             s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5], my_bench_id);
    return lora_send_packet(pkt, sizeof(pkt));
}

// envia requisição das credenciais de wifi e broker MQTT com MAC e ID deste nó
esp_err_t lora_send_req_config(void) {
    uint16_t my_bench_id = BENCH_ID_UNCONFIGURED;
    nvs_manager_get_bench_id(&my_bench_id);

    // formato: [0xEB, 0x20, SENDER_MAC(6B), BENCH_ID(2B)] = 10 bytes
    uint8_t pkt[10];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_REQ_CONFIG;
    memcpy(&pkt[2], s_my_mac, 6);
    memcpy(&pkt[8], &my_bench_id, sizeof(uint16_t));

    ESP_LOGI(TAG,
             "Enviando solicitacao de Configuracoes (0x20) via LoRa [MAC: %02X:%02X:%02X:%02X:%02X:%02X, ID: %u]...",
             s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5], my_bench_id);
    return lora_send_packet(pkt, sizeof(pkt));
}

// transmite pacote anunciando presença física (botão segurado por 3s) para pareamento
esp_err_t lora_send_announce_pairing(void) {
    uint16_t my_bench_id = BENCH_ID_UNCONFIGURED;
    nvs_manager_get_bench_id(&my_bench_id);

    // formato: [0xEB, 0x33, SENDER_MAC(6B), BENCH_ID(2B)] = 10 bytes
    uint8_t pkt[10];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_ANNOUNCE_PAIRING;
    memcpy(&pkt[2], s_my_mac, 6);
    memcpy(&pkt[8], &my_bench_id, sizeof(uint16_t));

    ESP_LOGI(TAG, "Transmitindo ANUNCIO DE PAREAMENTO (0x33) via LoRa [MAC: %02X:%02X:%02X:%02X:%02X:%02X, ID: %u]...",
             s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5], my_bench_id);

    return lora_send_packet(pkt, sizeof(pkt));
}

// envia pacote de telemetria (fallback offline)
esp_err_t lora_send_telemetry(uint32_t count, uint64_t timestamp) {
    uint16_t my_bench_id = BENCH_ID_UNCONFIGURED;
    nvs_manager_get_bench_id(&my_bench_id);

    // formato: [0xEB, 0x50, MAC(6B), ID(2B), COUNT(4B), TIMESTAMP(8B)] = 22 bytes
    uint8_t pkt[22];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_TELEMETRY;
    memcpy(&pkt[2], s_my_mac, 6);
    memcpy(&pkt[8], &my_bench_id, sizeof(uint16_t));
    memcpy(&pkt[10], &count, sizeof(uint32_t));
    memcpy(&pkt[14], &timestamp, sizeof(uint64_t));

    ESP_LOGI(TAG, "Enviando TELEMETRIA (0x50) via LoRa [ID: %u, Count: %lu]...", my_bench_id, (unsigned long)count);
    return lora_send_packet(pkt, sizeof(pkt));
}
