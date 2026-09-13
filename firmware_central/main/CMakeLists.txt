idf_component_register(SRCS "main.c" "lora_transmitter.c" "serial_bridge.c"
                    PRIV_REQUIRES driver nvs_flash esp_timer json
                    INCLUDE_DIRS ".")
