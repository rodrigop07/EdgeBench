#ifndef LORA_TRANSMITTER_H
#define LORA_TRANSMITTER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// frequência de operação do rádio LoRa 915 MHz
#define LORA_FREQUENCY_HZ 915000000ULL

// byte especial que identifica o pacote enviado via LoRa
#define LORA_ESPECIAL_BYTE 0xEB

// tipos de mensagens e comandos via rádio
#define LORA_MSG_TIME_BEACON 0x01 // sincronização de horário absoluto
#define LORA_MSG_SET_BROKER 0x02  // atualização da URL do Broker MQTT
#define LORA_MSG_SET_WIFI 0x03    // atualização de SSID e Senha Wi-Fi
#define LORA_MSG_SET_BENCH 0x04   // atualização do ID da bancada

// token de segurança para autorizar comandos enviados via LoRa
#define LORA_SECURITY_TOKEN 0xABCD1234

// mapeamento de pinos do chip SX1262 no ESP32-S3 LoRa V3
#define LORA_PIN_NSS 8
#define LORA_PIN_SCK 9
#define LORA_PIN_MOSI 10
#define LORA_PIN_MISO 11
#define LORA_PIN_RST 12
#define LORA_PIN_BUSY 13
#define LORA_PIN_DIO1 14

/**
 * @brief inicializa o barramento SPI e configura o transceptor SX1262 para transmissão
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_transmitter_init(void);

/**
 * @brief transmite um pacote de dados arbitrário via LoRa 915 MHz
 * @param payload Buffer de bytes a ser transmitido
 * @param length Quantidade de bytes
 * @return ESP_OK se o pacote foi transmitido com sucesso
 */
esp_err_t lora_send_packet(const uint8_t *payload, size_t length);

/**
 * @brief emite um pacote Time Beacon (0x01) com timestamp Unix Epoch
 * @param timestamp Segundos desde 01/01/1970
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_send_beacon(uint64_t timestamp);

/**
 * @brief emite o comando de reconfiguração de URL do Broker MQTT (0x02) com token de segurança
 * @param broker_url String com a URL (ex: mqtt://192.168.1.100:1883)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_send_set_broker(const char *broker_url);

/**
 * @brief emite o comando de reconfiguração de rede Wi-Fi (0x03) com token de segurança
 * @param ssid Nome da rede Wi-Fi
 * @param password Senha da rede Wi-Fi
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_send_set_wifi(const char *ssid, const char *password);

/**
 * @brief emite o comando de reatribuição de ID de Bancada (0x04) com token de segurança
 * @param bench_id Novo ID da bancada (1 a 65535)
 * @return ESP_OK em caso de sucesso
 */
esp_err_t lora_send_set_bench(uint16_t bench_id);

#endif /* LORA_TRANSMITTER_H */
