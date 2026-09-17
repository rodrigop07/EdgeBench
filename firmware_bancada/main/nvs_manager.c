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

    memset(out_url, 0, max_len);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para leitura (%s)", esp_err_to_name(err));
        strncpy(out_url, CONFIG_BROKER_URL, max_len - 1);
        out_url[max_len - 1] = '\0';
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(handle, "broker_url", out_url, &required_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "URL do broker carregada na NVS: %s", out_url);
    } else {
        ESP_LOGW(TAG, "URL do broker não encontrada ou inválida na NVS (%s), usando url padrão: %s",
                 esp_err_to_name(err), CONFIG_BROKER_URL);
        strncpy(out_url, CONFIG_BROKER_URL, max_len - 1);
        out_url[max_len - 1] = '\0';

        // salva a url padrão na NVS para uso futuro
        nvs_set_str(handle, "broker_url", out_url);
        nvs_commit(handle);
        err = ESP_OK;
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
    // valida os argumentos
    if (out_ssid == NULL || max_ssid_len == 0 || out_pass == NULL || max_pass_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_ssid, 0, max_ssid_len);
    memset(out_pass, 0, max_pass_len);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nenhuma credencial Wi-Fi salva na NVS (%s)", esp_err_to_name(err));
        return err;
    }

    // lê o SSID da rede
    size_t required_ssid_len = max_ssid_len;
    err = nvs_get_str(handle, "wifi_ssid", out_ssid, &required_ssid_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSID Wi-Fi não encontrado na NVS (%s)", esp_err_to_name(err));
        out_ssid[0] = '\0';
        out_pass[0] = '\0';
        nvs_close(handle);
        return err;
    }

    // lê a Senha (opcional, suporta rede aberta caso vazia)
    size_t required_pass_len = max_pass_len;
    esp_err_t pass_err = nvs_get_str(handle, "wifi_pass", out_pass, &required_pass_len);
    if (pass_err != ESP_OK) {
        out_pass[0] = '\0';
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

#ifndef CONFIG_BENCH_ID
#define CONFIG_BENCH_ID BENCH_ID_UNCONFIGURED
#endif

esp_err_t nvs_manager_get_bench_id(uint16_t *bench_id) {
    if (bench_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // cria o handle para acessar a NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para ler bench_id (%s)", esp_err_to_name(err));
        *bench_id = BENCH_ID_UNCONFIGURED;
        return err;
    }

    uint16_t id = 0;
    err = nvs_get_u16(handle, "bench_id", &id);
    if (err != ESP_OK || id > 9999) {
        id = BENCH_ID_UNCONFIGURED;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "bench_id não encontrado na NVS, bancada inicializada como NAO CONFIGURADA (ID: %u)", id);
        } else {
            ESP_LOGW(TAG, "Valor de 'bench_id' na NVS inválido ou erro (%s, lido=%u). Resetando para NAO CONFIGURADA (ID: %u)",
                     esp_err_to_name(err), id, id);
        }
        nvs_erase_key(handle, "bench_id");
        nvs_set_u16(handle, "bench_id", id);
        nvs_commit(handle);
        err = ESP_OK;
    }

    *bench_id = id;
    if (id == BENCH_ID_UNCONFIGURED) {
        ESP_LOGW(TAG, "ID da Bancada carregado da NVS: %u [NAO CONFIGURADA - Aguardando atribuicao via Central]", id);
    } else {
        ESP_LOGI(TAG, "ID da Bancada carregado da NVS: %u", id);
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_manager_set_bench_id(uint16_t bench_id) {
    // valida o bench_id (IDs válidos de bancada DEVEM começar do 1, pois 0 é reservado para não configurado)
    if (bench_id == 0 || bench_id > 9999) {
        ESP_LOGE(TAG, "bench_id inválido: %u (números válidos devem começar do 1, faixa de 1 a 9999)", bench_id);
        return ESP_ERR_INVALID_ARG;
    }

    // cria o handle para acessar a NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para gravar bench_id (%s)", esp_err_to_name(err));
        return err;
    }

    nvs_erase_key(handle, "bench_id");
    err = nvs_set_u16(handle, "bench_id", bench_id);
    if (err == ESP_OK) {
        // escreve o valor na flash
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Novo ID da Bancada salvo na NVS: %u", bench_id);
        } else {
            ESP_LOGE(TAG, "Falha ao comitar bench_id na NVS (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Falha ao gravar bench_id na NVS (%s)", esp_err_to_name(err));
    }

    // fecha o handle da NVS
    nvs_close(handle);
    return err;
}

#define DEFAULT_DEBOUNCE_MS 300

esp_err_t nvs_manager_get_debounce_ms(uint32_t *out_debounce_ms) {
    if (out_debounce_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para ler debounce_ms (%s)", esp_err_to_name(err));
        *out_debounce_ms = DEFAULT_DEBOUNCE_MS;
        return err;
    }

    uint32_t val = 0;
    err = nvs_get_u32(handle, "debounce_ms", &val);
    if (err != ESP_OK || val < 10 || val > 5000) {
        val = DEFAULT_DEBOUNCE_MS;
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "debounce_ms não encontrado na NVS. Usando padrão: %lu ms", (unsigned long)val);
        } else {
            ESP_LOGW(TAG, "Valor de 'debounce_ms' na NVS inválido (%s, lido=%lu). Resetando para padrão: %lu ms",
                     esp_err_to_name(err), (unsigned long)val, (unsigned long)val);
        }
        nvs_set_u32(handle, "debounce_ms", val);
        nvs_commit(handle);
        err = ESP_OK;
    }

    *out_debounce_ms = val;
    ESP_LOGI(TAG, "Tempo de debounce carregado da NVS: %lu ms", (unsigned long)val);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_manager_set_debounce_ms(uint32_t debounce_ms) {
    if (debounce_ms < 10 || debounce_ms > 5000) {
        ESP_LOGE(TAG, "debounce_ms inválido: %lu (deve estar entre 10 e 5000 ms)", (unsigned long)debounce_ms);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para gravar debounce_ms (%s)", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(handle, "debounce_ms", debounce_ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Novo tempo de debounce salvo na NVS: %lu ms", (unsigned long)debounce_ms);
        } else {
            ESP_LOGE(TAG, "Falha ao comitar debounce_ms na NVS (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Falha ao gravar debounce_ms na NVS (%s)", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_factory_reset(void) {
    ESP_LOGW(TAG, "Iniciando Factory Reset, apagando namespace '%s' da NVS...", NVS_NAMESPACE);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS para Factory Reset (%s)", esp_err_to_name(err));
        return err;
    }

    // apaga todas as chaves gravadas no namespace "config" (wifi_ssid, wifi_pass, broker_url, bench_id, etc)
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        // grava bench_id = BENCH_ID_UNCONFIGURED (0)
        nvs_set_u16(handle, "bench_id", BENCH_ID_UNCONFIGURED);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Factory Reset concluido: todas as configuracoes foram apagadas e bench_id resetado para 0 (NAO CONFIGURADA)");
    } else {
        ESP_LOGE(TAG, "Falha ao apagar chaves da NVS (%s)", esp_err_to_name(err));
    }

    // libera o handle da NVS
    nvs_close(handle);
    return err;
}
