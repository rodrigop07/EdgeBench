#ifndef LORA_RECEIVER_H
#define LORA_RECEIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// frequência de operação do rádio LoRa 915 MHz
#define LORA_FREQUENCY_HZ 915000000ULL

// byte inicial para identificar pacote do EdgeBench
#define LORA_ESPECIAL_BYTE 0xEB

// opcodes dos comandos
#define LORA_MSG_REQ_TIME 0x10         // Nó -> Central: solicita timestamp
#define LORA_MSG_RESP_TIME 0x11        // Central -> Nó: responde timestamp
#define LORA_MSG_REQ_CONFIG 0x20       // Nó -> Central: solicita wifi e Broker
#define LORA_MSG_RESP_CONFIG 0x21      // Central -> Nó: responde credenciais
#define LORA_MSG_CMD_SET_BENCH 0x30    // Central -> Nó: configura ID direcionado por ID e/ou MAC
#define LORA_MSG_REQ_BENCH_INFO 0x31   // Central -> Nó: consulta MAC de bancada com ID x
#define LORA_MSG_RESP_BENCH_INFO 0x32  // Nó -> Central: informa seu ID e MAC
#define LORA_MSG_ANNOUNCE_PAIRING 0x33 // Nó -> Central: anuncia presença para pareamento
#define LORA_MSG_CMD_OTA 0x40          // Central -> Nó: comanda início de atualização OTA

// token de segurança para autorizar comandos críticos
#define LORA_SECURITY_TOKEN 0xABCD1234

// mapeamento dos pinos do SX1262 na placa Heltec ESP32-S3 LoRa
#define LORA_PIN_NSS 8
#define LORA_PIN_SCK 9
#define LORA_PIN_MOSI 10
#define LORA_PIN_MISO 11
#define LORA_PIN_RST 12
#define LORA_PIN_BUSY 13
#define LORA_PIN_DIO1 14
#define LORA_PIN_VEXT 36

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
 * @brief transmite pacote arbitrário via LoRa e retorna ao modo de escuta (RX)
 */
esp_err_t lora_send_packet(const uint8_t *payload, size_t length);

/**
 * @brief envia requisição de horário (0x10) o MAC do nó
 */
esp_err_t lora_send_req_time(void);

/**
 * @brief envia requisição de configurações de wifi e broker (0x20)
 */
esp_err_t lora_send_req_config(void);

/**
 * @brief emite pacote de anúncio de pareamento físico (0x33) contendo o MAC e ID atual
 */
esp_err_t lora_send_announce_pairing(void);

/**
 * @brief decodifica e processa o pacote LoRa recebido
 * @param payload ponteiro para o buffer de bytes
 * @param length quantidade de bytes recebidos
 */
void lora_process_packet(const uint8_t *payload, size_t length);

#endif /* LORA_RECEIVER_H */
