#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief inicializa a partição NVS
 * @return ESP_OK em caso de sucesso
 */
esp_err_t central_nvs_init(void);

/**
 * @brief grava credenciais de wifi na NVS
 */
esp_err_t central_nvs_set_wifi(const char *ssid, const char *password);

/**
 * @brief lê credenciais de wifi salvas na NVS
 */
esp_err_t central_nvs_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len);

/**
 * @brief grava URL do broker MQTT na NVS
 */
esp_err_t central_nvs_set_broker(const char *broker_url);

/**
 * @brief lê URL do broker MQTT salva na NVS
 */
esp_err_t central_nvs_get_broker(char *broker_url, size_t broker_len);

#endif /* NVS_CONFIG_H */
