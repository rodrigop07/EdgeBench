#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include "sensor_manager.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief inicializa e inicia o cliente MQTT 5 com a URI e ID de bancada informados
 * @param broker_uri URL do broker MQTT (ex: mqtt://192.168.1.100:1883)
 * @param bench_id Identificador univoco desta bancada
 * @return ESP_OK em caso de sucesso
 */
esp_err_t mqtt_manager_start(const char *broker_uri, uint16_t bench_id);

/**
 * @brief verifica se o cliente MQTT está atualmente conectado ao broker
 * @return true se conectado, false caso contrário
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief publica um registro de peça detectada no tópico MQTT da bancada
 * @param record Ponteiro para a struct sensor_data_record_t
 * @param offline true se é um dado retroativo lido da Flash, false se é em tempo real
 * @return ESP_OK em caso de sucesso
 */
esp_err_t mqtt_manager_publish_detection(const sensor_data_record_t *record, bool offline);

#endif /* MQTT_MANAGER_H */
