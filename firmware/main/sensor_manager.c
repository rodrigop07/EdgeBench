#include "sensor_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs_manager.h"

static const char *TAG = "SENSOR_MGR";

static QueueHandle_t s_sensor_evt_queue = NULL;
static QueueHandle_t s_storage_queue = NULL;
static volatile int64_t s_last_sensor_interrupt_time = 0;
static volatile uint64_t s_sensor_debounce_us = SENSOR_DEBOUNCE_US;
static uint32_t s_total_detection_count = 0;

// timer não-bloqueante para desligar o LED após o pulso visual de detecção
static esp_timer_handle_t s_led_off_timer = NULL;

static void led_off_timer_callback(void *arg) {
    gpio_set_level(BOARD_LED_PIN, 0);
}

void sensor_manager_blink_led(uint32_t duration_ms) {
    gpio_set_level(BOARD_LED_PIN, 1);
    if (s_led_off_timer != NULL) {
        esp_timer_stop(s_led_off_timer);
        uint64_t duration_us = (duration_ms > 0 ? duration_ms : 80) * 1000ULL;
        esp_timer_start_once(s_led_off_timer, duration_us);
    }
}

/**
 * @brief rotina de atendimento a ISR vinculada ao GPIO 48
 * executada no contexto da ISR, alocada na IRAM
 * aplica filtro de debounce não-bloqueante baseado em temporizador de hardware
 * e envia uma notificação para a fila FreeRTOS
 */
static void IRAM_ATTR sensor_gpio_isr_handler(void *arg) {
    int64_t now = esp_timer_get_time();
    if ((now - s_last_sensor_interrupt_time) > (int64_t)s_sensor_debounce_us) {
        s_last_sensor_interrupt_time = now;
        uint32_t gpio_num = (uint32_t)arg;
        BaseType_t high_task_wakeup = pdFALSE;

        xQueueSendFromISR(s_sensor_evt_queue, &gpio_num, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

/**
 * @brief tarefa FreeRTOS vinculada ao Core 1 dedicada a processar eventos do sensor
 */
static void core1_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa do sensor iniciada no Core %d", xPortGetCoreID());

    // vincula o tratador de interrupção ao GPIO do sensor
    gpio_isr_handler_add(SENSOR_E18_PIN, sensor_gpio_isr_handler, (void *)SENSOR_E18_PIN);

    uint32_t io_num;
    while (1) {
        if (xQueueReceive(s_sensor_evt_queue, &io_num, portMAX_DELAY)) {
            s_total_detection_count++;
            time_t now = time(NULL);

            // pisca o LED onboard (GPIO 35) por 80ms para dar feedback visual da detecção de passagem
            sensor_manager_blink_led(80);

            sensor_data_record_t record = {
                .count = s_total_detection_count,
                .timestamp = now,
            };

            ESP_LOGI(TAG, "Detecção no Core 1 - Total acumulado: %lu peças", (unsigned long)s_total_detection_count);

            if (s_storage_queue != NULL) {
                xQueueSend(s_storage_queue, &record, 0);
            }
        }
    }
}

esp_err_t sensor_manager_init(QueueHandle_t storage_queue) {
    s_storage_queue = storage_queue;

    // carrega tempo de debounce salvo na NVS ou mantém padrão
    uint32_t saved_debounce_ms = 0;
    if (nvs_manager_get_debounce_ms(&saved_debounce_ms) == ESP_OK && saved_debounce_ms >= 10 && saved_debounce_ms <= 5000) {
        s_sensor_debounce_us = (uint64_t)saved_debounce_ms * 1000ULL;
        ESP_LOGI(TAG, "Tempo de debounce do sensor carregado da NVS: %lu ms (%llu us)",
                 (unsigned long)saved_debounce_ms, (unsigned long long)s_sensor_debounce_us);
    } else {
        s_sensor_debounce_us = SENSOR_DEBOUNCE_US;
        ESP_LOGI(TAG, "Tempo de debounce do sensor padrão: %llu us (300 ms)", (unsigned long long)s_sensor_debounce_us);
    }

    // cria a fila intermediária entre a ISR e a tarefa do Core 1
    s_sensor_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (s_sensor_evt_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila s_sensor_evt_queue");
        return ESP_ERR_NO_MEM;
    }

    // configuração do pino GPIO 48:
    // o sensor E18-D80NK possui saída NPN em coletor aberto (ativo em nível lógico baixo quando detecta obstáculo)
    // configura com pull-up interno ativado e interrupção na borda de descida (GPIO_INTR_NEGEDGE)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_E18_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar GPIO %d (%s)", SENSOR_E18_PIN, esp_err_to_name(err));
        return err;
    }

    // configura o LED onboard (GPIO 35) como saída para feedback visual de detecção
    gpio_config_t led_io_conf = {
        .pin_bit_mask = (1ULL << BOARD_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io_conf);
    gpio_set_level(BOARD_LED_PIN, 0);

    // cria o timer de software para desligamento do pulso visual do LED
    const esp_timer_create_args_t led_timer_args = {
        .callback = &led_off_timer_callback,
        .name = "sensor_led_timer",
    };
    esp_timer_create(&led_timer_args, &s_led_off_timer);

    // cria a tarefa dedicada no Core 1 com alta prioridade
    BaseType_t ret = xTaskCreatePinnedToCore(core1_sensor_task, "core1_sensor_task", 4096, NULL, 10, NULL, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar core1_sensor_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor E18-D80NK inicializado no GPIO %d com LED no GPIO %d (Core 1)", SENSOR_E18_PIN, BOARD_LED_PIN);
    return ESP_OK;
}

uint32_t sensor_manager_get_count(void) {
    return s_total_detection_count;
}

esp_err_t sensor_manager_set_debounce_ms(uint32_t debounce_ms) {
    if (debounce_ms < 10 || debounce_ms > 5000) {
        ESP_LOGE(TAG, "Tempo de debounce invalido: %lu ms (deve estar entre 10 e 5000 ms)", (unsigned long)debounce_ms);
        return ESP_ERR_INVALID_ARG;
    }
    s_sensor_debounce_us = (uint64_t)debounce_ms * 1000ULL;
    ESP_LOGI(TAG, "Novo tempo de debounce configurado dinamicamente: %lu ms (%llu us)",
             (unsigned long)debounce_ms, (unsigned long long)s_sensor_debounce_us);
    return ESP_OK;
}

uint32_t sensor_manager_get_debounce_ms(void) {
    return (uint32_t)(s_sensor_debounce_us / 1000ULL);
}
