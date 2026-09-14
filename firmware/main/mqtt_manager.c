#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "MQTT_MGR";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_is_mqtt_connected = false;
static uint16_t s_bench_id = 1;

// controle de two way ACK (confirmação PUBACK com timeout)
static SemaphoreHandle_t s_puback_sem = NULL;
static volatile int s_pending_puback_msg_id = -1;

// buffers de tópicos dinâmicos por bancada
static char s_topic_production[64];
static char s_topic_status[64];
static char s_lwt_msg[64];

static esp_mqtt5_user_property_item_t user_property_arr[] = {
    {"board", "esp32s3"},
    {"app", "EdgeBench"},
};

#define USE_PROPERTY_ARR_SIZE (sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t))

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
};

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Broker conectado com sucesso!");
        s_is_mqtt_connected = true;

        // publica status "online" no tópico de diagnóstico/status retido (QoS 1)
        if (s_mqtt_client != NULL && strlen(s_topic_status) > 0) {
            char status_payload[128];
            snprintf(status_payload, sizeof(status_payload),
                     "{\"bancada\": %u, \"status\": \"online\", \"uptime_s\": %lld}", s_bench_id,
                     (long long)(esp_timer_get_time() / 1000000ULL));
            esp_mqtt_client_publish(s_mqtt_client, s_topic_status, status_payload, 0, 1, 1);
            ESP_LOGI(TAG, "Status de conexao da bancada %u publicado em %s", s_bench_id, s_topic_status);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED: Desconectado do Broker.");
        s_is_mqtt_connected = false;
        if (s_puback_sem != NULL && s_pending_puback_msg_id != -1) {
            s_pending_puback_msg_id = -1;
            xSemaphoreGive(s_puback_sem);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED (PUBACK recebido), msg_id=%d", event->msg_id);
        if (s_puback_sem != NULL && event->msg_id == s_pending_puback_msg_id) {
            s_pending_puback_msg_id = -1;
            xSemaphoreGive(s_puback_sem);
        }
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA: TOPIC=%.*s DATA=%.*s", event->topic_len, event->topic, event->data_len,
                 event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Erro de transporte TCP: 0x%x", event->error_handle->esp_tls_last_esp_err);
        }
        break;

    default:
        break;
    }
}

esp_err_t mqtt_manager_start(const char *broker_uri, uint16_t bench_id) {
    if (broker_uri == NULL || strlen(broker_uri) == 0) {
        ESP_LOGE(TAG, "URI do broker inválida");
        return ESP_ERR_INVALID_ARG;
    }

    s_bench_id = (bench_id > 0) ? bench_id : 1;

    // configura os tópicos padronizados da bancada
    snprintf(s_topic_production, sizeof(s_topic_production), "fabrica/bancada_%u/producao", s_bench_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "fabrica/bancada_%u/status", s_bench_id);
    snprintf(s_lwt_msg, sizeof(s_lwt_msg), "{\"bancada\": %u, \"status\": \"offline\"}", s_bench_id);

    ESP_LOGI(TAG, "Iniciando cliente MQTT5 para bancada %u:", s_bench_id);
    ESP_LOGI(TAG, "  Broker: %s", broker_uri);
    ESP_LOGI(TAG, "  Topico Producao: %s", s_topic_production);
    ESP_LOGI(TAG, "  Topico Status/LWT: %s", s_topic_status);

    esp_mqtt5_connection_property_config_t connect_property = {
        .session_expiry_interval = 10,
        .maximum_packet_size = 1024,
        .receive_maximum = 65535,
        .topic_alias_maximum = 2,
        .request_resp_info = true,
        .request_problem_info = true,
        .will_delay_interval = 10,
        .payload_format_indicator = true,
        .message_expiry_interval = 10,
        .response_topic = "/test/response",
        .correlation_data = "123456",
        .correlation_data_len = 6,
    };

    esp_mqtt_client_config_t mqtt5_cfg = {
        .broker.address.uri = broker_uri,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = false,
        .session.last_will.topic = s_topic_status,
        .session.last_will.msg = s_lwt_msg,
        .session.last_will.msg_len = strlen(s_lwt_msg),
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt5_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Falha ao criar instância do cliente MQTT");
        return ESP_FAIL;
    }

    // configura propriedades de conexão MQTT 5
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(s_mqtt_client, &connect_property);

    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    return esp_mqtt_client_start(s_mqtt_client);
}

bool mqtt_manager_is_connected(void) {
    return s_is_mqtt_connected;
}

