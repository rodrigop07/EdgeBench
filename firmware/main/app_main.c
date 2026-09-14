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

#include "lora_receiver.h"
#include "mqtt_manager.h"
#include "nvs_manager.h"
#include "sensor_manager.h"
#include "storage_manager.h"

static const char *TAG = "APP_MAIN";
static esp_netif_t *s_sta_netif = NULL;

/**
 * @brief manipulador de eventos unificado para conexao e reconexao WiFi
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA iniciado. Conectando a rede...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;
        ESP_LOGW(TAG, "Wi-Fi desconectado (motivo: %u). Tentando reconectar...", reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso! Endereco IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/**
 * @brief inicializa a interface Wi-Fi no modo Station de forma nativa e segura
 */
static esp_err_t wifi_init_sta(const char *ssid, const char *password) {
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar Wi-Fi (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA configurado e iniciado para SSID: '%s'", ssid);
    return ESP_OK;
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
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    nvs_manager_get_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    wifi_init_sta(wifi_ssid, wifi_pass);

    // configura fuso horário para Horário de Brasília (UTC-3) e cliente SNTP
    setenv("TZ", "<-03>3", 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("a.st1.ntp.br");
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "SNTP inicializado em background (servidor: a.st1.ntp.br, fuso: UTC-3)");

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK) {
        ESP_LOGI(TAG, "Horario sincronizado com sucesso via SNTP!");
    } else {
        ESP_LOGW(TAG, "Aguardando sincronizacao de horario em segundo plano (ou via LoRa Beacon)...");
    }

    // inicializa e conecta o cliente MQTT 5 com o broker configurado na NVS
    char broker_uri[128];
    nvs_manager_get_broker_url(broker_uri, sizeof(broker_uri));
    ESP_LOGI(TAG, "Iniciando cliente MQTT5 com broker NVS: %s (Bancada: %u)", broker_uri, bench_id);
    esp_err_t mqtt_err = mqtt_manager_start(broker_uri, bench_id);
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT (%s). Continuará tentando em background.", esp_err_to_name(mqtt_err));
    }
}
