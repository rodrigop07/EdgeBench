idf_component_register(SRCS "wifi_manager.c" "app_main.c" "nvs_manager.c" "lora_receiver.c" "sensor_manager.c" "storage_manager.c" "mqtt_manager.c"
                    PRIV_REQUIRES mqtt nvs_flash esp_netif driver esp_timer lwip spiffs esp_wifi
                    INCLUDE_DIRS ".")
