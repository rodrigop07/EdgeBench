#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_transmitter.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "serial_bridge.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "GATEWAY_CENTRAL";

void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "   EdgeBench - ESP32S3 LoRa USB Central          ");
    ESP_LOGI(TAG, "   IDF: %s | Free Heap: %" PRIu32 " bytes        ", esp_get_idf_version(), esp_get_free_heap_size());
    ESP_LOGI(TAG, "=================================================");

    // configura fuso horário para GMT-3
    setenv("TZ", "<-03>3", 1);
    tzset();

    // inicializa a NVS
    ESP_ERROR_CHECK(central_nvs_init());

    // instala serviço de interrupções GPIO para o pino DIO1 do SX1262
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }

    // inicializa o transceptor LoRa SX1262 915 MHz (TX + RX)
    ESP_ERROR_CHECK(lora_transmitter_init());

    // inicia a tarefa de escuta contínua de requisições das bancadas
    ESP_ERROR_CHECK(lora_start_rx_task());

    // inicializa a ponte de comunicação Serial USB
    ESP_ERROR_CHECK(serial_bridge_init());

    ESP_LOGI(TAG, "Gateway Central pronto em modo servidor sob demanda (RX padrao, TX em respostas)");

    // solicita sincronizacao inicial de horario ao PC conectado
    printf("{\"event\":\"req_time\"}\n");
    fflush(stdout);
}
