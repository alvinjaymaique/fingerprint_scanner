idf_component_register(SRCS "fingerprint.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_uart esp_driver_gpio)
