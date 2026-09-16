#ifndef LORA_TRANSMITTER_H
#define LORA_TRANSMITTER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// frequência de operação do rádio LoRa 915 MHz
#define LORA_FREQUENCY_HZ 915000000ULL

// byte identificador do EdgeBench
#define LORA_ESPECIAL_BYTE 0xEB

// opcodes dos comandos
#define LORA_MSG_REQ_TIME 0x10        // Nó -> Central: solicita timestamp
#define LORA_MSG_RESP_TIME 0x11       // Central -> Nó: responde timestamp
#define LORA_MSG_REQ_CONFIG 0x20      // Nó -> Central: solicita wifi e Broker
#define LORA_MSG_RESP_CONFIG 0x21     // Central -> Nó: responde credenciais
#define LORA_MSG_CMD_SET_BENCH 0x30   // Central -> Nó: configura ID direcionado por ID e/ou MAC
#define LORA_MSG_REQ_BENCH_INFO 0x31  // Central -> Nó: consulta MAC de bancada com ID x
#define LORA_MSG_RESP_BENCH_INFO 0x32 // Nó -> Central: informa seu ID e MAC
#define LORA_MSG_ANNOUNCE_PAIRING 0x33 // Nó -> Central: anúncio de presença para pareamento físico
#define LORA_MSG_CMD_OTA 0x40          // Central -> Nó: comanda início de atualização OTA
#define LORA_MSG_TELEMETRY 0x50        // Nó -> Central: fallback de telemetria (offline)
#define LORA_MSG_CMD_SET_DEBOUNCE 0x60 // Central -> Nó: configura tempo de debounce em ms
#define LORA_MSG_CMD_PING 0x70         // Central -> Broadcast: ping para todos os nós
#define LORA_MSG_RESP_PONG 0x71        // Nó -> Central: resposta pong contendo MAC e ID de bancada

// token de segurança para comandos críticos
#define LORA_SECURITY_TOKEN 0xABCD1234

// mapeamento de pinos do chip SX1262
#define LORA_PIN_NSS 8
#define LORA_PIN_SCK 9
#define LORA_PIN_MOSI 10
#define LORA_PIN_MISO 11
#define LORA_PIN_RST 12
#define LORA_PIN_BUSY 13
#define LORA_PIN_DIO1 14
#define LORA_PIN_VEXT 36

/**
 * @brief inicializa o transceptor SX1262 para transmissão e recepção
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_transmitter_init(void);

/**
 * @brief inicia a tarefa de escuta contínua
 * @return ESP_OK se a tarefa foi criada
 */
esp_err_t lora_start_rx_task(void);

/**
 * @brief transmite pacote arbitrário via LoRa e retorna para modo de escuta
 */
esp_err_t lora_send_packet(const uint8_t *payload, size_t length);

/**
 * @brief envia resposta de horário (0x11) direcionada ao MAC da bancada ou broadcast
 */
esp_err_t lora_send_resp_time(const uint8_t target_mac[6], uint64_t timestamp);

/**
 * @brief envia resposta de configuração (0x21) direcionada ao MAC da bancada
 */
esp_err_t lora_send_resp_config(const uint8_t target_mac[6], const char *ssid, const char *password,
                                const char *broker_url);

/**
 * @brief envia comando de configuração de ID da bancada direcionado por ID atual e/ou MAC
 */
esp_err_t lora_send_set_bench(const uint8_t target_mac[6], uint16_t target_bench_id, uint16_t new_bench_id);

/**
 * @brief envia solicitação de consulta de MAC para a bancada indicada
 */
esp_err_t lora_send_req_bench_info(uint16_t target_bench_id);

/**
 * @brief envia comando de atualização OTA direcionado por ID (ou 0 para todas)
 */
esp_err_t lora_send_cmd_ota(uint16_t target_bench_id, const char *url);

/**
 * @brief envia comando para reconfigurar o tempo de debounce do sensor via LoRa
 */
esp_err_t lora_send_cmd_set_debounce(const uint8_t target_mac[6], uint16_t target_bench_id, uint32_t debounce_ms);

/**
 * @brief envia comando de ping em broadcast para descobrir todas as bancadas no alcance LoRa
 */
esp_err_t lora_send_ping_broadcast(void);

#endif /* LORA_TRANSMITTER_H */
