#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t nvs_manager_init();
esp_err_t nvs_manager_get_broker_url(char *out_url, size_t max_len);
esp_err_t nvs_manager_set_broker_url(const char *url);
esp_err_t nvs_manager_get_boot_count(uint32_t *boot_count);

#endif /* NVS_MANAGER_H */