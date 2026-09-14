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

/**
 * @brief publica um registro de peça aguardando confirmação PUBACK do broker
 * @param record ponteiro para a struct sensor_data_record_t
 * @param offline true se é um dado lido da Flash, false se é em tempo real
 * @param timeout_ticks tempo máximo de espera pela confirmação PUBACK
 * @return ESP_OK se confirmado com sucesso pelo broker, erro caso contrário
 */
esp_err_t mqtt_manager_publish_detection_sync(const sensor_data_record_t *record, bool offline,
                                              TickType_t timeout_ticks);

/**
 * @brief reconfigura a URI do broker MQTT sem reiniciar o esp32
 * @param broker_uri Nova URL do broker MQTT (ex: mqtt://192.168.1.100:1883)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t mqtt_manager_set_broker(const char *broker_uri);

/**
 * @brief reconfigura o ID da bancada e atualiza os tópicos MQTT
 * @param bench_id Novo identificador numérico da bancada
 * @return ESP_OK em caso de sucesso
 */
esp_err_t mqtt_manager_set_bench_id(uint16_t bench_id);

#endif /* MQTT_MANAGER_H */
