idf_component_register(SRCS "uart_async_rxtxtasks_main.c"
                    REQUIRES esp_driver_uart esp_driver_gpio fingerprint
                    INCLUDE_DIRS ".")
