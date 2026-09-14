#include "storage_manager.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "STORAGE_MGR";
static QueueHandle_t s_storage_queue = NULL;
static const char *s_log_file_path = "/spiffs/offline_log.bin";

/**
 * @brief inicializa e monta o sistema de arquivos SPIFFS
 */
static esp_err_t init_spiffs(void) {
    ESP_LOGI(TAG, "Inicializando sistema de arquivos SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = "storage", .max_files = 5, .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Falha ao montar ou formatar partição SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partição SPIFFS não encontrada");
        } else {
            ESP_LOGE(TAG, "Erro na inicialização do SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS montado. Total: %d bytes, Usado: %d bytes", total, used);
    }
    return ESP_OK;
}

/**
 * @brief tarefa FreeRTOS vinculada ao Core 0 dedicada a persistência e despacho
 */
static void core0_storage_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de armazenamento iniciada no Core %d", xPortGetCoreID());

    sensor_data_record_t record;

    while (1) {
        // aguarda eventos da fila com timeout de 1 segundo
        if (xQueueReceive(s_storage_queue, &record, pdMS_TO_TICKS(1000))) {
            if (mqtt_manager_is_connected()) {
                // online: envia diretamente via MQTT
                mqtt_manager_publish_detection(&record, false);
            } else {
                // offline: salva no buffer binário na Flash SPIFFS
                ESP_LOGW(TAG, "MQTT desconectado. Salvando evento offline na Flash SPIFFS...");
                FILE *f = fopen(s_log_file_path, "ab");
                if (f != NULL) {
                    fwrite(&record, sizeof(sensor_data_record_t), 1, f);
                    fclose(f);
                } else {
                    ESP_LOGE(TAG, "Falha ao abrir arquivo para escrita offline");
                }
            }
        }

        // se o MQTT estiver conectado, verifica se existem logs offline acumulados para enviar
        if (mqtt_manager_is_connected()) {
            struct stat st;
            if (stat(s_log_file_path, &st) == 0 && st.st_size > 0) {
                ESP_LOGI(TAG, "Detectados %ld bytes de logs offline na Flash. Sincronizando com o broker...",
                         (long)st.st_size);
                FILE *f = fopen(s_log_file_path, "rb");
                if (f != NULL) {
                    sensor_data_record_t off_rec;
                    while (fread(&off_rec, sizeof(sensor_data_record_t), 1, f) == 1) {
                        mqtt_manager_publish_detection(&off_rec, true);
                        vTaskDelay(pdMS_TO_TICKS(50)); // delay para não sobrecarregar a fila de rede
                    }
                    fclose(f);
                    // apaga o arquivo após o envio
                    unlink(s_log_file_path);
                    ESP_LOGI(TAG, "Sincronização de logs offline concluída com sucesso!");
                }
            }
        }
    }
}

esp_err_t storage_manager_init(QueueHandle_t *out_storage_queue) {
    // 1. Inicializa o sistema de arquivos SPIFFS
    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    // 2. Cria a fila de eventos de armazenamento
    s_storage_queue = xQueueCreate(10, sizeof(sensor_data_record_t));
    if (s_storage_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila s_storage_queue");
        return ESP_ERR_NO_MEM;
    }

    if (out_storage_queue != NULL) {
        *out_storage_queue = s_storage_queue;
    }

    // 3. Cria a tarefa de armazenamento no Core 0
    BaseType_t task_ret = xTaskCreatePinnedToCore(core0_storage_task, "core0_storage_task", 4096, NULL, 5, NULL,
                                                  0 // Core 0
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar core0_storage_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Módulo de armazenamento (SPIFFS + Task Core 0) inicializado com sucesso!");
    return ESP_OK;
}

QueueHandle_t storage_manager_get_queue(void) {
    return s_storage_queue;
}
