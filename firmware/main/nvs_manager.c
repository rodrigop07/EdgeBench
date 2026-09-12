/*
 * @brief
 *  Módulo responsável por gerenciar o armazenamento de dados na memória não volátil (NVS)
 */

#include "nvs_manager.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "NVS_MGR";
#define NVS_NAMESPACE "config"

esp_err_t nvs_manager_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Partição NVS corrompida ou inicializada, formatando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS inicializado com sucesso");
    } else {
        ESP_LOGE(TAG, "Falha ao inicializar NVS: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t nvs_manager_get_broker_url(char *out_url, size_t max_len) {
    if (out_url == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para leitura (%s)", esp_err_to_name(err));
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(handle, "broker_url", out_url, &required_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "URL do broker carregada na NVS: %s", out_url);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "URL do broker não encontrada na NVS, usando url padrão: %s", CONFIG_BROKER_URL);
        strncpy(out_url, CONFIG_BROKER_URL, max_len - 1);
        out_url[max_len - 1] = '\0';

        // salva a url padrão na NVS para uso futuro
        nvs_set_str(handle, "broker_url", out_url);
        nvs_commit(handle);
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Falha ao ler URL do broker da NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_set_broker_url(const char *url) {
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "URL do broker inválida");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para escrita: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, "broker_url", url);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "URL do broker salva com sucesso: %s", url);
        } else {
            ESP_LOGE(TAG, "Falha ao commitar alterações na NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Falha ao salvar URL do broker: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_boot_count(uint32_t *boot_count) {
    if (boot_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para ler boot count (%s)", esp_err_to_name(err));
        return err;
    }

    uint32_t count = 0;
    err = nvs_get_u32(handle, "boot_cnt", &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Primeira vez que a placa liga na vida
        count = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao ler 'boot_cnt' da NVS (%s)", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // incrementa para a sessão atual
    count++;
    // salva o novo valor na Flash
    err = nvs_set_u32(handle, "boot_cnt", count);
    if (err == ESP_OK) {
        nvs_commit(handle);
        *boot_count = count;
        ESP_LOGI(TAG, "Sessão de Boot atual: #%lu", (unsigned long)count);
    } else {
        ESP_LOGE(TAG, "Falha ao atualizar 'boot_cnt' na NVS (%s)", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_wifi_credentials(char *out_ssid, size_t max_ssid_len, char *out_pass, size_t max_pass_len) {
    // valida os argumento
    if (out_ssid == NULL || max_ssid_len == 0 || out_pass == NULL || max_pass_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para ler credenciais Wi-Fi (%s)", esp_err_to_name(err));
        return err;
    }

    // lê o SSID da rede
    size_t required_ssid_len = max_ssid_len;
    err = nvs_get_str(handle, "wifi_ssid", out_ssid, &required_ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "SSID não encontrado na NVS. Usando padrão: %s", CONFIG_EXAMPLE_WIFI_SSID);
        strncpy(out_ssid, CONFIG_EXAMPLE_WIFI_SSID, max_ssid_len - 1);
        out_ssid[max_ssid_len - 1] = '\0';
        nvs_set_str(handle, "wifi_ssid", out_ssid);
        nvs_commit(handle);
    }

    // lê a Senha
    size_t required_pass_len = max_pass_len;
    err = nvs_get_str(handle, "wifi_pass", out_pass, &required_pass_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Senha de Wi-Fi não encontrada na NVS. Usando padrão.");
        strncpy(out_pass, CONFIG_EXAMPLE_WIFI_PASSWORD, max_pass_len - 1);
        out_pass[max_pass_len - 1] = '\0';
        nvs_set_str(handle, "wifi_pass", out_pass);
        nvs_commit(handle);
    }

    ESP_LOGI(TAG, "Credenciais Wi-Fi carregadas da NVS. SSID: %s", out_ssid);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_manager_set_wifi_credentials(const char *ssid, const char *pass) {
    // valida os argumentos
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID inválido");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para gravar Wi-Fi (%s)", esp_err_to_name(err));
        return err;
    }

    // grava SSID da rede
    err = nvs_set_str(handle, "wifi_ssid", ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao gravar 'wifi_ssid' na NVS (%s)", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // grava Senha, permite string vazia para redes abertas
    const char *safe_pass = (pass != NULL) ? pass : "";
    err = nvs_set_str(handle, "wifi_pass", safe_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao gravar 'wifi_pass' na NVS (%s)", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // força a persistência física na Flash
    err = nvs_commit(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Novas credenciais Wi-Fi salvas com sucesso na NVS! (SSID: %s)", ssid);
    } else {
        ESP_LOGE(TAG, "Falha ao comitar credenciais Wi-Fi na NVS (%s)", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}