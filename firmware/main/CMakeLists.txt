idf_component_register(
    SRCS "app_main.c" "display.c" "touch.c"
    INCLUDE_DIRS "."
    REQUIRES bridge nvs_flash esp_wifi esp_netif esp_event
             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio lwip
)
