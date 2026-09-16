#include "lora_transmitter.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include <string.h>
#include <time.h>

static const char *TAG = "GATEWAY_LORA";

static spi_device_handle_t s_lora_spi = NULL;
static TaskHandle_t s_rx_task_handle = NULL;
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

// opcodes de comando do transceptor SX1262
#define SX126X_CMD_SET_STANDBY 0x80
#define SX126X_CMD_SET_RX 0x82
#define SX126X_CMD_SET_TX 0x83
#define SX126X_CMD_SET_PACKET_TYPE 0x8A
#define SX126X_CMD_SET_RF_FREQUENCY 0x86
#define SX126X_CMD_SET_PA_CONFIG 0x95
#define SX126X_CMD_SET_REGULATOR_MODE 0x96
#define SX126X_CMD_SET_TX_PARAMS 0x8E
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
#define SX126X_CMD_WRITE_BUFFER 0x0E
#define SX126X_CMD_GET_RX_BUFFER_STATUS 0x13
#define SX126X_CMD_READ_BUFFER 0x1E

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

static esp_err_t sx1262_write_buffer(uint8_t offset, const uint8_t *data, size_t length) {
    sx1262_wait_busy();

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    uint8_t tx_buf[258];
    tx_buf[0] = SX126X_CMD_WRITE_BUFFER;
    tx_buf[1] = offset;
    memcpy(&tx_buf[2], data, length);

    t.length = (2 + length) * 8;
    t.tx_buffer = tx_buf;

    esp_err_t ret = spi_device_transmit(s_lora_spi, &t);
    sx1262_wait_busy();
    return ret;
}

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
    tx_buf[2] = 0x00;

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

static esp_err_t sx1262_clear_irq_status(uint16_t irq_mask) {
    uint8_t data[2] = {(uint8_t)(irq_mask >> 8), (uint8_t)(irq_mask & 0xFF)};
    return sx1262_write_command(SX126X_CMD_CLEAR_IRQ_STATUS, data, 2);
}

static esp_err_t sx1262_set_rx(uint32_t timeout) {
    uint8_t data[3] = {(uint8_t)((timeout >> 16) & 0xFF), (uint8_t)((timeout >> 8) & 0xFF), (uint8_t)(timeout & 0xFF)};
    return sx1262_write_command(SX126X_CMD_SET_RX, data, 3);
}

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

