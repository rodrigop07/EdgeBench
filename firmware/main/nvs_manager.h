#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief inicializa o NVS
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_init();

/**
 * @brief recupera a URL do broker
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_get_broker_url(char *out_url, size_t max_len);

/**
 * @brief salva a URL do broker
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_set_broker_url(const char *url);

/**
 * @brief recupera o contador de boot
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_get_boot_count(uint32_t *boot_count);

#endif /* NVS_MANAGER_H */