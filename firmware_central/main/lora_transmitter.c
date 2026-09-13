#include "lora_transmitter.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LORA_TX";

static spi_device_handle_t s_lora_spi = NULL;

// opcodes de comando do transceptor
#define SX126X_CMD_SET_STANDBY 0x80
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
#define SX126X_CMD_WRITE_BUFFER 0x0E

/**
 * @brief aguarda o chip SX1262 liberar o pino BUSY
 */
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
 * @brief envia comando SPI sem leitura de retorno
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
 * @brief escreve dados no buffer interno FIFO do SX1262
 */
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

esp_err_t lora_transmitter_init(void) {
    ESP_LOGI(TAG, "Inicializando transceptor LoRa SX1262 para transmissao (915 MHz)...");

    // configuração dos pinos GPIO de controle
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LORA_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << LORA_PIN_BUSY) | (1ULL << LORA_PIN_DIO1);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // barramento SPI2
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

    // configuracao do SPI
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = LORA_PIN_NSS,
        .queue_size = 7,
    };

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_lora_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar dispositivo SPI (%s)", esp_err_to_name(ret));
        return ret;
    }

    // reset do chip SX1262
    gpio_set_level(LORA_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LORA_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    sx1262_wait_busy();

    // registradores de RF do SX1262
    // standby RC
    uint8_t standby_mode = 0x00;
    sx1262_write_command(SX126X_CMD_SET_STANDBY, &standby_mode, 1);

    // ativa regulador interno DC-DC
    uint8_t reg_mode = 0x01;
    sx1262_write_command(SX126X_CMD_SET_REGULATOR_MODE, &reg_mode, 1);

    // configura DIO2 para chavear a antena RF
    uint8_t dio2_switch = 0x01;
    sx1262_write_command(SX126X_CMD_SET_DIO2_AS_RF_SWITCH, &dio2_switch, 1);

    // tsipo de pacote: LoRa (0x01)
    uint8_t pkt_type = 0x01;
    sx1262_write_command(SX126X_CMD_SET_PACKET_TYPE, &pkt_type, 1);

    // frequência: 915 MHz (0x39300000)
    uint8_t rf_freq[4] = {0x39, 0x30, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_RF_FREQUENCY, rf_freq, 4);

    // PA config (+22 dBm)
    uint8_t pa_config[4] = {0x04, 0x07, 0x00, 0x01};
    sx1262_write_command(SX126X_CMD_SET_PA_CONFIG, pa_config, 4);

    // Tx params: +22 dBm (0x16), Ramp time 40us (0x02)
    uint8_t tx_params[2] = {0x16, 0x02};
    sx1262_write_command(SX126X_CMD_SET_TX_PARAMS, tx_params, 2);

    // buffer base: TxBase=0x00, RxBase=0x00
    uint8_t buf_base[2] = {0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_BUFFER_BASE_ADDR, buf_base, 2);

    // parâmetros de Modulação: SF7 (0x07), BW 125kHz (0x04), CR 4/5 (0x01), LDRO off (0x00)
    uint8_t mod_params[4] = {0x07, 0x04, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_MODULATION_PARAMS, mod_params, 4);

    // configura interrupções DIO1 para TxDone (0x0001)
    uint8_t irq_params[8] = {0x03, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    sx1262_write_command(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq_params, 8);

    ESP_LOGI(TAG, "SX1262 inicializado com sucesso em 915 MHz (+22 dBm)!");
    return ESP_OK;
}

esp_err_t lora_send_packet(const uint8_t *payload, size_t length) {
    if (payload == NULL || length == 0 || length > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    // garante que o rádio está em Standby
    uint8_t standby_mode = 0x00;
    sx1262_write_command(SX126X_CMD_SET_STANDBY, &standby_mode, 1);

    // escreve os dados no buffer FIFO a partir do offset 0
    sx1262_write_buffer(0x00, payload, length);

    // ajusta o tamanho do pacote no PacketParams
    // Preamble=8 (0x0008), Header=Explicit (0x00), PayloadLen=length, CRC=On (0x01), InvertIQ=Std (0x00)
    uint8_t pkt_params[6] = {0x00, 0x08, 0x00, (uint8_t)length, 0x01, 0x00};
    sx1262_write_command(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, 6);

    // limpa todas as flags de interrupção
    uint8_t clear_irq[2] = {0x03, 0xFF};
    sx1262_write_command(SX126X_CMD_CLEAR_IRQ_STATUS, clear_irq, 2);

    // inicia a transmissão em modo Tx (timeout de 3 segundos no rádio: ~3000ms / 15.625us = 0x02EE00)
    uint8_t tx_timeout[3] = {0x02, 0xEE, 0x00};
    sx1262_write_command(SX126X_CMD_SET_TX, tx_timeout, 3);

    // aguarda a conclusão da transmissão (pino BUSY vai para zero ou timeout)
    int wait_ms = 3000;
    while (gpio_get_level(LORA_PIN_BUSY) == 1 && wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        wait_ms -= 5;
    }

    if (wait_ms <= 0) {
        ESP_LOGW(TAG, "Aviso: Timeout aguardando conclusao da transmissao LoRa");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Pacote LoRa (%d bytes, tipo: 0x%02X) transmitido com sucesso!", (int)length, payload[1]);
    return ESP_OK;
}

esp_err_t lora_send_beacon(uint64_t timestamp) {
    // cria o pacote de beacon
    uint8_t pkt[10];
    pkt[0] = LORA_ESPECIAL_BYTE;                   // byte de identificação
    pkt[1] = LORA_MSG_TIME_BEACON;                 // tipo de mensagem
    memcpy(&pkt[2], &timestamp, sizeof(uint64_t)); // timestamp

    // envia o pacote
    ESP_LOGI(TAG, "Transmitindo Time Beacon LoRa (Epoch: %llu)...", (unsigned long long)timestamp);
    return lora_send_packet(pkt, sizeof(pkt));
}

esp_err_t lora_send_set_broker(const char *broker_url) {
    // verifica se o broker_url é válido
    if (broker_url == NULL || strlen(broker_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // verifica se o tamanho do broker_url é válido
    uint8_t url_len = (uint8_t)strlen(broker_url);
    if (url_len > 120) {
        return ESP_ERR_INVALID_SIZE;
    }

    // token de segurança
    uint32_t token = LORA_SECURITY_TOKEN;

    // cria o pacote de set broker
    uint8_t pkt[7 + url_len];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_SET_BROKER;
    memcpy(&pkt[2], &token, sizeof(uint32_t));
    pkt[6] = url_len;
    memcpy(&pkt[7], broker_url, url_len);

    // envia o pacote
    ESP_LOGI(TAG, "Transmitindo comando Set Broker via LoRa: %s", broker_url);
    return lora_send_packet(pkt, sizeof(pkt));
}

esp_err_t lora_send_set_wifi(const char *ssid, const char *password) {
    // verifica se o ssid é válido
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // verifica se o tamanho do ssid é válido
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    const char *safe_pass = (password != NULL) ? password : "";
    uint8_t pass_len = (uint8_t)strlen(safe_pass);

    if (ssid_len > 32 || pass_len > 64) {
        return ESP_ERR_INVALID_SIZE;
    }

    // token de segurança
    uint32_t token = LORA_SECURITY_TOKEN;

    // cria o pacote de set wifi
    size_t pkt_size = 7 + ssid_len + 1 + pass_len;
    uint8_t pkt[pkt_size];

    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_SET_WIFI;
    memcpy(&pkt[2], &token, sizeof(uint32_t));
    pkt[6] = ssid_len;
    memcpy(&pkt[7], ssid, ssid_len);

    // adiciona o password ao pacote
    size_t pass_offset = 7 + ssid_len;
    pkt[pass_offset] = pass_len;
    if (pass_len > 0) {
        memcpy(&pkt[pass_offset + 1], safe_pass, pass_len);
    }

    // envia o pacote
    ESP_LOGI(TAG, "Transmitindo comando Set Wi-Fi via LoRa (SSID: %s)...", ssid);
    return lora_send_packet(pkt, pkt_size);
}

esp_err_t lora_send_set_bench(uint16_t bench_id) {
    // verifica se o bench_id é válido
    if (bench_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // token de segurança
    uint32_t token = LORA_SECURITY_TOKEN;

    // cria o pacote de set bench
    uint8_t pkt[8];
    pkt[0] = LORA_ESPECIAL_BYTE;
    pkt[1] = LORA_MSG_SET_BENCH;
    memcpy(&pkt[2], &token, sizeof(uint32_t));
    memcpy(&pkt[6], &bench_id, sizeof(uint16_t));

    // envia o pacote
    ESP_LOGI(TAG, "Transmitindo comando Set Bench ID via LoRa: %u", bench_id);
    return lora_send_packet(pkt, sizeof(pkt));
}
