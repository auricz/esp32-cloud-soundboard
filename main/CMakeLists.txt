idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_adc
                    EMBED_FILES "../Demoman_laughlong02.wav")
