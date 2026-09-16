#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <time.h>

// pino de conexão do sensor E18-D80NK
#define SENSOR_E18_PIN GPIO_NUM_48

// pino do LED onboard no Heltec ESP32-S3 LoRa V3 (ativo em nível alto)
#define BOARD_LED_PIN GPIO_NUM_35

// janela de debounce temporal não-bloqueante de 300ms
#define SENSOR_DEBOUNCE_US 300000ULL

// estrutura compacta do registro de produção (8 bytes)
typedef struct {
    uint32_t count;
    time_t timestamp;
} __attribute__((packed)) sensor_data_record_t;

/**
 * @brief inicializa o GPIO 48, o LED no GPIO 35, registra a ISR no Core 1 e inicia a tarefa de contagem
 * @param storage_queue Fila para onde os eventos de detecção serão despachados
 * @return ESP_OK em caso de sucesso
 */
esp_err_t sensor_manager_init(QueueHandle_t storage_queue);

/**
 * @brief obtém o total acumulado de peças detectadas até o momento
 * @return número total de peças
 */
uint32_t sensor_manager_get_count(void);

/**
 * @brief define dinamicamente o tempo de debounce do sensor em milissegundos
 * @param debounce_ms tempo em milissegundos (10 a 5000)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t sensor_manager_set_debounce_ms(uint32_t debounce_ms);

/**
 * @brief obtém o tempo de debounce atual do sensor em milissegundos
 * @return tempo em milissegundos
 */
uint32_t sensor_manager_get_debounce_ms(void);

/**
 * @brief pisca o LED onboard da bancada por uma duração específica em milissegundos de forma não-bloqueante
 * @param duration_ms duração do pulso em milissegundos (padrão 80ms)
 */
void sensor_manager_blink_led(uint32_t duration_ms);

#endif /* SENSOR_MANAGER_H */
