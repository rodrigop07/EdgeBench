#include "serial_bridge.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_transmitter.h"
#include "nvs_config.h"
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

static bool parse_mac_string(const char *str, uint8_t out_mac[6]) {
    if (str == NULL || strlen(str) < 17) {
        return false;
    }
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            out_mac[i] = (uint8_t)m[i];
        }
        return true;
    }
    return false;
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

            // Transmite imediatamente broadcast de horário via LoRa para todas as bancadas
            uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_err_t err = lora_send_resp_time(broadcast_mac, ts);
            if (err == ESP_OK) {
                send_json_status("ok", "time_synced_and_broadcast");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_timestamp");
        }
    } else if (strcmp(cmd, "set_broker") == 0) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring != NULL) {
            // salva na NVS da Central
            central_nvs_set_broker(url_item->valuestring);

            char ssid[33] = {0};
            char pass[65] = {0};
            central_nvs_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

            uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_err_t err = lora_send_resp_config(broadcast_mac, ssid, pass, url_item->valuestring);
            if (err == ESP_OK) {
                send_json_status("ok", "broker_saved_and_broadcast");
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
            // salva na NVS da Central
            central_nvs_set_wifi(ssid_item->valuestring, pass);

            char broker[128] = {0};
            central_nvs_get_broker(broker, sizeof(broker));

            uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_err_t err = lora_send_resp_config(broadcast_mac, ssid_item->valuestring, pass, broker);
            if (err == ESP_OK) {
                send_json_status("ok", "wifi_saved_and_broadcast");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_ssid");
        }
    } else if (strcmp(cmd, "set_bench") == 0) {
        cJSON *id_item = cJSON_GetObjectItem(root, "bench_id");
        if (!id_item) {
            id_item = cJSON_GetObjectItem(root, "new_id");
        }
        cJSON *target_id_item = cJSON_GetObjectItem(root, "target_id");
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");

        if (cJSON_IsNumber(id_item)) {
            uint16_t new_bench_id = (uint16_t)id_item->valueint;
            uint16_t target_bench_id =
                (target_id_item && cJSON_IsNumber(target_id_item)) ? (uint16_t)target_id_item->valueint : 0;
            uint8_t target_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // default: broadcast

            if (cJSON_IsString(mac_item) && mac_item->valuestring != NULL && strlen(mac_item->valuestring) >= 17) {
                if (!parse_mac_string(mac_item->valuestring, target_mac)) {
                    ESP_LOGW(TAG, "Formato de MAC invalido: %s, usando broadcast", mac_item->valuestring);
                }
            }

            esp_err_t err = lora_send_set_bench(target_mac, target_bench_id, new_bench_id);
            if (err == ESP_OK) {
                send_json_status("ok", "set_bench_transmitted");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_bench_id");
        }
    } else if (strcmp(cmd, "get_bench_info") == 0 || strcmp(cmd, "query_bench") == 0) {
        cJSON *id_item = cJSON_GetObjectItem(root, "bench_id");
        uint16_t target_id = (id_item && cJSON_IsNumber(id_item)) ? (uint16_t)id_item->valueint : 0;
        esp_err_t err = lora_send_req_bench_info(target_id);
        if (err == ESP_OK) {
            send_json_status("ok", "req_bench_info_transmitted");
        } else {
            send_json_status("error", "lora_tx_failed");
        }
    } else if (strcmp(cmd, "trigger_ota") == 0 || strcmp(cmd, "start_ota") == 0) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        cJSON *id_item = cJSON_GetObjectItem(root, "target_id");
        uint16_t target_id = (id_item && cJSON_IsNumber(id_item)) ? (uint16_t)id_item->valueint : 0;

        if (cJSON_IsString(url_item) && url_item->valuestring != NULL) {
            esp_err_t err = lora_send_cmd_ota(target_id, url_item->valuestring);
            if (err == ESP_OK) {
                send_json_status("ok", "ota_cmd_transmitted");
            } else {
                send_json_status("error", "lora_tx_failed");
            }
        } else {
            send_json_status("error", "missing_url");
        }
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

        ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
        ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0));
    }

    BaseType_t ret = xTaskCreatePinnedToCore(serial_rx_task, "serial_rx_task", 4096, NULL, 4, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar serial_rx_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Ponte Serial USB inicializada com sucesso!");
    return ESP_OK;
}
