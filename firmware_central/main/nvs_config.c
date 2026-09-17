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

    memset(ssid, 0, ssid_len);
    if (password && pass_len > 0) {
        memset(password, 0, pass_len);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t s_len = ssid_len;
    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &s_len);
    if (err != ESP_OK) {
        ssid[0] = '\0';
        nvs_close(handle);
        return err;
    }

    if (password && pass_len > 0) {
        size_t p_len = pass_len;
        esp_err_t p_err = nvs_get_str(handle, KEY_WIFI_PASS, password, &p_len);
        if (p_err != ESP_OK) {
            password[0] = '\0';
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

    memset(broker_url, 0, broker_len);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t b_len = broker_len;
    err = nvs_get_str(handle, KEY_BROKER_URL, broker_url, &b_len);
    if (err != ESP_OK) {
        broker_url[0] = '\0';
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    return ESP_OK;
}
