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

/**
 * @brief recupera o SSID e a Senha do Wi-Fi da NVS, ou grava do kconfig se não existir
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_get_wifi_credentials(char *out_ssid, size_t max_ssid_len, char *out_pass, size_t max_pass_len);

/**
 * @brief grava novo SSID e Senha de Wi-Fi na NVS
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_set_wifi_credentials(const char *ssid, const char *pass);

/**
 * @brief recupera o identificador da bancada (bench_id) da NVS
 * @param bench_id ponteiro para armazenar o ID da bancada
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_get_bench_id(uint16_t *bench_id);

/**
 * @brief salva o identificador da bancada (bench_id) na NVS
 * @param bench_id novo ID da bancada
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_set_bench_id(uint16_t bench_id);

/**
 * @brief recupera o tempo de debounce do sensor em milissegundos da NVS
 * @param out_debounce_ms ponteiro para armazenar o valor
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_get_debounce_ms(uint32_t *out_debounce_ms);

/**
 * @brief salva o tempo de debounce do sensor em milissegundos na NVS
 * @param debounce_ms tempo em milissegundos (entre 10 e 5000)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_set_debounce_ms(uint32_t debounce_ms);

/**
 * @brief apaga todas as configurações salvas na NVS
 * @return ESP_OK em caso de sucesso
 */
esp_err_t nvs_manager_factory_reset(void);

#endif /* NVS_MANAGER_H */