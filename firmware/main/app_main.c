#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_manager.h"
#include "lora_receiver.h"
#include "mqtt_manager.h"
#include "nvs_manager.h"
#include "ota_manager.h"
#include "sensor_manager.h"
#include "storage_manager.h"
#include "wifi_manager.h"

static const char *TAG = "APP_MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Memoria livre: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF Versao: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_WARN);
    esp_log_level_set("MQTT_MGR", ESP_LOG_INFO);
    esp_log_level_set("SENSOR_MGR", ESP_LOG_INFO);
    esp_log_level_set("STORAGE_MGR", ESP_LOG_INFO);
    esp_log_level_set("LORA_RCV", ESP_LOG_INFO);
    esp_log_level_set("NVS_MGR", ESP_LOG_INFO);
    esp_log_level_set("OTA_MGR", ESP_LOG_INFO);

    // configura fuso horário para GMT-3
    setenv("TZ", "<-03>3", 1);
    tzset();

    // inicializa o sistema NVS
    ESP_ERROR_CHECK(nvs_manager_init());

    // inicializa e valida a imagem OTA atual cancelando rollback automático
    ESP_ERROR_CHECK(ota_manager_init());
    ota_manager_validate_boot();

    // instala serviço de interrupções GPIO
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }

    // lê e exibe o contador de boot
    uint32_t boot_count = 0;
    nvs_manager_get_boot_count(&boot_count);
    ESP_LOGI(TAG, "Boot count atual: %" PRIu32, boot_count);

    // lê e exibe o identificador da bancada
    uint16_t bench_id = 1;
    nvs_manager_get_bench_id(&bench_id);
    ESP_LOGI(TAG, "ID da Bancada (EdgeBench): %u", bench_id);

    // inicializa o rádio LoRa (SX1262) e sua tarefa de escuta no Core 0
    ESP_ERROR_CHECK(lora_receiver_init());
    ESP_ERROR_CHECK(lora_receiver_start_task());

    // inicializa o monitoramento do botão no GPIO 0
    ESP_ERROR_CHECK(button_manager_init());

    // inicializa pilhas de rede e loop de eventos padrão do ESP-IDF
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // inicializa o módulo de armazenamento
    QueueHandle_t storage_queue = NULL;
    ESP_ERROR_CHECK(storage_manager_init(&storage_queue));

    // inicializa o sensor E18-D80NK com ISR no GPIO48
    ESP_ERROR_CHECK(sensor_manager_init(storage_queue));

    // inicializa e conecta ao wifi utilizando as credenciais salvas na NVS
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    esp_err_t wifi_err = nvs_manager_get_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    if (wifi_err != ESP_OK || wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Nenhuma credencial Wi-Fi salva na NVS. Solicitando credenciais via LoRa...");
        lora_send_req_config();
        // aguarda até 2 segundos caso a Central responda de imediato
        vTaskDelay(pdMS_TO_TICKS(2000));
        nvs_manager_get_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));
    }

    if (!wifi_manager_is_initialized()) {
        if (wifi_ssid[0] != '\0') {
            wifi_manager_init_sta(wifi_ssid, wifi_pass);
        } else {
            ESP_LOGW(TAG, "Credenciais Wi-Fi ainda não disponíveis, aguardando configuração via LoRa...");
        }
    }

    // configura fuso horário para horário de Brasília e cliente SNTP
    setenv("TZ", "<-03>3", 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("a.st1.ntp.br");
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "SNTP inicializado em background (servidor: a.st1.ntp.br, fuso: UTC-3)");

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK) {
        ESP_LOGI(TAG, "Horario sincronizado com sucesso via SNTP!");
    } else {
        ESP_LOGW(TAG, "Falha na sincronizacao SNTP. Solicitando horario via LoRa...");
        lora_send_req_time();
    }

    // inicializa e conecta o cliente MQTT 5 com o broker configurado na NVS
    char broker_uri[128];
    nvs_manager_get_broker_url(broker_uri, sizeof(broker_uri));
    ESP_LOGI(TAG, "Iniciando cliente MQTT5 com broker NVS: %s (Bancada: %u)", broker_uri, bench_id);
    esp_err_t mqtt_err = mqtt_manager_start(broker_uri, bench_id);
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT (%s). Continuará tentando em background.",
                 esp_err_to_name(mqtt_err));
    }
}
