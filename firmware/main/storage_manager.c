#include "storage_manager.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "STORAGE_MGR";
static QueueHandle_t s_storage_queue = NULL;

// caminhos dos buffers na Flash SPIFFS
static const char *s_log_file_path = "/spiffs/offline_log.bin";
static const char *s_sync_file_path = "/spiffs/offline_sync.bin";

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
        ESP_LOGI(TAG, "SPIFFS montado. Total: %d bytes, Usado: %d bytes", (int)total, (int)used);
    }
    return ESP_OK;
}

/**
 * @brief anexa com segurança um registro de peça ao arquivo de log offline
 */
static esp_err_t append_to_offline_log(const sensor_data_record_t *record) {
    FILE *f = fopen(s_log_file_path, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "Falha ao abrir arquivo para escrita offline");
        return ESP_FAIL;
    }
    fwrite(record, sizeof(sensor_data_record_t), 1, f);
    fclose(f);
    return ESP_OK;
}

/**
 * @brief tarefa FreeRTOS vinculada ao Core 0 dedicada a persistência e despacho
 * Implementa política two way ACK e garantia de contagem
 */
static void core0_storage_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarefa de armazenamento iniciada no Core %d", xPortGetCoreID());

    sensor_data_record_t record;

    while (1) {
        // processa eventos da fila com timeout de 100ms para manter o laço responsivo
        if (xQueueReceive(s_storage_queue, &record, pdMS_TO_TICKS(100))) {
            if (mqtt_manager_is_connected()) {
                // online: envia diretamente via MQTT
                mqtt_manager_publish_detection(&record, false);
            } else {
                // offline: salva no buffer binário na Flash SPIFFS
                ESP_LOGW(TAG, "MQTT desconectado. Salvando evento offline na Flash SPIFFS...");
                append_to_offline_log(&record);
            }
        }

        // se o MQTT estiver conectado, verifica se existem logs offline acumulados para sincronizar
        if (mqtt_manager_is_connected()) {
            struct stat st_sync, st_log;
            bool has_sync = (stat(s_sync_file_path, &st_sync) == 0 && st_sync.st_size > 0);
            bool has_log = (stat(s_log_file_path, &st_log) == 0 && st_log.st_size > 0);

            // Se não há sincronização em andamento, mas há log acumulado, renomeia atomicamente
            if (!has_sync && has_log) {
                if (rename(s_log_file_path, s_sync_file_path) == 0) {
                    has_sync = true;
                    st_sync = st_log;
                    ESP_LOGI(TAG, "Buffer offline preparado para sincronização (%ld bytes)", (long)st_sync.st_size);
                } else {
                    ESP_LOGE(TAG, "Falha ao renomear arquivo para sincronizacao");
                }
            }

            // Executa a sincronização segura com Two-Way ACK
            if (has_sync) {
                FILE *f_sync = fopen(s_sync_file_path, "rb");
                if (f_sync != NULL) {
                    size_t total_records = st_sync.st_size / sizeof(sensor_data_record_t);
                    size_t sent_records = 0;
                    sensor_data_record_t off_rec;
                    bool sync_interrupted = false;

                    ESP_LOGI(TAG, "Iniciando sincronizacao resiliente de %zu registros offline (Two-Way ACK)...",
                             total_records);

                    while (fread(&off_rec, sizeof(sensor_data_record_t), 1, f_sync) == 1) {
                        // Prioriza detecção em tempo real para não engarrafar a fila (RN-04)
                        sensor_data_record_t live_rec;
                        while (xQueueReceive(s_storage_queue, &live_rec, 0)) {
                            if (mqtt_manager_is_connected()) {
                                mqtt_manager_publish_detection(&live_rec, false);
                            } else {
                                append_to_offline_log(&live_rec);
                            }
                        }

                        // Se a conexão caiu, interrompe imediatamente
                        if (!mqtt_manager_is_connected()) {
                            ESP_LOGW(TAG, "Conexao perdida durante sincronizacao offline. Interrompendo...");
                            sync_interrupted = true;
                            break;
                        }

                        // Envia aguardando confirmação PUBACK do Broker (QoS 1 - Two-Way ACK)
                        esp_err_t pub_err = mqtt_manager_publish_detection_sync(&off_rec, true, pdMS_TO_TICKS(3000));
                        if (pub_err == ESP_OK) {
                            sent_records++;
                            vTaskDelay(pdMS_TO_TICKS(20)); // pequeno intervalo para alívio do canal
                        } else {
                            ESP_LOGW(TAG, "Falha ou timeout no PUBACK do registro %zu. Interrompendo sync.",
                                     sent_records + 1);
                            sync_interrupted = true;
                            break;
                        }
                    }

                    if (!sync_interrupted && sent_records == total_records) {
                        fclose(f_sync);
                        unlink(s_sync_file_path);
                        ESP_LOGI(
                            TAG,
                            "Sincronizacao de logs offline concluida com sucesso! (%zu/%zu enviados e confirmados)",
                            sent_records, total_records);
                    } else {
                        // Se interrompido, preserva os registros não enviados de volta no offline_log.bin
                        ESP_LOGW(TAG,
                                 "Sincronizacao interrompida (%zu/%zu enviados). Preservando registros restantes...",
                                 sent_records, total_records);

                        FILE *f_log = fopen(s_log_file_path, "ab");
                        if (f_log != NULL) {
                            // se houve falha no envio do off_rec atual, grava de volta
                            if (sent_records < total_records) {
                                fwrite(&off_rec, sizeof(sensor_data_record_t), 1, f_log);
                            }
                            // lê e devolve todos os demais registros não enviados
                            while (fread(&off_rec, sizeof(sensor_data_record_t), 1, f_sync) == 1) {
                                fwrite(&off_rec, sizeof(sensor_data_record_t), 1, f_log);
                            }
                            fclose(f_log);
                            ESP_LOGI(TAG, "Registros restantes salvos de volta com sucesso no buffer offline!");
                        } else {
                            ESP_LOGE(TAG, "Erro critico ao reabrir offline_log.bin para devolucao de registros");
                        }
                        fclose(f_sync);
                        unlink(s_sync_file_path);
                    }
                }
            }
        }
    }
}

esp_err_t storage_manager_init(QueueHandle_t *out_storage_queue) {
    // inicializa o sistema de arquivos SPIFFS
    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    // cria a fila de eventos de armazenamento com capacidade para 50 detecções
    s_storage_queue = xQueueCreate(50, sizeof(sensor_data_record_t));
    if (s_storage_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila s_storage_queue");
        return ESP_ERR_NO_MEM;
    }

    if (out_storage_queue != NULL) {
        *out_storage_queue = s_storage_queue;
    }

    // cria a tarefa de armazenamento no Core 0
    BaseType_t task_ret = xTaskCreatePinnedToCore(core0_storage_task, "core0_storage_task", 4096, NULL, 5, NULL,
                                                  0 // Core 0
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar core0_storage_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Módulo de armazenamento local inicializado");
    return ESP_OK;
}

QueueHandle_t storage_manager_get_queue(void) {
    return s_storage_queue;
}
