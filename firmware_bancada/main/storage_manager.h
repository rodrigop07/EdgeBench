#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sensor_manager.h"

/**
 * @brief inicializa a partição SPIFFS e dispara a tarefa de armazenamento no Core 0
 * @param out_storage_queue Ponteiro para receber o handle da fila criada
 * @return ESP_OK em caso de sucesso
 */
esp_err_t storage_manager_init(QueueHandle_t *out_storage_queue);

/**
 * @brief retorna o handle da fila de armazenamento
 * @return QueueHandle_t
 */
QueueHandle_t storage_manager_get_queue(void);

#endif /* STORAGE_MANAGER_H */
