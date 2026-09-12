#ifndef LORA_RECEIVER_H
#define LORA_RECEIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// define a frequência do LoRa, 915MHz
#define LORA_FREQUENCY_HZ 915000000ULL

// byte inicial para identificar pacote do EdgeBench
#define LORA_ESPECIAL_BYTE 0xEB

// tipos de comandos que podem ser executados pelo rádio
#define LORA_MSG_TIME_BEACON 0x01
#define LORA_MSG_SET_BROKER 0x02

// token de segurança para autorizar mudança de Broker pelo ar
#define LORA_SECURITY_TOKEN 0xABCD1234

// mapeamento dos pinos do SX1262 na placa Heltec ESP32-S3 LoRa
#define LORA_PIN_NSS 8
#define LORA_PIN_SCK 9
#define LORA_PIN_MOSI 10
#define LORA_PIN_MISO 11
#define LORA_PIN_RST 12
#define LORA_PIN_BUSY 13
#define LORA_PIN_DIO1 14

/**
 * @brief inicializa os pinos e o barramento SPI para o chip Semtech SX1262
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_receiver_init(void);

/**
 * @brief inicia a tarefa em background no core 0 para escuta contínua de pacotes
 * @return ESP_OK se a tarefa foi criada
 */
esp_err_t lora_receiver_start_task(void);

/**
 * @brief decodifica e processa o pacote LoRa recebido
 * @param payload ponteiro para o buffer de bytes
 * @param length quantidade de bytes recebidos
 */
void lora_process_packet(const uint8_t *payload, size_t length);

#endif /* LORA_RECEIVER_H */
