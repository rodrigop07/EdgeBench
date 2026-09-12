#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "MQTT_MGR";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_is_mqtt_connected = false;

static esp_mqtt5_user_property_item_t user_property_arr[] = {{"board", "esp32s3"}, {"u", "user"}, {"p", "password"}};

#define USE_PROPERTY_ARR_SIZE (sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t))

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Broker conectado com sucesso!");
        s_is_mqtt_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED: Desconectado do Broker.");
        s_is_mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
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

esp_err_t mqtt_manager_start(const char *broker_uri) {
    if (broker_uri == NULL || strlen(broker_uri) == 0) {
        ESP_LOGE(TAG, "URI do broker inválida");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Iniciando cliente MQTT5 para o broker: %s", broker_uri);

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
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "EdgeBench disconnected",
        .session.last_will.msg_len = 22,
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

    char payload[256];
    if (offline) {
        snprintf(payload, sizeof(payload),
                 "{\"gpio\": %lu, \"contagem\": %lu, \"horario\": \"%s\", "
                 "\"timestamp\": %lld, \"offline\": true}",
                 (unsigned long)SENSOR_E18_PIN, (unsigned long)record->count, time_str, (long long)record->timestamp);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"gpio\": %lu, \"contagem\": %lu, \"horario\": \"%s\", "
                 "\"timestamp\": %lld}",
                 (unsigned long)SENSOR_E18_PIN, (unsigned long)record->count, time_str, (long long)record->timestamp);
    }

    esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, "sensor/e18/contagem", payload, 0, 1, 0);

    if (msg_id != -1) {
        ESP_LOGI(TAG, "Mensagem MQTT enviada com QoS 1, id: %d (offline: %s)", msg_id, offline ? "true" : "false");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Falha ao publicar mensagem MQTT");
        return ESP_FAIL;
    }
}
