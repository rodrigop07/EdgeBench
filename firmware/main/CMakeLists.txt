idf_component_register(SRCS "ota_manager.c" "button_manager.c" "wifi_manager.c" "app_main.c" "nvs_manager.c" "lora_receiver.c" "sensor_manager.c" "storage_manager.c" "mqtt_manager.c"
                    PRIV_REQUIRES mqtt nvs_flash esp_netif driver esp_timer lwip spiffs esp_wifi app_update esp_https_ota esp_http_client
                    INCLUDE_DIRS ".")
