#include "serial_bridge.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_transmitter.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "SERIAL_BRIDGE";
#define UART_PORT UART_NUM_0
#define BUF_SIZE 1024

void serial_bridge_send_response(const char *json_str) {
    if (json_str != NULL) {
        printf("%s\n", json_str);
        fflush(stdout);
    }
}

static void send_json_status(const char *status, const char *msg) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", status);
    if (msg != NULL) {
        cJSON_AddStringToObject(resp, "msg", msg);
    }
    cJSON_AddNumberToObject(resp, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));

    char *rendered = cJSON_PrintUnformatted(resp);
    if (rendered != NULL) {
        serial_bridge_send_response(rendered);
        free(rendered);
    }
    cJSON_Delete(resp);
}

static void process_json_command(const char *line) {
    if (line == NULL || strlen(line) == 0) {
        return;
    }

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON invalido recebido pela Serial: %s", line);
        send_json_status("error", "invalid_json");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL) {
        ESP_LOGW(TAG, "Campo 'cmd' ausente ou invalido no JSON");
        send_json_status("error", "missing_cmd");
        cJSON_Delete(root);
        return;
    }

    const char *cmd = cmd_item->valuestring;
    ESP_LOGI(TAG, "Comando serial recebido: '%s'", cmd);

    if (strcmp(cmd, "sync_time") == 0) {
        cJSON *ts_item = cJSON_GetObjectItem(root, "timestamp");
        if (cJSON_IsNumber(ts_item)) {
            uint64_t ts = (uint64_t)ts_item->valuedouble;
            struct timeval tv = {.tv_sec = (time_t)ts, .tv_usec = 0};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Horario do ESP32 Central ajustado para epoch %llu", (unsigned long long)ts);

            // dispara beacon LoRa imediato
            lora_send_beacon(ts);
            send_json_status("ok", "time_synced_and_beacon_broadcast");
        } else {
            send_json_status("error", "missing_timestamp");
        }
    } else if (strcmp(cmd, "set_broker") == 0) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring != NULL) {
            esp_err_t err = lora_send_set_broker(url_item->valuestring);
            if (err == ESP_OK) {
                send_json_status("ok", "set_broker_transmitted");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_url");
        }
    } else if (strcmp(cmd, "set_wifi") == 0) {
        cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
        cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(ssid_item) && ssid_item->valuestring != NULL) {
            const char *pass =
                (cJSON_IsString(pass_item) && pass_item->valuestring != NULL) ? pass_item->valuestring : "";
            esp_err_t err = lora_send_set_wifi(ssid_item->valuestring, pass);
            if (err == ESP_OK) {
                send_json_status("ok", "set_wifi_transmitted");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_ssid");
        }
    } else if (strcmp(cmd, "set_bench") == 0) {
        cJSON *id_item = cJSON_GetObjectItem(root, "bench_id");
        if (cJSON_IsNumber(id_item)) {
            uint16_t bench_id = (uint16_t)id_item->valueint;
            esp_err_t err = lora_send_set_bench(bench_id);
            if (err == ESP_OK) {
                send_json_status("ok", "set_bench_transmitted");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_bench_id");
        }
    } else if (strcmp(cmd, "beacon_now") == 0) {
        time_t now = time(NULL);
        lora_send_beacon((uint64_t)now);
        send_json_status("ok", "beacon_transmitted");
    } else if (strcmp(cmd, "ping") == 0) {
        send_json_status("pong", "gateway_online");
    } else {
        ESP_LOGW(TAG, "Comando desconhecido: %s", cmd);
        send_json_status("error", "unknown_cmd");
    }

    cJSON_Delete(root);
}

static void serial_rx_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa serial_bridge iniciada na UART0 (115200 bps)");

    uint8_t line_buffer[BUF_SIZE];
    size_t line_idx = 0;
    uint8_t ch;

    while (1) {
        int rx_bytes = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (rx_bytes > 0) {
            if (ch == '\n' || ch == '\r') {
                if (line_idx > 0) {
                    line_buffer[line_idx] = '\0';
                    process_json_command((char *)line_buffer);
                    line_idx = 0;
                }
            } else {
                if (line_idx < (BUF_SIZE - 1)) {
                    line_buffer[line_idx++] = ch;
                } else {
                    // buffer terminou sem quebra de linha, descarta
                    line_idx = 0;
                }
            }
        }
    }
}

esp_err_t serial_bridge_init(void) {
    if (!uart_is_driver_installed(UART_PORT)) {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        esp_err_t err = uart_param_config(UART_PORT, &uart_config);
        if (err != ESP_OK) {
            return err;
        }

        err = uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    BaseType_t ret = xTaskCreatePinnedToCore(serial_rx_task, "serial_rx_task", 4096, NULL, 5, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa serial_rx_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Ponte Serial USB inicializada com sucesso!");
    return ESP_OK;
}
