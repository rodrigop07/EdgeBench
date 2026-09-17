#include "button_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_receiver.h"
#include "nvs_manager.h"

// tag identificadora dos logs do gerenciador de botão
static const char *TAG = "BUTTON_MGR";

// constantes de temporização para detecção dos eventos do botão
#define BUTTON_POLL_MS 50           // intervalo de amostragem do pino (50ms)
#define SHORT_PRESS_MAX_MS 2000     // limite superior de duração para clique curto (< 2s)
#define PAIRING_HOLD_MS 3000        // tempo necessário segurando o botão para ativar pareamento (3s)
#define FACTORY_RESET_HOLD_MS 10000 // tempo necessário segurando o botão para reset de fábrica (10s)

/**
 * @brief Tarefa FreeRTOS que amostra ciclicamente o estado do botão físico.
 *
 * o botão PRG (GPIO 0) opera em lógica invertida
 * a tarefa mede a duração em que o botão permaneceu pressionado:
 * - se atingir 3s: aciona anúncio de pareamento via LoRa
 * - se atingir 10s: apaga as configurações da NVS (Factory Reset) e reinicia o ESP32
 * - se for solto antes de 2s (e após filtro de debounce de 50ms): trata como clique curto,
 *   solicitando credenciais de rede e horário à Central via LoRa
 */
static void button_task(void *pvParameters) {
    uint32_t press_duration_ms = 0; // acumulador do tempo contínuo de pressão do botão
    bool pairing_triggered = false; // flag para evitar disparos repetidos de pareamento no mesmo hold
    bool reset_triggered = false;   // flag para evitar execuções múltiplas do reset de fábrica

    ESP_LOGI(TAG, "Monitoramento do botão iniciado no GPIO %d (Core %d)", BUTTON_USER_GPIO, xPortGetCoreID());

    while (1) {
        // lê o nível lógico do pino
        int level = gpio_get_level(BUTTON_USER_GPIO);

        if (level == 0) {
            // botão pressionado
            press_duration_ms += BUTTON_POLL_MS;

            // feedback aos 3 segundos: dispara o modo de pareamento físico
            if (press_duration_ms >= PAIRING_HOLD_MS && !pairing_triggered && !reset_triggered) {
                pairing_triggered = true;
                ESP_LOGI(TAG, "Botão mantido por 3s: disparando ANUNCIO DE PAREAMENTO via LoRa...");
                // envia pacote LoRa informando o MAC e ID atual para cadastrar
                lora_send_announce_pairing();
            }

            // feedback aos 10 segundos: Dispara o Factory Reset
            if (press_duration_ms >= FACTORY_RESET_HOLD_MS && !reset_triggered) {
                reset_triggered = true;
                ESP_LOGW(TAG, "Botão mantido por 10s: executando FACTORY RESET na NVS!");
                // limpa todo o namespace da NVS (reseta bench_id para 1 e apaga credenciais)
                nvs_manager_factory_reset();
                ESP_LOGW(TAG, "Reiniciando ESP32 em 1 segundo...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        } else {
            // botão solto
            if (press_duration_ms > 0) {
                // se foi solto antes de 2s e após o debounce (> 50ms), processa clique curto
                if (press_duration_ms >= 50 && press_duration_ms < SHORT_PRESS_MAX_MS && !pairing_triggered &&
                    !reset_triggered) {
                    ESP_LOGI(TAG, "Clique curto detectado: solicitando configurações e horário via LoRa...");
                    // solicita WiFi/Broker
                    lora_send_req_config();
                    // pequeno intervalo entre envios para não saturar o canal de RF
                    vTaskDelay(pdMS_TO_TICKS(200));
                    // solicita sincronização de data/hora atual
                    lora_send_req_time();
                }

                // reseta variáveis de controle para o próximo ciclo de clique
                press_duration_ms = 0;
                pairing_triggered = false;
                reset_triggered = false;
            }
        }

        // aguarda até o próximo ciclo de amostragem
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_manager_init(void) {
    // configuração do GPIO 0 como entrada com resistor de pull-up interno
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_USER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar GPIO %d do botão (%s)", BUTTON_USER_GPIO, esp_err_to_name(err));
        return err;
    }

    // cria a tarefa de monitoramento no Core 0 com prioridade baixa (2)
    BaseType_t ret = xTaskCreatePinnedToCore(button_task, "button_task", 3072, NULL, 2, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa button_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Gerenciador do botão inicializado com sucesso!");
    return ESP_OK;
}
