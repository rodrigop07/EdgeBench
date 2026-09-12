#include "sensor_manager.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "SENSOR_MGR";

static QueueHandle_t s_sensor_evt_queue = NULL;
static QueueHandle_t s_storage_queue = NULL;
static volatile int64_t s_last_sensor_interrupt_time = 0;
static uint32_t s_total_detection_count = 0;

/**
 * @brief rotina de atendimento a ISR vinculada ao GPIO 48
 * executada no contexto da ISR, alocada na IRAM
 * aplica filtro de debounce não-bloqueante baseado em temporizador de hardware
 * e envia uma notificação para a fila FreeRTOS
 */
static void IRAM_ATTR sensor_gpio_isr_handler(void *arg) {
    int64_t now = esp_timer_get_time();
    if ((now - s_last_sensor_interrupt_time) > SENSOR_DEBOUNCE_US) {
        s_last_sensor_interrupt_time = now;
        uint32_t gpio_num = (uint32_t)arg;
        BaseType_t high_task_wakeup = pdFALSE;

        // impressão segura em nível de ROM direto do contexto da ISR
        esp_rom_printf("\n[ISR] Interrupcao detectada no GPIO %lu (Sensor E18-D80NK)!\n", (unsigned long)gpio_num);

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

    // cria a tarefa dedicada no Core 1 com alta prioridade
    BaseType_t ret = xTaskCreatePinnedToCore(core1_sensor_task, "core1_sensor_task", 4096, NULL, 10, NULL, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar core1_sensor_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor E18-D80NK inicializado no GPIO %d (Core 1)", SENSOR_E18_PIN);
    return ESP_OK;
}

uint32_t sensor_manager_get_count(void) {
    return s_total_detection_count;
}
