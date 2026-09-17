#include "ota_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "OTA_MGR";
static bool s_ota_in_progress = false;
static SemaphoreHandle_t s_ota_mutex = NULL;

esp_err_t ota_manager_init(void) {
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (s_ota_mutex == NULL) {
            ESP_LOGE(TAG, "Falha ao criar mutex do OTA");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

bool ota_manager_is_in_progress(void) {
    return s_ota_in_progress;
}

static void ota_task(void *pvParameters) {
    char *url = (char *)pvParameters;
    ESP_LOGI(TAG, "Iniciando processo OTA em background...");
    ESP_LOGI(TAG, "URL do binario: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar conexao OTA com o servidor (%s)", esp_err_to_name(err));
        free(url);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Conexao estabelecida, baixando e gravando imagem OTA...");
    int last_percent = -1;

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int total_len = esp_https_ota_get_image_size(https_ota_handle);
        int read_len = esp_https_ota_get_image_len_read(https_ota_handle);
        if (total_len > 0) {
            int percent = (read_len * 100) / total_len;
            if (percent != last_percent && (percent % 10 == 0 || percent == 100)) {
                last_percent = percent;
                ESP_LOGI(TAG, "Progresso OTA: %d%% (%d/%d bytes)", percent, read_len, total_len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err == ESP_OK) {
        err = esp_https_ota_finish(https_ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "=================================================");
            ESP_LOGI(TAG, "   OTA CONCLUIDO COM SUCESSO                     ");
            ESP_LOGI(TAG, "   Nova imagem validada. Reiniciando em 2s...    ");
            ESP_LOGI(TAG, "=================================================");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Falha ao finalizar e validar imagem OTA (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Falha durante o download da imagem OTA (%s)", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
    }

    free(url);
    s_ota_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start(const char *url) {
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "URL de OTA invalida");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_mutex == NULL) {
        ota_manager_init();
    }

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Nao foi possivel obter mutex do OTA");
        return ESP_ERR_TIMEOUT;
    }

    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "Uma atualizacao OTA ja esta em andamento");
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_ota_in_progress = true;
    xSemaphoreGive(s_ota_mutex);

    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        s_ota_in_progress = false;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(&ota_task, "ota_task", 8192, url_copy, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa ota_task");
        free(url_copy);
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ota_manager_validate_boot(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "Particao ativa em execucao: '%s' (offset 0x%08" PRIx32 ", tamanho 0x%08" PRIx32 ")",
                 running->label, running->address, running->size);

        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG, "Primeiro boot com nova imagem OTA detectado, confirmando integridade...");
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Nova imagem validada com sucesso, rollback automatico cancelado");
                } else {
                    ESP_LOGE(TAG, "Falha ao validar nova imagem OTA (%s)", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "Imagem atual em estado estavel, estado OTA: %d", ota_state);
            }
        }
    }
}
