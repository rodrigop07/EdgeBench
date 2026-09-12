#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lora_receiver.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs_manager.h"
#include "protocol_examples_common.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// estrutura para envio de dados do sensor com tempo
typedef struct {
    uint32_t count;
    time_t timestamp;
} __attribute__((packed)) sensor_data_record_t;

// variáveis globais para gerenciar a conexão MQTT
static const char *TAG = "mqtt5_example";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_is_mqtt_connected = false;

static void log_error_if_nonzero(const char *message, int error_code) {
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static esp_mqtt5_user_property_item_t user_property_arr[] = {{"board", "esp32s3"}, {"u", "user"}, {"p", "password"}};

#define USE_PROPERTY_ARR_SIZE sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t)

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static void print_user_property(mqtt5_user_property_handle_t user_property) {
    if (user_property) {
        uint8_t count = esp_mqtt5_client_get_user_property_count(user_property);
        if (count) {
            esp_mqtt5_user_property_item_t *item = malloc(count * sizeof(esp_mqtt5_user_property_item_t));
            if (esp_mqtt5_client_get_user_property(user_property, item, &count) == ESP_OK) {
                for (int i = 0; i < count; i++) {
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

/**
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    ESP_LOGD(TAG, "free heap size is %" PRIu32 ", minimum %" PRIu32, esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size());
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
            ESP_LOGI(TAG, "response_topic is %.*s", event->property->response_topic_len,
                     event->property->response_topic);
            ESP_LOGI(TAG, "correlation_data is %.*s", event->property->correlation_data_len,
                     event->property->correlation_data);
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
                log_error_if_nonzero("captured as transport's socket errno",
                                     event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt5_app_start(void) {
    // pega a url do broker na NVS
    static char s_broker_uri[128];
    nvs_manager_get_broker_url(s_broker_uri, sizeof(s_broker_uri));

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
        .broker.address.uri = s_broker_uri,
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

    s_mqtt_client = esp_mqtt_client_init(&mqtt5_cfg);

    /* Set connection properties and user properties */
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(s_mqtt_client, &connect_property);

    /* If you call esp_mqtt5_client_set_user_property to set user properties, DO
     * NOT forget to delete them. esp_mqtt5_client_set_connect_property will
     * malloc buffer to store the user_property and you can delete it after
     */
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

    /* The last argument may be used to pass data to the event handler, in this
     * example mqtt_event_handler */
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

#define SENSOR_E18_PIN GPIO_NUM_48   // define o pino onde esta conectado o sensor
#define ESP_INTR_FLAG_DEFAULT 0      // define a flag de interrupcao
#define SENSOR_DEBOUNCE_US 300000ULL // Debounce de 300 ms para evitar falsos positivos

static QueueHandle_t s_sensor_evt_queue = NULL;
static QueueHandle_t s_storage_queue = NULL;
static volatile int64_t s_last_sensor_interrupt_time = 0;

/**
 * @brief rotina de atendimento a interrupcao no GPIO48
 *
 * executada no contexto da ISR (alocada na IRAM)
 * aplica filtro de debounce nao-bloqueante baseado em temporizador de hardware
 * e envia uma notificacao para a fila FreeRTOS sem invocar chamadas de I/O
 * bloqueantes
 */
static void IRAM_ATTR sensor_gpio_isr_handler(void *arg) {
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

/**
 * @brief Tarefa FreeRTOS vinculada ao Core 1 dedicada a processar eventos do
 * sensor e enviar para o Core 0
 */
static void core1_sensor_task(void *pvParameters) {
    gpio_isr_handler_add(SENSOR_E18_PIN, sensor_gpio_isr_handler, (void *)SENSOR_E18_PIN);
    uint32_t io_num;
    uint32_t detection_count = 0;

    while (1) {
        if (xQueueReceive(s_sensor_evt_queue, &io_num, portMAX_DELAY)) {
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
 * @brief Tarefa FreeRTOS vinculada ao Core 0 dedicada a armazenar e enviar
 * dados para o Core 0
 */
static esp_err_t init_spiffs(void) {
    ESP_LOGI(TAG, "Inicializando SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Falha ao montar ou formatar sistema de arquivos");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Falha ao encontrar partição SPIFFS");
        } else {
            ESP_LOGE(TAG, "Falha ao inicializar SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter informacoes da particao SPIFFS (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Tamanho da particao: total: %d, usada: %d", total, used);
    }
    return ESP_OK;
}

static void core0_storage_task(void *pvParameters) {
    sensor_data_record_t record;

    const char *log_file_path = "/spiffs/offline_log.bin";

    while (1) {
        // usa um timeout para permitir envio de logs offline quando reconectar
        if (xQueueReceive(s_storage_queue, &record, pdMS_TO_TICKS(1000))) {
            struct tm timeinfo;
            char time_str[64];
            time_t ts = record.timestamp;
            localtime_r(&ts, &timeinfo);

            // se o relógio já foi sincronizado via NTP (ano >= 2024), formata a
            // data/hora local
            if (timeinfo.tm_year >= (2024 - 1900)) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            } else {
                snprintf(time_str, sizeof(time_str), "Nao sincronizado (uptime %llds)",
                         (long long)(esp_timer_get_time() / 1000000ULL));
            }

            if (s_is_mqtt_connected) {
                char payload[256];
                snprintf(payload, sizeof(payload),
                         "{\"gpio\": %lu, \"contagem\": %lu, \"horario\": \"%s\", "
                         "\"timestamp\": %lld}",
                         (unsigned long)SENSOR_E18_PIN, (unsigned long)record.count, time_str,
                         (long long)record.timestamp);
                esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
                int msg_id = esp_mqtt_client_publish(s_mqtt_client, "sensor/e18/contagem", payload, 0, 1, 0);
                ESP_LOGI(TAG, "Mensagem MQTT enviada, id: %d", msg_id);
            } else {
                ESP_LOGI(TAG, "MQTT desconectado. Salvando evento no SPIFFS...");
                FILE *f = fopen(log_file_path, "ab");
                if (f != NULL) {
                    fwrite(&record, sizeof(sensor_data_record_t), 1, f);
                    fclose(f);
                } else {
                    ESP_LOGE(TAG, "Falha ao abrir arquivo para escrita offline");
                }
            }
        }

        // verifica se há dados offline para enviar e se o MQTT está conectado
        if (s_is_mqtt_connected) {
            struct stat st;
            if (stat(log_file_path, &st) == 0 && st.st_size > 0) {
                ESP_LOGI(TAG, "Encontrados %ld bytes de logs offline. Sincronizando...", (long)st.st_size);
                FILE *f = fopen(log_file_path, "rb");
                if (f != NULL) {
                    sensor_data_record_t off_rec;
                    while (fread(&off_rec, sizeof(sensor_data_record_t), 1, f) == 1) {
                        struct tm timeinfo;
                        char time_str[64];
                        time_t ts = off_rec.timestamp;
                        localtime_r(&ts, &timeinfo);
                        if (timeinfo.tm_year >= (2024 - 1900)) {
                            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
                        } else {
                            snprintf(time_str, sizeof(time_str), "Offline_Uptime");
                        }

                        char payload[256];
                        snprintf(payload, sizeof(payload),
                                 "{\"gpio\": %lu, \"contagem\": %lu, \"horario\": \"%s\", "
                                 "\"timestamp\": %lld, \"offline\": true}",
                                 (unsigned long)SENSOR_E18_PIN, (unsigned long)off_rec.count, time_str,
                                 (long long)off_rec.timestamp);
                        esp_mqtt5_client_set_publish_property(s_mqtt_client, &publish_property);
                        esp_mqtt_client_publish(s_mqtt_client, "sensor/e18/contagem", payload, 0, 1, 0);
                        vTaskDelay(pdMS_TO_TICKS(50)); // pequeno delay para não floodar o broker
                    }
                    fclose(f);
                    // apaga o arquivo após enviar tudo
                    unlink(log_file_path);
                    ESP_LOGI(TAG, "Sincronização offline concluída.");
                }
            }
        }
    }
}

/*
 * @brief Configura o GPIO 48 e vincula a ISR para o sensor E18-D80NK
 */
static void sensor_e18_init(void) {
    // configuracao do pino GPIO 48
    // o sensor E18-D80NK possui saida NPN em coletor aberto (ativo em nivel
    // logico BAIXO quando detecta obstaculo) configuramos com pull-up interno
    // ativado e interrupcao na borda de descida (GPIO_INTR_NEGEDGE)
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

static void wifi_reconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Conexao Wi-Fi perdida. Tentando reconectar (retry infinito)...");
        esp_wifi_connect();
    }
}

void app_main(void) {
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

    // inicia NVS
    ESP_ERROR_CHECK(nvs_manager_init());

    // instala serviço de interrupção GPIO
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    // pega o bootcount
    uint32_t boot_count;
    nvs_manager_get_boot_count(&boot_count);

    // inicia o rádio LoRa
    ESP_ERROR_CHECK(lora_receiver_init());
    // cria a tarefa para o core 0
    ESP_ERROR_CHECK(lora_receiver_start_task());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // inicializa SPIFFS para logs offline
    init_spiffs();

    // cria as filas de comunicação entre os cores
    s_sensor_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    s_storage_queue = xQueueCreate(10, sizeof(sensor_data_record_t));

    // verifica se as filas foram criadas com sucesso
    if (s_storage_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar s_storage_queue");
        return;
    }
    if (s_sensor_evt_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar s_sensor_evt_queue");
        return;
    }

    // inicializa o sensor E18-D80NK e vincula a ISR
    sensor_e18_init();

    // carrega as credenciais da rede Wi-Fi salvas na NVS
    char wifi_ssid[32];
    char wifi_pass[64];
    nvs_manager_get_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    // inicializa a infraestrutura de rede Wi-Fi
    esp_err_t wifi_err = example_connect();

    // garante que as credenciais ativas sejam as da NVS
    wifi_config_t current_wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &current_wifi_cfg) == ESP_OK) {
        if (strcmp((char *)current_wifi_cfg.sta.ssid, wifi_ssid) != 0 ||
            strcmp((char *)current_wifi_cfg.sta.password, wifi_pass) != 0) {
            ESP_LOGI(TAG, "Aplicando novas credenciais da NVS no Wi-Fi: SSID=%s", wifi_ssid);
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
        ESP_LOGI(TAG, "Wi-Fi conectado com sucesso");
    }

    // registra handler para manter o Wi-Fi sempre conectando em caso de queda
    // futura
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_reconnect_handler, NULL, NULL);

    // configura fuso horario para Horario de Brasilia (UTC-3)
    setenv("TZ", "<-03>3", 1);
    tzset();

    // inicializa o cliente SNTP (assincrono)
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("a.st1.ntp.br");
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "SNTP inicializado em background (servidor: a.st1.ntp.br, fuso: UTC-3)");

    if (wifi_err == ESP_OK && esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000)) == ESP_OK) {
        ESP_LOGI(TAG, "Horario sincronizado com sucesso via SNTP!");
    } else {
        ESP_LOGW(TAG, "Aguardando sincronizacao de horario em segundo plano...");
    }

    ESP_LOGI(TAG, "Iniciando cliente MQTT5 em background...");
    mqtt5_app_start();
    lora_receiver_test();
}
