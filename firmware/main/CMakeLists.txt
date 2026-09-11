idf_component_register(SRCS "app_main.c"
                    PRIV_REQUIRES mqtt nvs_flash esp_netif driver esp_timer lwip
                    INCLUDE_DIRS ".")
