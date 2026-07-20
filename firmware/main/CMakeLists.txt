idf_component_register(
    SRCS "app_main.c" "display.c" "touch.c" "lvgl_port.c" "ui.c" "data.c"
         "lv_font_palm.c" "lv_font_palm_bold.c" "lv_font_kana.c" "palm_icons.c" "hotsync.c" "graffiti.c"
         "kana_data.c" "kana_strokes.c" "kana_write.c" "dash.c" "minesweeper.c" "wordie.c" "appcfg.c" "clock.c" "power.c"
    INCLUDE_DIRS "."
    REQUIRES bridge nvs_flash esp_wifi esp_netif esp_event
             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio lwip
             lvgl esp_timer esp_driver_ledc esp_pm
)

# UI_DEVTOOLS keeps the Options -> "Add test events" item (seeds 30 events to
# exercise the >24-record streaming sync on device). REMOVE this line for a
# release build so users never see dev scaffolding.
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_DEVTOOLS)
