idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_adc esp_event esp_http_client
                             esp_netif esp_wifi mbedtls nvs_flash)