static void IRAM_ATTR lora_dio1_isr_handler(void *arg) {
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_rx_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_rx_task_handle, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t lora_transmitter_init(void) {
    ESP_LOGI(TAG, "Inicializando transceptor LoRa SX1262 a 915MHz...");

    if (s_lora_mutex == NULL) {
        s_lora_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_lora_mutex == NULL) {
            ESP_LOGE(TAG, "Falha ao criar s_lora_mutex");
            return ESP_FAIL;
        }
    }

    LORA_LOCK();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LORA_PIN_RST) | (1ULL << LORA_PIN_VEXT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // ativa alimentação de periféricos/rádio no Heltec V3 (nível baixo)
    gpio_set_level(LORA_PIN_VEXT, 0);

    // configura pino BUSY como entrada com pull-up
    gpio_config_t io_conf_busy = {
        .pin_bit_mask = (1ULL << LORA_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_busy);

    // configura pino DIO1 com interrupção na borda de subida (POSEDGE) para capturar RxDone e TxDone
    gpio_config_t io_conf_dio = {
        .pin_bit_mask = (1ULL << LORA_PIN_DIO1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf_dio);

    // reset físico do chip
    gpio_set_level(LORA_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    sx1262_wait_busy();

    spi_bus_config_t buscfg = {
        .miso_io_num = LORA_PIN_MISO,
        .mosi_io_num = LORA_PIN_MOSI,
        .sclk_io_num = LORA_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8000000,
        .mode = 0,
        .spics_io_num = LORA_PIN_NSS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_lora_spi));

    // modo Standby
    uint8_t standby_data = 0x00; // STDBY_RC
    sx1262_write_command(SX126X_CMD_SET_STANDBY, &standby_data, 1);

    // modo regulador DC-DC (deve ser antes de calibrar e antes do TCXO)
    uint8_t reg_mode = 0x01;
    sx1262_write_command(SX126X_CMD_SET_REGULATOR_MODE, &reg_mode, 1);

    // DIO3 como controle de TCXO (1.8V, timeout 10ms = 0x000280 - padrao Heltec V3)
    uint8_t dio3_data[4] = {0x02, 0x00, 0x02, 0x80};
    sx1262_write_command(SX126X_CMD_SET_DIO3_AS_TCXO_CTRL, dio3_data, 4);

    // calibração
    uint8_t calib_param = 0x7F;
    sx1262_write_command(SX126X_CMD_CALIBRATE, &calib_param, 1);

    // calibração de imagem ISM 902-928 MHz
    uint8_t calib_img[2] = {0xE1, 0xE9};
    sx1262_write_command(SX126X_CMD_CALIBRATE_IMAGE, calib_img, 2);

    // DIO2 como RF Switch
    uint8_t dio2_data = 0x01;
    sx1262_write_command(SX126X_CMD_SET_DIO2_AS_RF_SWITCH, &dio2_data, 1);

    // tipo de pacote LoRa
    uint8_t pkt_type = 0x01;
    sx1262_write_command(SX126X_CMD_SET_PACKET_TYPE, &pkt_type, 1);

    // frequência 915 MHz (0x39300000)
    uint8_t rf_freq[4] = {0x39, 0x30, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_RF_FREQUENCY, rf_freq, 4);

    // PA config para +22 dBm
    uint8_t pa_cfg[4] = {0x04, 0x07, 0x00, 0x01};
    sx1262_write_command(SX126X_CMD_SET_PA_CONFIG, pa_cfg, 4);

    // parâmetros de TX: +22 dBm, ramp 200us
    uint8_t tx_params[2] = {0x16, 0x02};
    sx1262_write_command(SX126X_CMD_SET_TX_PARAMS, tx_params, 2);

    // buffer base address (TX=0, RX=0)
    uint8_t buf_base[2] = {0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_BUFFER_BASE_ADDR, buf_base, 2);

    // modulação: SF7, BW 125kHz, CR 4/5, LowDataRateOptimize off
    uint8_t mod_params[4] = {0x07, 0x04, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_MODULATION_PARAMS, mod_params, 4);

    // pacote: preâmbulo 8, header explícito, max len 255, CRC on, standard IQ
    uint8_t pkt_params[6] = {0x00, 0x08, 0x00, 0xFF, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // DIO1 IRQ para TxDone e RxDone com máscara completa 0x03FF
    uint8_t dio_irq[8] = {0x03, 0xFF, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_DIO_IRQ_PARAMS, dio_irq, 8);

    sx1262_clear_irq_status(0xFFFF);

    // coloca em escuta contínua imediatamente
    sx1262_set_rx(0xFFFFFF);

    LORA_UNLOCK();

    ESP_LOGI(TAG, "SX1262 inicializado com sucesso e em modo de escuta continua!");
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
        sx1262_set_rx(0xFFFFFF); // retorna para escuta
        LORA_UNLOCK();
        return ESP_ERR_TIMEOUT;
    }

    // volta imediatamente para modo de escuta contínua (RX)
    sx1262_set_rx(0xFFFFFF);
    LORA_UNLOCK();
    return ESP_OK;
}

// envia resposta de horário (0x11) com timestamp Epoch Unix
esp_err_t lora_send_resp_time(const uint8_t target_mac[6], uint64_t timestamp) {
    if (target_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // formato do pacote: [0xEB, 0x11, TARGET_MAC(6B), TIMESTAMP(8B)] = 16 bytes
    uint8_t pkt[16];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_RESP_TIME;
    memcpy(&pkt[2], target_mac, 6);
    memcpy(&pkt[8], &timestamp, sizeof(uint64_t));

    ESP_LOGI(TAG, "Enviando RESP_TIME para MAC %02X:%02X:%02X:%02X:%02X:%02X (Epoch: %llu)", target_mac[0],
             target_mac[1], target_mac[2], target_mac[3], target_mac[4], target_mac[5], (unsigned long long)timestamp);

    return lora_send_packet(pkt, sizeof(pkt));
}

// envia configurações de rede wifi e URL do broker MQTT para a bancada
esp_err_t lora_send_resp_config(const uint8_t target_mac[6], const char *ssid, const char *password,
                                const char *broker_url) {
    if (target_mac == NULL || ssid == NULL || broker_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t ssid_len = (uint8_t)strlen(ssid);
    const char *safe_pass = (password != NULL) ? password : "";
    uint8_t pass_len = (uint8_t)strlen(safe_pass);
    uint8_t broker_len = (uint8_t)strlen(broker_url);

    // token de segurança para autorizar o salvamento na NVS do receptor
    uint32_t token = LORA_SECURITY_TOKEN;
    // tamanho dinâmico: cabeçalho + MAC + token + strings com seus respectivos tamanhos
    size_t pkt_size = 2 + 6 + 4 + 1 + ssid_len + 1 + pass_len + 1 + broker_len;
    uint8_t pkt[pkt_size];

    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_RESP_CONFIG;
    memcpy(&pkt[2], target_mac, 6);
    memcpy(&pkt[8], &token, sizeof(uint32_t));

    // monta o payload serializado para cada parâmetro
    size_t offset = 12;
    pkt[offset++] = ssid_len;
    memcpy(&pkt[offset], ssid, ssid_len);
    offset += ssid_len;

    pkt[offset++] = pass_len;
    if (pass_len > 0) {
        memcpy(&pkt[offset], safe_pass, pass_len);
        offset += pass_len;
    }

    pkt[offset++] = broker_len;
    memcpy(&pkt[offset], broker_url, broker_len);
    offset += broker_len;

    ESP_LOGI(TAG, "Enviando RESP_CONFIG para MAC %02X:%02X:%02X:%02X:%02X:%02X (SSID: %s, Broker: %s)", target_mac[0],
             target_mac[1], target_mac[2], target_mac[3], target_mac[4], target_mac[5], ssid, broker_url);

    return lora_send_packet(pkt, pkt_size);
}

// envia comando para reconfigurar o ID da bancada direcionado por ID atual e/ou MAC
esp_err_t lora_send_set_bench(const uint8_t target_mac[6], uint16_t target_bench_id, uint16_t new_bench_id) {
    if (new_bench_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // se MAC não especificado, usa broadcast FF:FF:FF:FF:FF:FF
    uint8_t mac_to_use[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (target_mac != NULL) {
        memcpy(mac_to_use, target_mac, 6);
    }

    uint32_t token = LORA_SECURITY_TOKEN;
    // formato: [0xEB, 0x30, MAC(6B), TARGET_ID(2B), TOKEN(4B), NEW_ID(2B)] = 16 bytes
    uint8_t pkt[16];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_CMD_SET_BENCH;
    memcpy(&pkt[2], mac_to_use, 6);
    memcpy(&pkt[8], &target_bench_id, sizeof(uint16_t));
    memcpy(&pkt[10], &token, sizeof(uint32_t));
    memcpy(&pkt[14], &new_bench_id, sizeof(uint16_t));

    ESP_LOGI(TAG, "Enviando CMD_SET_BENCH (Alvo ID: %u, Novo ID: %u) para MAC %02X:%02X:%02X:%02X:%02X:%02X",
             target_bench_id, new_bench_id, mac_to_use[0], mac_to_use[1], mac_to_use[2], mac_to_use[3], mac_to_use[4],
             mac_to_use[5]);

    return lora_send_packet(pkt, sizeof(pkt));
}

// envia consulta à bancada para que ela responda seu endereço MAC físico
esp_err_t lora_send_req_bench_info(uint16_t target_bench_id) {
    // formato: [0xEB, 0x31, TARGET_BENCH_ID(2B)] = 4 bytes
    uint8_t pkt[4];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_REQ_BENCH_INFO;
    memcpy(&pkt[2], &target_bench_id, sizeof(uint16_t));

    ESP_LOGI(TAG, "Enviando REQ_BENCH_INFO para consultar bancada ID %u...", target_bench_id);
    return lora_send_packet(pkt, sizeof(pkt));
}

// envia comando de atualização OTA direcionado por ID (ou 0 para todas as bancadas)
esp_err_t lora_send_cmd_ota(uint16_t target_bench_id, const char *url) {
    if (url == NULL || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t url_len = strlen(url);
    if (url_len > 180) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t token = LORA_SECURITY_TOKEN;
    // formato: [0xEB, 0x40, TARGET_ID(2B), TOKEN(4B), URL_LEN(1B), URL(N_BYTES)] = 9 + N bytes
    size_t pkt_size = 9 + url_len;
    uint8_t pkt[pkt_size];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_CMD_OTA;
    memcpy(&pkt[2], &target_bench_id, sizeof(uint16_t));
    memcpy(&pkt[4], &token, sizeof(uint32_t));
    pkt[8] = (uint8_t)url_len;
    memcpy(&pkt[9], url, url_len);

    ESP_LOGI(TAG, "Enviando CMD_OTA (Alvo ID: %u, URL: %s)...", target_bench_id, url);
    return lora_send_packet(pkt, pkt_size);
}

// envia comando para reconfigurar o tempo de debounce do sensor via LoRa
esp_err_t lora_send_cmd_set_debounce(const uint8_t target_mac[6], uint16_t target_bench_id, uint32_t debounce_ms) {
    if (debounce_ms < 10 || debounce_ms > 5000) {
        ESP_LOGE(TAG, "Tempo de debounce fora do intervalo aceitavel: %lu ms (esperado 10..5000)", (unsigned long)debounce_ms);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac_to_use[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (target_mac != NULL) {
        memcpy(mac_to_use, target_mac, 6);
    }

    uint32_t token = LORA_SECURITY_TOKEN;
    // formato: [0xEB, 0x60, MAC(6B), TARGET_ID(2B), TOKEN(4B), DEBOUNCE_MS(4B)] = 18 bytes
    uint8_t pkt[18];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_CMD_SET_DEBOUNCE;
    memcpy(&pkt[2], mac_to_use, 6);
    memcpy(&pkt[8], &target_bench_id, sizeof(uint16_t));
    memcpy(&pkt[10], &token, sizeof(uint32_t));
    memcpy(&pkt[14], &debounce_ms, sizeof(uint32_t));

    ESP_LOGI(TAG, "Enviando CMD_SET_DEBOUNCE (Alvo ID: %u, Debounce: %lu ms) para MAC %02X:%02X:%02X:%02X:%02X:%02X",
             target_bench_id, (unsigned long)debounce_ms,
             mac_to_use[0], mac_to_use[1], mac_to_use[2], mac_to_use[3], mac_to_use[4], mac_to_use[5]);

    return lora_send_packet(pkt, sizeof(pkt));
}

esp_err_t lora_send_ping_broadcast(void) {
    uint32_t token = LORA_SECURITY_TOKEN;
    // formato: [0xEB, 0x70, TOKEN(4B)] = 6 bytes
    uint8_t pkt[6];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_CMD_PING;
    memcpy(&pkt[2], &token, sizeof(uint32_t));

    ESP_LOGI(TAG, "Enviando Ping Broadcast via LoRa para descoberta de bancadas...");
    return lora_send_packet(pkt, sizeof(pkt));
}

static void lora_rx_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de escuta RX iniciada (Core 0)");

    // coloca o rádio SX1262 em modo de escuta contínua (timeout 0xFFFFFF)
    sx1262_clear_irq_status(0xFFFF);
    sx1262_set_rx(0xFFFFFF);

    uint8_t rx_buffer[256];

    while (1) {
        // aguarda notificação da ISR do pino DIO1 com timeout de 1 segundo para segurança
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        // bloqueia o acesso concorrente ao SPI enquanto lê e processa os dados do rádio
        LORA_LOCK();

        uint16_t irq = sx1262_get_irq_status();

        // bit 1 = RxDone (pacote recebido no buffer interno do chip)
        if (irq & 0x0002) {
            uint8_t payload_len = 0;
            uint8_t rx_ptr = 0;
            if (sx1262_get_rx_buffer_status(&payload_len, &rx_ptr) == ESP_OK && payload_len >= 4) {
                if (sx1262_read_buffer(rx_ptr, rx_buffer, payload_len) == ESP_OK) {
                    ESP_LOGI(TAG, "Pacote LoRa RF recebido na Central! Tam: %u bytes, Byte0: 0x%02X, Tipo: 0x%02X",
                             payload_len, rx_buffer[0], rx_buffer[1]);

                    // valida byte mágico do EdgeBench
                    if (rx_buffer[0] == LORA_ESPECIAL_BYTE) {
                        uint8_t msg_type = rx_buffer[1];

                        // bancada solicitando horário atual
                        if (msg_type == LORA_MSG_REQ_TIME && payload_len >= 8) {
                            uint8_t sender_mac[6];
                            memcpy(sender_mac, &rx_buffer[2], 6);
                            uint16_t sender_id = 0;
                            if (payload_len >= 10) {
                                memcpy(&sender_id, &rx_buffer[8], sizeof(uint16_t));
                            }
                            ESP_LOGI(TAG,
                                     "[REQ_TIME] Recebido de Bancada ID %u (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
                                     sender_id, sender_mac[0], sender_mac[1], sender_mac[2], sender_mac[3],
                                     sender_mac[4], sender_mac[5]);

                            time_t now = time(NULL);
                            struct tm ti;
                            localtime_r(&now, &ti);
                            if (ti.tm_year >= (2024 - 1900)) {
                                // responde o timestamp em broadcast para qualquer bancada sem horário
                                uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                                lora_send_resp_time(broadcast_mac, (uint64_t)now);
                            } else {
                                ESP_LOGW(TAG, "Horario Central nao sincronizado, solicitando ao PC via serial...");
                                printf("{\"event\":\"req_time\",\"bench_id\":%u}\n", sender_id);
                                fflush(stdout);
                            }
                            // bancada solicitando credenciais Wi-Fi e Broker MQTT
                        } else if (msg_type == LORA_MSG_REQ_CONFIG && payload_len >= 8) {
                            uint8_t sender_mac[6];
                            memcpy(sender_mac, &rx_buffer[2], 6);
                            uint16_t sender_id = 0;
                            if (payload_len >= 10) {
                                memcpy(&sender_id, &rx_buffer[8], sizeof(uint16_t));
                            }
                            ESP_LOGI(TAG,
                                     "[REQ_CONFIG] Recebido de Bancada ID %u (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
                                     sender_id, sender_mac[0], sender_mac[1], sender_mac[2], sender_mac[3],
                                     sender_mac[4], sender_mac[5]);

                            char ssid[33] = {0};
                            char pass[65] = {0};
                            char broker[128] = {0};
                            // lê as credenciais armazenadas na NVS e responde para o MAC solicitante
                            central_nvs_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
                            central_nvs_get_broker(broker, sizeof(broker));

                            if (ssid[0] != '\0') {
                                lora_send_resp_config(sender_mac, ssid, pass, broker);
                            } else {
                                ESP_LOGW(TAG, "[REQ_CONFIG] Central sem credenciais Wi-Fi cadastradas na NVS para enviar");
                            }
                            // bancada respondendo consulta de identificação (ID e MAC)
                        } else if (msg_type == LORA_MSG_RESP_BENCH_INFO && payload_len >= 10) {
                            uint8_t b_mac[6];
                            memcpy(b_mac, &rx_buffer[2], 6);
                            uint16_t b_id = 0;
                            memcpy(&b_id, &rx_buffer[8], sizeof(uint16_t));
                            ESP_LOGI(TAG,
                                     "[RESP_BENCH_INFO] Bancada ID %u respondeu, MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                                     b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            // emite JSON na porta serial para consumo do script Python
                            printf("{\"status\":\"ok\",\"msg\":\"bench_info\",\"bench_id\":%u,\"mac\":\"%02X:%02X:%02X:"
                                   "%02X:%02X:%02X\"}\n",
                                   b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            fflush(stdout);
                            // bancada anunciando pareamento físico disparado pelo botão
                        } else if (msg_type == LORA_MSG_ANNOUNCE_PAIRING && payload_len >= 10) {
                            uint8_t b_mac[6];
                            memcpy(b_mac, &rx_buffer[2], 6);
                            uint16_t b_id = 0;
                            memcpy(&b_id, &rx_buffer[8], sizeof(uint16_t));
                            ESP_LOGI(TAG,
                                     "[ANUNCIO_PAREAMENTO] Bancada ID %u solicitou pareamento físico! MAC: "
                                     "%02X:%02X:%02X:%02X:%02X:%02X",
                                     b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            // emite evento JSON de pareamento na serial para o menu interativo
                            printf(
                                "{\"status\":\"pairing\",\"bench_id\":%u,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\n",
                                b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            fflush(stdout);
                        } else if (msg_type == LORA_MSG_TELEMETRY && payload_len >= 22) {
                            uint8_t b_mac[6];
                            memcpy(b_mac, &rx_buffer[2], 6);
                            uint16_t b_id = 0;
                            memcpy(&b_id, &rx_buffer[8], sizeof(uint16_t));
                            uint32_t count = 0;
                            memcpy(&count, &rx_buffer[10], sizeof(uint32_t));
                            uint64_t timestamp = 0;
                            memcpy(&timestamp, &rx_buffer[14], sizeof(uint64_t));

                            ESP_LOGI(TAG, "[TELEMETRIA] Híbrida recebida da Bancada %u (Count: %lu, Time: %llu)",
                                     b_id, (unsigned long)count, (unsigned long long)timestamp);
                            // emite evento JSON de telemetria na serial para o script python publicar no MQTT
                            printf("{\"type\":\"telemetry\",\"bench_id\":%u,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"count\":%lu,\"timestamp\":%llu}\n",
                                   b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5], (unsigned long)count, (unsigned long long)timestamp);
                            fflush(stdout);
                        } else if (msg_type == LORA_MSG_RESP_PONG && payload_len >= 10) {
                            uint8_t b_mac[6];
                            memcpy(b_mac, &rx_buffer[2], 6);
                            uint16_t b_id = 0;
                            memcpy(&b_id, &rx_buffer[8], sizeof(uint16_t));
                            ESP_LOGI(TAG,
                                     "[PONG] Bancada ID %u respondeu, MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                                     b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            // emite JSON na porta serial para consumo do script Python
                            printf("{\"status\":\"ok\",\"msg\":\"pong\",\"bench_id\":%u,\"mac\":\"%02X:%02X:%02X:"
                                   "%02X:%02X:%02X\"}\n",
                                   b_id, b_mac[0], b_mac[1], b_mac[2], b_mac[3], b_mac[4], b_mac[5]);
                            fflush(stdout);
                        } else {
                            ESP_LOGW(TAG, "Pacote LoRa ignorado: msg_type=0x%02X, len=%u", msg_type, payload_len);
                        }
                    } else {
                        ESP_LOGW(TAG, "Pacote LoRa com byte especial invalido: 0x%02X (esperado 0xEB)", rx_buffer[0]);
                    }
                }
            }
            sx1262_clear_irq_status(0x03FF);
            sx1262_set_rx(0xFFFFFF);
        } else if (irq & 0x0040) { // CrcErr
            ESP_LOGW(TAG, "Pacote LoRa descartado na Central: Erro de CRC de RF");
            sx1262_clear_irq_status(0x03FF);
            sx1262_set_rx(0xFFFFFF);
        } else if (irq != 0) {
            sx1262_clear_irq_status(irq);
            sx1262_set_rx(0xFFFFFF);
        }

        // libera a trava do barramento SPI e rádio
        LORA_UNLOCK();
    }
}

esp_err_t lora_start_rx_task(void) {
    // instala serviço ISR no pino DIO1
    gpio_isr_handler_add(LORA_PIN_DIO1, lora_dio1_isr_handler, NULL);

    BaseType_t ret = xTaskCreatePinnedToCore(lora_rx_task, "lora_central_rx", 4096, NULL, 5, &s_rx_task_handle, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar lora_central_rx");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Escuta contínua de requisições ativada no SX1262");
    return ESP_OK;
}
