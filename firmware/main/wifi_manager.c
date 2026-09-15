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

static const char *wifi_disconn_reason_to_str(uint8_t reason) {
    switch (reason) {
    case 1:
        return "UNSPECIFIED";
    case 2:
        return "AUTH_EXPIRE";
    case 3:
        return "AUTH_LEAVE";
    case 4:
        return "ASSOC_EXPIRE";
    case 5:
        return "ASSOC_TOOMANY";
    case 6:
        return "NOT_AUTHED";
    case 7:
        return "NOT_ASSOCED";
    case 8:
        return "ASSOC_LEAVE";
    case 15:
        return "4WAY_HANDSHAKE_TIMEOUT (Possivel senha incorreta)";
    case 200:
        return "BEACON_TIMEOUT (Sinal fraco ou AP desligado)";
    case 201:
        return "NO_AP_FOUND (SSID nao encontrado / fora de alcance)";
    case 202:
        return "AUTH_FAIL (Modo de autenticacao incompativel)";
    case 204:
        return "HANDSHAKE_TIMEOUT";
    case 205:
        return "CONNECTION_FAIL";
    default:
        return "ERRO_DESCONHECIDO";
    }
}

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
        ESP_LOGW(TAG, "Wi-Fi desconectado (motivo: %u - %s).", reason, wifi_disconn_reason_to_str(reason));

        // se nao estiver em meio a uma reconfiguração explícita, tenta reconectar
        if (!s_is_reconfiguring) {
            ESP_LOGI(TAG, "Tentando reconectar em 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_is_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso, endereco IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_init_sta(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "SSID vazio fornecido na inicialização");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao inicializar Wi-Fi (%s)", esp_err_to_name(ret));
        return ret;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    // habilita PMF (Protected Management Frames) para compatibilidade com roteadores modernos e WPA3 Transition
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

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

    // se a pilha de Wi-Fi ainda não foi inicializada (ex: NVS estava vazia no boot)
    if (s_sta_netif == NULL) {
        ESP_LOGI(TAG, "Pilha Wi-Fi ainda nao inicializada, inicializando pela primeira vez...");
        return wifi_manager_init_sta(ssid, password);
    }

    ESP_LOGI(TAG, "Reconfigurando Wi-Fi para novo SSID: '%s'...", ssid);

    // ativa flag para evitar que a ISR tente reconectar na rede antiga
    s_is_reconfiguring = true;
    s_is_connected = false;

    // para o driver de Wi-Fi para resetar limpo a máquina de estados
    esp_wifi_disconnect();
    esp_wifi_stop();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir nova configuracao Wi-Fi (%s)", esp_err_to_name(ret));
        s_is_reconfiguring = false;
        return ret;
    }

    ret = esp_wifi_start();
    s_is_reconfiguring = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao reiniciar Wi-Fi (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Conectando a nova rede Wi-Fi '%s'...", ssid);
    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}
