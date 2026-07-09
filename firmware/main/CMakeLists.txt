idf_component_register(
    SRCS "app_main.c" "display.c" "touch.c" "lvgl_port.c" "ui.c" "data.c"
         "lv_font_palm.c" "lv_font_palm_bold.c" "palm_icons.c" "hotsync.c" "graffiti.c"
         "appcfg.c"
    INCLUDE_DIRS "."
    REQUIRES bridge nvs_flash esp_wifi esp_netif esp_event
             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio lwip
             lvgl esp_timer
)
