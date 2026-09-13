#ifndef SERIAL_BRIDGE_H
#define SERIAL_BRIDGE_H

#include "esp_err.h"

/**
 * @brief inicializa a porta UART0 e cria a tarefa de escuta de comandos JSON do PC
 * @return ESP_OK em caso de sucesso
 */
esp_err_t serial_bridge_init(void);

/**
 * @brief envia uma string JSON de resposta ou telemetria pela UART0 para o PC
 * @param json_str mensagem terminada em nulo
 */
void serial_bridge_send_response(const char *json_str);

#endif /* SERIAL_BRIDGE_H */
