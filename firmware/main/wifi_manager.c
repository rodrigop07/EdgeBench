#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static esp_netif_t *s_sta_netif = NULL;
static bool s_is_connected = false;
static bool s_is_reconfiguring = false;

/**
 * @brief manipulador de eventos unificado para conexão e reconexão WiFi
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA iniciado. Conectando a rede...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;
        ESP_LOGW(TAG, "Wi-Fi desconectado (motivo: %u).", reason);

        // se nao estiver em meio a uma reconfiguração explícita, tenta reconectar
        if (!s_is_reconfiguring) {
            ESP_LOGI(TAG, "Tentando reconectar...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_is_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso! Endereco IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_init_sta(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID invalido ou vazio");
        return ESP_ERR_INVALID_ARG;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar Wi-Fi (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

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

esp_err_t wifi_manager_reconfigure(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID invalido para reconfiguracao");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Reconfigurando Wi-Fi para novo SSID: '%s'...", ssid);

    // ativa flag para evitar reconexão na rede antiga
    s_is_reconfiguring = true;
    s_is_connected = false;

    // desconecta da rede atual
    esp_wifi_disconnect();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    s_is_reconfiguring = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir nova configuracao Wi-Fi (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Conectando a nova rede Wi-Fi '%s' sem reiniciar o chip...", ssid);
    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}
