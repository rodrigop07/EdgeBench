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
#include "protocol_examples_common.h"

#include "lora_receiver.h"
#include "mqtt_manager.h"
#include "nvs_manager.h"
#include "sensor_manager.h"
#include "storage_manager.h"

static const char *TAG = "APP_MAIN";

/**
 * @brief handler para gerenciar a reconexão automática do wifi
 */
static void wifi_reconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Conexao Wi-Fi perdida. Tentando reconectar (retry continuo)...");
        esp_wifi_connect();
    }
}

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

    // inicializa o sistema NVS
    ESP_ERROR_CHECK(nvs_manager_init());

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

    // inicializa pilhas de rede e loop de eventos padrão do ESP-IDF
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // inicializa o módulo de armazenamento
    QueueHandle_t storage_queue = NULL;
    ESP_ERROR_CHECK(storage_manager_init(&storage_queue));

    // inicializa o sensor E18-D80NK com ISR no GPIO48
    ESP_ERROR_CHECK(sensor_manager_init(storage_queue));

    // inicializa e conecta ao wifi utilizando as credenciais salvas na NVS
    char wifi_ssid[32];
    char wifi_pass[64];
    nvs_manager_get_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    esp_err_t wifi_err = example_connect();

    // garante que as credenciais de rede sejam as da NVS
    wifi_config_t current_wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &current_wifi_cfg) == ESP_OK) {
        if (strcmp((char *)current_wifi_cfg.sta.ssid, wifi_ssid) != 0 ||
            strcmp((char *)current_wifi_cfg.sta.password, wifi_pass) != 0) {
            ESP_LOGI(TAG, "Aplicando credenciais salvas da NVS no Wi-Fi: SSID=%s", wifi_ssid);
            strncpy((char *)current_wifi_cfg.sta.ssid, wifi_ssid, sizeof(current_wifi_cfg.sta.ssid) - 1);
            strncpy((char *)current_wifi_cfg.sta.password, wifi_pass, sizeof(current_wifi_cfg.sta.password) - 1);

            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &current_wifi_cfg);
            esp_wifi_connect();
        }
    }

    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi nao conectou de primeira. Tentando reconectar com as credenciais da NVS...");
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso!");
    }

    // registra listener para manter reconexão persistente em caso de queda
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_reconnect_handler, NULL, NULL);

    // configura fuso horário para Horário de Brasília (UTC-3) e cliente SNTP
    setenv("TZ", "<-03>3", 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("a.st1.ntp.br");
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "SNTP inicializado em background (servidor: a.st1.ntp.br, fuso: UTC-3)");

    if (wifi_err == ESP_OK && esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK) {
        ESP_LOGI(TAG, "Horario sincronizado com sucesso via SNTP!");
    } else {
        ESP_LOGW(TAG, "Aguardando sincronizacao de horario em segundo plano...");
    }

    // inicializa e conecta o cliente MQTT 5 com o broker configurado na NVS
    char broker_uri[128];
    nvs_manager_get_broker_url(broker_uri, sizeof(broker_uri));
    ESP_LOGI(TAG, "Iniciando cliente MQTT5 com broker NVS: %s (Bancada: %u)", broker_uri, bench_id);
    ESP_ERROR_CHECK(mqtt_manager_start(broker_uri, bench_id));
}
