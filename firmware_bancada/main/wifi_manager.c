#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lora_receiver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "WIFI_MGR";
static esp_netif_t *s_sta_netif = NULL;
static bool s_is_initialized = false;
static bool s_is_connected = false;
static bool s_is_reconfiguring = false;
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint8_t s_consecutive_failures = 0;
static char s_current_ssid[33] = {0};
static SemaphoreHandle_t s_wifi_mutex = NULL;
static StaticSemaphore_t s_wifi_mutex_buf;

#define WIFI_MAX_FAILURES_BEFORE_LORA_REQ 3

static void wifi_mutex_init_once(void) {
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutexStatic(&s_wifi_mutex_buf);
    }
}

static void trim_str(char *str) {
    if (str == NULL) {
        return;
    }
    // remove caracteres de controle e espaços no fim
    size_t len = strlen(str);
    while (len > 0 && (isspace((unsigned char)str[len - 1]) || str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[--len] = '\0';
    }
    // remove no início
    char *start = str;
    while (*start && (isspace((unsigned char)*start) || *start == '\r' || *start == '\n')) {
        start++;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

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

static void reconnect_timer_callback(void *arg) {
    if (!s_is_reconfiguring && !s_is_connected) {
        ESP_LOGI(TAG, "Tentando reconectar ao Wi-Fi agora...");
        esp_wifi_connect();
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

        if (reason == 201) { // WIFI_REASON_NO_AP_FOUND
            ESP_LOGW(TAG,
                     "[AVISO] Verifique se a rede Wi-Fi opera em 2.4 GHz (ESP32 nao suporta 5 GHz) e se o SSID esta "
                     "correto.");
        }

        // se nao estiver em meio a uma reconfiguração explícita, agenda reconexão não bloqueante via timer
        if (!s_is_reconfiguring) {
            s_consecutive_failures++;
            if (s_consecutive_failures >= WIFI_MAX_FAILURES_BEFORE_LORA_REQ) {
                ESP_LOGW(TAG, "Falhas de Wi-Fi consecutivas (%d). Solicitando novas credenciais via LoRa...", s_consecutive_failures);
                lora_send_req_config();
                // reinicia o contador para não floodar a rede LoRa
                s_consecutive_failures = 0;
            }

            ESP_LOGI(TAG, "Agendando tentativa de reconexao em 2 segundos...");
            if (s_reconnect_timer != NULL) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, 2000000); // 2 segundos (em microssegundos)
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_is_connected = true;
        s_consecutive_failures = 0;
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
        }
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso, endereco IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_init_sta(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "SSID vazio fornecido na inicialização");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_mutex_init_once();
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    char clean_ssid[33] = {0};
    strncpy(clean_ssid, ssid, sizeof(clean_ssid) - 1);
    trim_str(clean_ssid);

    if (s_is_initialized) {
        // se já está inicializado com o mesmo SSID, nada a fazer
        if (strncmp(s_current_ssid, clean_ssid, sizeof(s_current_ssid)) == 0) {
            ESP_LOGI(TAG, "Wi-Fi já inicializado para o SSID '%s'. Chamada redundante ignorada.", clean_ssid);
            xSemaphoreGive(s_wifi_mutex);
            return ESP_OK;
        }

        // se o SSID for diferente, redireciona para reconfiguração segura
        ESP_LOGI(TAG, "Wi-Fi já ativo, atualizando credenciais para novo SSID: '%s'...", clean_ssid);
        xSemaphoreGive(s_wifi_mutex);
        return wifi_manager_reconfigure(ssid, password);
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &reconnect_timer_callback,
            .name = "wifi_reconnect_tmr",
        };
        esp_timer_create(&timer_args, &s_reconnect_timer);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao inicializar Wi-Fi (%s)", esp_err_to_name(ret));
        xSemaphoreGive(s_wifi_mutex);
        return ret;
    }

    // configura código de país para o Brasil (habilita canais 1 a 13)
    wifi_country_t country = {
        .cc = "BR",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, clean_ssid, sizeof(wifi_config.sta.ssid) - 1);

    if (password != NULL && strlen(password) > 0) {
        char clean_pass[65] = {0};
        strncpy(clean_pass, password, sizeof(clean_pass) - 1);
        trim_str(clean_pass);
        strncpy((char *)wifi_config.sta.password, clean_pass, sizeof(wifi_config.sta.password) - 1);
    }

    // scan completo em todos os canais e conexão pelo melhor sinal
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.threshold.rssi = -127;

    // habilita PMF (Protected Management Frames) para compatibilidade com roteadores modernos e WPA3 Transition
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Aviso ao definir modo STA (%s)", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Wi-Fi em estado conectando ao aplicar config; parando para aplicar...");
        esp_wifi_disconnect();
        esp_wifi_stop();
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir configuracao Wi-Fi (%s)", esp_err_to_name(ret));
        xSemaphoreGive(s_wifi_mutex);
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "Falha ao iniciar Wi-Fi (%s)", esp_err_to_name(ret));
        xSemaphoreGive(s_wifi_mutex);
        return ret;
    }

    strncpy(s_current_ssid, clean_ssid, sizeof(s_current_ssid) - 1);
    s_is_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi STA configurado e iniciado para SSID: '%s' (canais 1-13)", clean_ssid);
    xSemaphoreGive(s_wifi_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_reconfigure(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID invalido para reconfiguracao");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_mutex_init_once();
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    // se a pilha de Wi-Fi ainda não foi inicializada (ex: NVS estava vazia no boot)
    if (!s_is_initialized || s_sta_netif == NULL) {
        ESP_LOGI(TAG, "Pilha Wi-Fi ainda nao inicializada, inicializando pela primeira vez...");
        xSemaphoreGive(s_wifi_mutex);
        return wifi_manager_init_sta(ssid, password);
    }

    char clean_ssid[33] = {0};
    strncpy(clean_ssid, ssid, sizeof(clean_ssid) - 1);
    trim_str(clean_ssid);

    ESP_LOGI(TAG, "Reconfigurando Wi-Fi para novo SSID: '%s'...", clean_ssid);

    // cancela qualquer tentativa de reconexão anterior agendada
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }

    // ativa flag para evitar que a ISR tente reconectar na rede antiga durante o processo
    s_is_reconfiguring = true;
    s_is_connected = false;
    s_consecutive_failures = 0;

    // para o driver de Wi-Fi para resetar limpo a máquina de estados
    esp_wifi_disconnect();
    esp_wifi_stop();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, clean_ssid, sizeof(wifi_config.sta.ssid) - 1);

    if (password != NULL && strlen(password) > 0) {
        char clean_pass[65] = {0};
        strncpy(clean_pass, password, sizeof(clean_pass) - 1);
        trim_str(clean_pass);
        strncpy((char *)wifi_config.sta.password, clean_pass, sizeof(wifi_config.sta.password) - 1);
    }

    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.threshold.rssi = -127;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Aguardando parada completa da interface Wi-Fi...");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_wifi_disconnect();
        esp_wifi_stop();
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir nova configuracao Wi-Fi (%s)", esp_err_to_name(ret));
        s_is_reconfiguring = false;
        xSemaphoreGive(s_wifi_mutex);
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "Falha ao reiniciar Wi-Fi (%s)", esp_err_to_name(ret));
        s_is_reconfiguring = false;
        xSemaphoreGive(s_wifi_mutex);
        return ret;
    }

    strncpy(s_current_ssid, clean_ssid, sizeof(s_current_ssid) - 1);
    s_is_reconfiguring = false;

    ESP_LOGI(TAG, "Wi-Fi reiniciado com SSID '%s', o driver irá reconectar automaticamente", clean_ssid);
    xSemaphoreGive(s_wifi_mutex);
    return ESP_OK;
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}

bool wifi_manager_is_initialized(void) {
    return s_is_initialized;
}
