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

static esp_mqtt5_subscribe_property_config_t subscribe_property = {
    .subscribe_id = 25555,
    .no_local_flag = false,
    .retain_as_published_flag = false,
    .retain_handle = 0,
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_subscribe_property_config_t subscribe1_property = {
    .subscribe_id = 25555,
    .no_local_flag = true,
    .retain_as_published_flag = false,
    .retain_handle = 0,
};

static esp_mqtt5_unsubscribe_property_config_t unsubscribe_property = {
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_disconnect_property_config_t disconnect_property = {
    .session_expiry_interval = 60,
    .disconnect_reason = 0,
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
        print_user_property(event->property->user_property);
        s_is_mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_publish_property(client, &publish_property);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        print_user_property(event->property->user_property);
        ESP_LOGI(TAG, "payload_format_indicator is %d", event->property->payload_format_indicator);
        ESP_LOGI(TAG, "response_topic is %.*s", event->property->response_topic_len, event->property->response_topic);
        ESP_LOGI(TAG, "correlation_data is %.*s", event->property->correlation_data_len, event->property->correlation_data);
        ESP_LOGI(TAG, "content_type is %.*s", event->property->content_type_len, event->property->content_type);
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        print_user_property(event->property->user_property);
        ESP_LOGI(TAG, "MQTT5 return code is %d", event->error_handle->connect_return_code);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
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

#define SENSOR_E18_PIN GPIO_NUM_48
#define ESP_INTR_FLAG_DEFAULT 0
#define SENSOR_DEBOUNCE_US 300000ULL // Debounce de 300 ms (em microssegundos)

static QueueHandle_t s_sensor_evt_queue = NULL;
static volatile int64_t s_last_sensor_interrupt_time = 0;

/*
 * @brief Rotina de Atendimento a Interrupcao (ISR) vinculada ao GPIO 19
 *
 * Executada no contexto da ISR (alocada na IRAM).
 * Aplica filtro de debounce nao-bloqueante baseado em temporizador de hardware e
 * envia uma notificacao para a fila FreeRTOS sem invocar chamadas de I/O bloqueantes.
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
 * @brief Tarefa FreeRTOS dedicada a processar eventos do sensor e imprimir no terminal
 */
static void sensor_task(void *pvParameters)
{
    uint32_t io_num;
    uint32_t detection_count = 0;

    while (1) {
        // Aguarda evento da ISR por ate 3 segundos
        if (xQueueReceive(s_sensor_evt_queue, &io_num, pdMS_TO_TICKS(3000))) {
            detection_count++;
            ESP_LOGI(TAG, "=======================================================");
            ESP_LOGI(TAG, "[ISR DETECTADA] Sensor E18-D80NK acionado no GPIO %" PRIu32 "!", io_num);
            ESP_LOGI(TAG, "Evento: Objeto/Peca detectada. Total de contagens: %" PRIu32, detection_count);
            ESP_LOGI(TAG, "=======================================================");
            printf(">>> [TERMINAL] Interrupcao ISR processada com sucesso no GPIO %lu (Deteccao #%lu) <<<\n\n",
                   (unsigned long)io_num, (unsigned long)detection_count);

            // verifica se o cliente mqtt está conectado
            if(s_is_mqtt_connected){
                //
                char payload[128];
                snprintf(payload, sizeof(payload), "{\"gpio\": %lu, \"contagem\": %lu}", (unsigned long)io_num, (unsigned long)detection_count);

                esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
                int msg_id = esp_mqtt_client_publish(s_mqtt_client, "sensor/e18/contagem", payload, 0, 2, 0);
                ESP_LOGI(TAG, "Mensagem MQTT enviada, id: %d", msg_id);
            }else{
                ESP_LOGI(TAG, "Broker MQTT nao esta conectado");
            }
        } else {
            // Diagnostico periodico para checar o nivel eletrico do pino em tempo real
            int current_level = gpio_get_level(SENSOR_E18_PIN);
            ESP_LOGI(TAG, "[DIAGNOSTICO E18] Leitura no GPIO %d: Nivel %s (%d)",
                     SENSOR_E18_PIN,
                     current_level ? "ALTO (1 - Sem obstaculo / Repouso)" : "BAIXO (0 - Obstaculo detectado / Travado)",
                     current_level);
        }
    }
}

/*
 * @brief Configura o GPIO 19 e vincula a ISR para o sensor E18-D80NK
 */
static void sensor_e18_init(void)
{
    // Cria fila de comunicacao segura entre ISR e Tarefa
    s_sensor_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    // Cria a tarefa FreeRTOS para impressao/processamento no terminal
    xTaskCreate(sensor_task, "sensor_e18_task", 3072, NULL, 10, NULL);

    // Configuracao do pino GPIO 19:
    // O sensor E18-D80NK possui saida NPN em coletor aberto (ativo em nivel logico BAIXO quando detecta obstaculo).
    // Configuramos com pull-up interno ativado e interrupcao na borda de descida (GPIO_INTR_NEGEDGE).
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_E18_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Instala servico de interrupcao GPIO (se ainda nao tiver sido instalado)
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao instalar servico ISR GPIO: %s", esp_err_to_name(err));
    }

    // Vincula a ISR ao GPIO 19
    ESP_ERROR_CHECK(gpio_isr_handler_add(SENSOR_E18_PIN, sensor_gpio_isr_handler, (void *)SENSOR_E18_PIN));

    ESP_LOGI(TAG, "Sensor E18-D80NK configurado no GPIO %d com ISR vinculada (NEGEDGE).", SENSOR_E18_PIN);
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

    /* Inicializa o sensor E18-D80NK e vincula a ISR */
    sensor_e18_init();

    /* Tenta conectar ao Wi-Fi configurado no menuconfig sem abortar em caso de falha */
    esp_err_t wifi_err = example_connect();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi nao conectado (%s). Operacao offline ativa: testando sensor E18-D80NK localmente.", esp_err_to_name(wifi_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso! Iniciando cliente MQTT5...");
        mqtt5_app_start();
    }
}
