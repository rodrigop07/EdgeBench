#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief inicializa a interface Wi-Fi no modo Station e registra manipuladores de evento
 * @param ssid Nome da rede
 * @param password Senha da rede (ou NULL/vazio para rede aberta)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t wifi_manager_init_sta(const char *ssid, const char *password);

/**
 * @brief reconfigura as credenciais de Wi-Fi e reconecta imediatamente
 * @param ssid novo nome da rede
 * @param password nova senha da rede (ou NULL/vazio para rede aberta)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t wifi_manager_reconfigure(const char *ssid, const char *password);

/**
 * @brief verifica se o Wi-Fi está conectado e com IP obtido
 * @return true se conectado, false caso contrário
 */
bool wifi_manager_is_connected(void);

#endif /* WIFI_MANAGER_H */
