#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include "esp_err.h"

// pino do botão físico PRG no Heltec ESP32-S3 LoRa V3
#define BUTTON_USER_GPIO 0

/**
 * @brief inicializa o monitoramento do botão físico no GPIO 0
 *
 * configura o pino com pull-up interno e cria uma tarefa FreeRTOS em segundo plano
 * para detectar três tipos de ações:
 * 1. clique curto (< 2s): solicita configurações de wifi/broker e horário via LoRa
 * 2. pressionar por 3s: transmite anúncio de pareamento físico para a Central
 * 3. pressionar por 10s: executa Factory Reset (apaga NVS) e reinicia o ESP32
 *
 * @return ESP_OK se configurado e a tarefa for criada com sucesso, ou código de erro
 */
esp_err_t button_manager_init(void);

#endif /* BUTTON_MANAGER_H */