esp_err_t mqtt_manager_publish_detection(const sensor_data_record_t *record, bool offline) {
    if (s_mqtt_client == NULL || !s_is_mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm timeinfo;
    char time_str[64];
    time_t ts = record->timestamp;
    localtime_r(&ts, &timeinfo);

    if (timeinfo.tm_year >= (2024 - 1900)) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        snprintf(time_str, sizeof(time_str), "Nao sincronizado (uptime %llds)",
                 (long long)(esp_timer_get_time() / 1000000ULL));
    }

    // payload que será enviado ao mqtt contendo as informações do registro
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"bancada\": %u, \"contagem\": %lu, \"horario\": \"%s\", "
             "\"timestamp\": %lld, \"quantidade\": 1, \"modo_offline\": %s}",
             s_bench_id, (unsigned long)record->count, time_str, (long long)record->timestamp,
             offline ? "true" : "false");

    esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_production, payload, 0, 1, 0);

    if (msg_id != -1) {
        ESP_LOGI(TAG, "Mensagem publicada em '%s' (QoS 1, id: %d, offline: %s)", s_topic_production, msg_id,
                 offline ? "true" : "false");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Falha ao publicar mensagem MQTT no topico %s", s_topic_production);
        return ESP_FAIL;
    }
}

esp_err_t mqtt_manager_set_broker(const char *broker_uri) {
    if (broker_uri == NULL || strlen(broker_uri) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Reconfigurando broker MQTT a quente para: %s", broker_uri);
    s_is_mqtt_connected = false;
    esp_mqtt_client_stop(s_mqtt_client);

    esp_err_t ret = esp_mqtt_client_set_uri(s_mqtt_client, broker_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao definir nova URI no cliente MQTT (%s)", esp_err_to_name(ret));
        return ret;
    }

    return esp_mqtt_client_start(s_mqtt_client);
}

esp_err_t mqtt_manager_set_bench_id(uint16_t bench_id) {
    if (bench_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bench_id = bench_id;

    // atualiza tópicos
    snprintf(s_topic_production, sizeof(s_topic_production), "fabrica/bancada_%u/producao", s_bench_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "fabrica/bancada_%u/status", s_bench_id);
    snprintf(s_lwt_msg, sizeof(s_lwt_msg), "{\"bancada\": %u, \"status\": \"offline\"}", s_bench_id);

    ESP_LOGI(TAG, "ID da Bancada atualizado a quente para %u:", s_bench_id);
    ESP_LOGI(TAG, "  Novo Topico Producao: %s", s_topic_production);
    ESP_LOGI(TAG, "  Novo Topico Status: %s", s_topic_status);

    // se estiver conectado, publica status de transicao no novo tópico retido
    if (s_mqtt_client != NULL && s_is_mqtt_connected) {
        char status_payload[128];
        snprintf(status_payload, sizeof(status_payload),
                 "{\"bancada\": %u, \"status\": \"online\", \"uptime_s\": %lld}", s_bench_id,
                 (long long)(esp_timer_get_time() / 1000000ULL));
        esp_mqtt_client_publish(s_mqtt_client, s_topic_status, status_payload, 0, 1, 1);
    }

    return ESP_OK;
}

esp_err_t mqtt_manager_publish_detection_sync(const sensor_data_record_t *record, bool offline,
                                              TickType_t timeout_ticks) {
    if (s_mqtt_client == NULL || !s_is_mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_puback_sem == NULL) {
        s_puback_sem = xSemaphoreCreateBinary();
        if (s_puback_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(s_puback_sem, 0); // limpa qualquer token residual anterior
    }

    struct tm timeinfo;
    char time_str[64];
    time_t ts = record->timestamp;
    localtime_r(&ts, &timeinfo);

    if (timeinfo.tm_year >= (2024 - 1900)) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        snprintf(time_str, sizeof(time_str), "Nao sincronizado (uptime %llds)",
                 (long long)(esp_timer_get_time() / 1000000ULL));
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"bancada\": %u, \"contagem\": %lu, \"horario\": \"%s\", "
             "\"timestamp\": %lld, \"quantidade\": 1, \"modo_offline\": %s}",
             s_bench_id, (unsigned long)record->count, time_str, (long long)record->timestamp,
             offline ? "true" : "false");

    esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_production, payload, 0, 1, 0);

    if (msg_id == -1) {
        ESP_LOGE(TAG, "Falha ao enfileirar mensagem de replay offline no MQTT");
        return ESP_FAIL;
    }

    s_pending_puback_msg_id = msg_id;

    // aguarda a chegada do PUBACK
    if (xSemaphoreTake(s_puback_sem, timeout_ticks) == pdTRUE) {
        if (s_is_mqtt_connected) {
            ESP_LOGD(TAG, "PUBACK confirmado para registro offline (msg_id: %d)", msg_id);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Conexao perdida enquanto aguardava PUBACK (msg_id: %d)", msg_id);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "Timeout aguardando PUBACK do registro offline (msg_id: %d)", msg_id);
        s_pending_puback_msg_id = -1;
        return ESP_ERR_TIMEOUT;
    }
}
