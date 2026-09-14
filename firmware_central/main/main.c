#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_transmitter.h"
#include "nvs_flash.h"
#include "serial_bridge.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "GATEWAY_CENTRAL";

// intervalo de transmissão periódica do Beacon de Horário (30 segundos)
#define BEACON_INTERVAL_MS 30000

/**
 * @brief tarefa responsável por emitir periodicamente o Time Beacon em broadcast LoRa
 */
static void beacon_broadcast_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de Beacon periodico iniciada (intervalo: %d segundos)", BEACON_INTERVAL_MS / 1000);

    // aguarda 5 segundos iniciais para estabilização
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // se o relógio foi sincronizado pelo PC (ano >= 2024), emite o beacon com hora real
        if (timeinfo.tm_year >= (2024 - 1900)) {
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "[HEARTBEAT] Emitindo Beacon LoRa sincronizado: %s (Epoch: %lld)", time_str, (long long)now);
            lora_send_beacon((uint64_t)now);
        } else {
            ESP_LOGW(TAG, "[HEARTBEAT] Relogio ainda nao sincronizado pelo PC (uptime: %llds). Beacon nao emitido.",
                     (long long)now);
        }

        vTaskDelay(pdMS_TO_TICKS(BEACON_INTERVAL_MS));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "   EdgeBench - ESP32S3 LoRa USB Central   ");
    ESP_LOGI(TAG, "   IDF: %s | Free Heap: %" PRIu32 " bytes        ", esp_get_idf_version(), esp_get_free_heap_size());
    ESP_LOGI(TAG, "=================================================");

    // inicializa a NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // inicializa o transceptor LoRa SX1262 915 MHz, +22 dBm
    ESP_ERROR_CHECK(lora_transmitter_init());

    // inicializa a ponte de comunicação Serial USB
    ESP_ERROR_CHECK(serial_bridge_init());

    // cria a tarefa periódica de emissão de Beacons no Core 0
    xTaskCreatePinnedToCore(beacon_broadcast_task, "beacon_task", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "Gateway Mestre pronto");
}
