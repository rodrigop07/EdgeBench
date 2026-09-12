#include "lora_receiver.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_manager.h"
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

        // estrutura de tempo
        struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};

        // ajusta o relógio interno do esp
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Horário sincronizado com sucesso via Beacon LoRa (Epoch: %llu)", (unsigned long long)epoch);
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
        if (length < (size_t)(7 + url_len) || url_len >= 128) {
            ESP_LOGW(TAG, "Comprimento da URL inválido (%d)", url_len);
            return;
        }

        // cria a nova url
        char nova_url[128];
        memcpy(nova_url, &payload[7], url_len);
        nova_url[url_len] = '\0';

        ESP_LOGI(TAG, "Novo Broker recebido via LoRa: %s", nova_url);

        // grava a nova url na NVS
        esp_err_t err = nvs_manager_set_broker_url(nova_url);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Broker atualizado na NVS com sucesso!");
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
        // validação do tamanho do ssid
        if (ssid_len == 0 || ssid_len >= 32 || length < (size_t)(7 + ssid_len + 1)) {
            ESP_LOGW(TAG, "Tamanho de SSID inválido (%d)", ssid_len);
            return;
        }

        // cria o novo ssid
        char novo_ssid[32];
        memcpy(novo_ssid, &payload[7], ssid_len);
        novo_ssid[ssid_len] = '\0';

        // posição do tamanho da senha
        size_t pass_offset = 7 + ssid_len;
        uint8_t pass_len = payload[pass_offset];

        // validação do tamanho da senha
        if (pass_len >= 64 || length < (pass_offset + 1 + pass_len)) {
            ESP_LOGW(TAG, "Tamanho de senha Wi-Fi inválido (%d)", pass_len);
            return;
        }

        // cria a nova senha
        char nova_senha[64];
        if (pass_len > 0) {
            memcpy(nova_senha, &payload[pass_offset + 1], pass_len);
        }
        nova_senha[pass_len] = '\0';
        ESP_LOGI(TAG, "Novas credenciais Wi-Fi recebidas via LoRa! SSID: %s", novo_ssid);

        // grava na NVS
        esp_err_t err = nvs_manager_set_wifi_credentials(novo_ssid, nova_senha);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi atualizado na Flash! Reiniciando em 2 segundos para conectar na nova rede...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Falha ao gravar Wi-Fi na NVS (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Tipo de mensagem LoRa desconhecido (0x%02X)", msg_type);
    }
}

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

    // configura os pinos GPIO de controle: RST (saída), BUSY (entrada), DIO1 (entrada)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LORA_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
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
    ESP_LOGI(TAG, "Hardware do SX1262 inicializado com sucesso!");
    return ESP_OK;
}

// tarefa executada em segundo plano no Core 0
static void lora_rx_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de recepcao LoRa iniciada no Core %d", xPortGetCoreID());

    uint8_t rx_buffer[256];

    while (1) {
        // trava a tarefa até receber interrupção
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Pacote recebido");

        size_t bytes_recebidos = 0;
        // (Leitura SPI dos bytes do SX1262)
        if (bytes_recebidos > 0) {
            lora_process_packet(rx_buffer, bytes_recebidos);
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
