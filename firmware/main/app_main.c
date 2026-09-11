/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <time.h>
#include <sys/time.h>
#include "esp_netif_sntp.h"

// estrutura para envio de dados do sensor com tempo
typedef struct {
    uint32_t count;
    time_t timestamp;
} sensor_data_record_t;

// variáveis globais para gerenciar a conexão MQTT
static const char *TAG = "mqtt5_example";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_is_mqtt_connected = false;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static esp_mqtt5_user_property_item_t user_property_arr[] = {
        {"board", "esp32s3"},
        {"u", "user"},
        {"p", "password"}
    };

#define USE_PROPERTY_ARR_SIZE   sizeof(user_property_arr)/sizeof(esp_mqtt5_user_property_item_t)

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static void print_user_property(mqtt5_user_property_handle_t user_property)
{
    if (user_property) {
        uint8_t count = esp_mqtt5_client_get_user_property_count(user_property);
        if (count) {
            esp_mqtt5_user_property_item_t *item = malloc(count * sizeof(esp_mqtt5_user_property_item_t));
            if (esp_mqtt5_client_get_user_property(user_property, item, &count) == ESP_OK) {
                for (int i = 0; i < count; i ++) {
                    esp_mqtt5_user_property_item_t *t = &item[i];
                    ESP_LOGI(TAG, "key is %s, value is %s", t->key, t->value);
                    free((char *)t->key);
                    free((char *)t->value);
                }
            }
            free(item);
        }
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    ESP_LOGD(TAG, "free heap size is %" PRIu32 ", minimum %" PRIu32, esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: broker conectado com sucesso");
        s_is_mqtt_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        if (event->property) {
            print_user_property(event->property->user_property);
        }
        s_is_mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        if (event->property) {
            print_user_property(event->property->user_property);
        }
        esp_mqtt5_client_set_publish_property(client, &publish_property);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        if (event->property) {
            print_user_property(event->property->user_property);
        }
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        if (event->property) {
            print_user_property(event->property->user_property);
            ESP_LOGI(TAG, "payload_format_indicator is %d", event->property->payload_format_indicator);
            ESP_LOGI(TAG, "response_topic is %.*s", event->property->response_topic_len, event->property->response_topic);
            ESP_LOGI(TAG, "correlation_data is %.*s", event->property->correlation_data_len, event->property->correlation_data);
            ESP_LOGI(TAG, "content_type is %.*s", event->property->content_type_len, event->property->content_type);
        }
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->property) {
            print_user_property(event->property->user_property);
        }
        if (event->error_handle) {
            ESP_LOGI(TAG, "MQTT5 return code is %d", event->error_handle->connect_return_code);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt5_app_start(void)
{
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
        .broker.address.uri = CONFIG_BROKER_URL,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = false,
        //.credentials.username = "123",
        //.credentials.authentication.password = "456",
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "i will leave",
        .session.last_will.msg_len = 12,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    
    #if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];
    
    if (strcmp(mqtt5_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt5_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
    #endif /* CONFIG_BROKER_URL_FROM_STDIN */
    
    s_mqtt_client = esp_mqtt_client_init(&mqtt5_cfg);

    /* Set connection properties and user properties */
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(s_mqtt_client, &connect_property);

    /* If you call esp_mqtt5_client_set_user_property to set user properties, DO NOT forget to delete them.
     * esp_mqtt5_client_set_connect_property will malloc buffer to store the user_property and you can delete it after
     */
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

#define SENSOR_E18_PIN GPIO_NUM_48 // define o pino onde esta conectado o sensor
#define ESP_INTR_FLAG_DEFAULT 0 // define a flag de interrupcao
#define SENSOR_DEBOUNCE_US 300000ULL // Debounce de 300 ms para evitar falsos positivos

static QueueHandle_t s_sensor_evt_queue = NULL;
static QueueHandle_t s_storage_queue = NULL;
static volatile int64_t s_last_sensor_interrupt_time = 0;

/*
 * @brief rotina de atendimento a interrupcao (ISR) vinculada ao GPIO 19
 *
 * executada no contexto da ISR (alocada na IRAM)
 * aplica filtro de debounce nao-bloqueante baseado em temporizador de hardware e
 * envia uma notificacao para a fila FreeRTOS sem invocar chamadas de I/O bloqueantes
 */
static void IRAM_ATTR sensor_gpio_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_sensor_interrupt_time) > SENSOR_DEBOUNCE_US) {
        s_last_sensor_interrupt_time = now;
        uint32_t gpio_num = (uint32_t)arg;
        BaseType_t high_task_wakeup = pdFALSE;

        // Impressao imediata e segura em nivel de ROM direto do contexto da ISR
        esp_rom_printf("\n[ISR] Interrupcao detectada no GPIO %lu (Sensor E18-D80NK)!\n", (unsigned long)gpio_num);

        xQueueSendFromISR(s_sensor_evt_queue, &gpio_num, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

/*
 * @brief Tarefa FreeRTOS vinculada ao Core 1 dedicada a processar eventos do sensor e enviar para o Core 0
 */
static void core1_sensor_task(void *pvParameters)
{
    // instala a ISR no core 1
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(SENSOR_E18_PIN, sensor_gpio_isr_handler, (void*)SENSOR_E18_PIN);

    uint32_t io_num;
    uint32_t detection_count = 0;

    while(1){
        if(xQueueReceive(s_sensor_evt_queue, &io_num, portMAX_DELAY)){
            detection_count++;
            time_t now = time(NULL);
            sensor_data_record_t record = {
                .count = detection_count,
                .timestamp = now,
            };
            ESP_LOGI("Core1", "Deteccao no core 1 - Total: %lu", (unsigned long)detection_count);
            xQueueSend(s_storage_queue, &record, 0);
        }
    }
}

/*
 * @brief Tarefa FreeRTOS vinculada ao Core 0 dedicada a armazenar e enviar dados para o Core 0
 */
static void core0_storage_task(void *pvParameters){
    sensor_data_record_t record;

    while(1){
        // fica esperando dados vindos do core 1
        if(xQueueReceive(s_storage_queue, &record, portMAX_DELAY)){
            struct tm timeinfo;
            char time_str[64];
            localtime_r(&record.timestamp, &timeinfo);

            // se o relógio já foi sincronizado via NTP (ano >= 2024), formata a data/hora local
            if (timeinfo.tm_year >= (2024 - 1900)) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            } else {
                snprintf(time_str, sizeof(time_str), "Nao sincronizado (uptime %llds)", (long long)(esp_timer_get_time() / 1000000ULL));
            }

            ESP_LOGI("Core0", "Contagem recebida do Core 1: %lu [Horario: %s], gravando na flash", (unsigned long)record.count, time_str);
            
            if(s_is_mqtt_connected){
                char payload[256];
                snprintf(payload, sizeof(payload),
                         "{\"gpio\": %lu, \"contagem\": %lu, \"horario\": \"%s\", \"timestamp\": %lld}",
                         (unsigned long)SENSOR_E18_PIN,
                         (unsigned long)record.count,
                         time_str,
                         (long long)record.timestamp);
                esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
                int msg_id = esp_mqtt_client_publish(s_mqtt_client, "sensor/e18/contagem", payload, 0, 1, 0);
                ESP_LOGI(TAG, "Mensagem MQTT enviada, id: %d, payload: %s", msg_id, payload);
            }else{
                ESP_LOGI(TAG, "Broker MQTT nao esta conectado");
            }
        }
    }
}

/*
 * @brief Configura o GPIO 48 e vincula a ISR para o sensor E18-D80NK
 */
static void sensor_e18_init(void)
{
    // configuracao do pino GPIO 48
    // o sensor E18-D80NK possui saida NPN em coletor aberto (ativo em nivel logico BAIXO quando detecta obstaculo)
    // configuramos com pull-up interno ativado e interrupcao na borda de descida (GPIO_INTR_NEGEDGE)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_E18_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // cria a task para o core 1
    xTaskCreatePinnedToCore(core1_sensor_task, "core1_sensor_task", 4096, NULL, 10, NULL, 1);

    // cria a task para o core 0
    xTaskCreatePinnedToCore(core0_storage_task, "core0_storage_task", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Sensor E18-D80NK configurado no GPIO %d.", SENSOR_E18_PIN);
}

void app_main(void)
{

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // cria as filas de comunicação entre os cores
    s_sensor_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    s_storage_queue = xQueueCreate(10, sizeof(sensor_data_record_t));

    // verifica se as filas foram criadas com sucesso
    if(s_storage_queue == NULL){
        ESP_LOGE(TAG, "Falha ao criar s_storage_queue");
        return;
    }
    if(s_sensor_evt_queue == NULL){
        ESP_LOGE(TAG, "Falha ao criar s_sensor_evt_queue");
        return;
    }

    // inicializa o sensor E18-D80NK e vincula a ISR
    sensor_e18_init();

    // tenta conectar ao wifi sem abortar em caso de falha
    esp_err_t wifi_err = example_connect();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi nao conectado (%s). Operacao offline ativa: testando sensor E18-D80NK localmente.", esp_err_to_name(wifi_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso!");

        // configura fuso horario para Horario de Brasilia (UTC-3)
        setenv("TZ", "<-03>3", 1);
        tzset();

        // inicializa o cliente SNTP para sincronizacao da hora via NTP
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("a.st1.ntp.br");
        esp_netif_sntp_init(&sntp_config);
        ESP_LOGI(TAG, "SNTP inicializado (servidor: a.st1.ntp.br, fuso: UTC-3)");

        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK) {
            ESP_LOGI(TAG, "Horario sincronizado com sucesso via SNTP!");
        } else {
            ESP_LOGW(TAG, "Aguardando sincronizacao de horario em segundo plano...");
        }

        ESP_LOGI(TAG, "Iniciando cliente MQTT5...");
        mqtt5_app_start();
    }
}
