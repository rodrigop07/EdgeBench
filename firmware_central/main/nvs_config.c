#include "nvs_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "CENTRAL_NVS";
#define NVS_NAMESPACE "central_cfg"

#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_BROKER_URL "broker_url"

#define DEFAULT_WIFI_SSID "Fabrica_IoT"
#define DEFAULT_WIFI_PASS "12345678"
#define DEFAULT_BROKER_URL "mqtt://192.168.1.100:1883"

esp_err_t central_nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t central_nvs_set_wifi(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para salvar Wi-Fi (%s)", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_PASS, (password != NULL) ? password : "");
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "Credenciais Wi-Fi salvas na NVS da Central (SSID: %s)", ssid);
    }
    nvs_close(handle);
    return err;
}

esp_err_t central_nvs_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    if (ssid == NULL || ssid_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        strncpy(ssid, DEFAULT_WIFI_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        if (password && pass_len > 0) {
            strncpy(password, DEFAULT_WIFI_PASS, pass_len - 1);
            password[pass_len - 1] = '\0';
        }
        return ESP_OK;
    }

    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        strncpy(ssid, DEFAULT_WIFI_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }

    if (password && pass_len > 0) {
        err = nvs_get_str(handle, KEY_WIFI_PASS, password, &pass_len);
        if (err != ESP_OK) {
            strncpy(password, DEFAULT_WIFI_PASS, pass_len - 1);
            password[pass_len - 1] = '\0';
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t central_nvs_set_broker(const char *broker_url) {
    if (broker_url == NULL || strlen(broker_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para salvar Broker (%s)", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_BROKER_URL, broker_url);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "URL do Broker salva na NVS da Central: %s", broker_url);
    }
    nvs_close(handle);
    return err;
}

esp_err_t central_nvs_get_broker(char *broker_url, size_t broker_len) {
    if (broker_url == NULL || broker_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        strncpy(broker_url, DEFAULT_BROKER_URL, broker_len - 1);
        broker_url[broker_len - 1] = '\0';
        return ESP_OK;
    }

    err = nvs_get_str(handle, KEY_BROKER_URL, broker_url, &broker_len);
    if (err != ESP_OK) {
        strncpy(broker_url, DEFAULT_BROKER_URL, broker_len - 1);
        broker_url[broker_len - 1] = '\0';
    }

    nvs_close(handle);
    return ESP_OK;
}
