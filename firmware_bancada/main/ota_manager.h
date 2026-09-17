#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief inicializa o módulo OTA e seus recursos de sincronização
 * @return ESP_OK em caso de sucesso
 */
esp_err_t ota_manager_init(void);

/**
 * @brief inicia o processo de atualização OTA a partir de uma URL HTTP/HTTPS em segundo plano
 * @param url URL completa do arquivo binário (ex: "http://192.168.1.50:8080/firmware_bancada.bin")
 * @return ESP_OK se a tarefa de atualização foi disparada com sucesso
 *         ESP_ERR_INVALID_ARG se a URL for nula ou vazia
 *         ESP_ERR_INVALID_STATE se já houver uma atualização OTA em andamento
 */
esp_err_t ota_manager_start(const char *url);

/**
 * @brief verifica se há um processo de atualização OTA em execução
 * @return true se o OTA estiver em andamento, false caso contrário
 */
bool ota_manager_is_in_progress(void);

/**
 * @brief valida o boot atual da imagem OTA, cancelando o rollback automático
 * @note deve ser chamada no app_main após a inicialização bem-sucedida dos serviços essenciais.
 */
void ota_manager_validate_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANAGER_H */
