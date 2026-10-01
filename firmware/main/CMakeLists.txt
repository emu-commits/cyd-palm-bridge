idf_component_register(
    SRCS "app_main.c" "display.c" "touch.c" "lvgl_port.c" "ui.c" "data.c"
         "lv_font_palm.c" "lv_font_palm_bold.c" "lv_font_palm_bold_2x.c" "lv_font_kana.c" "lv_font_kana_20.c" "palm_icons.c" "hotsync.c" "graffiti.c"
         "kana_data.c" "kana_strokes.c" "kana_write.c" "minesweeper.c" "wordie.c" "sudoku.c" "zip.c" "coach.c" "guru.c" "guru_pool.c" "gurupool.c" "course.c" "srs.c" "study.c" "study_demo.c" "appcfg.c" "secretstore.c" "clock.c" "power.c"
    INCLUDE_DIRS "."
    REQUIRES bridge nvs_flash esp_wifi esp_netif esp_event
             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio lwip
             lvgl esp_timer esp_driver_ledc esp_pm esp_adc esp_app_format
)

# Developer hooks (UI_DEVTOOLS) are a menuconfig option, OFF by default
# (Kconfig.projbuild: CYD_DEVTOOLS). This line used to switch them on for every
# build, behind a comment asking for it to be removed before a release.
if(CONFIG_CYD_DEVTOOLS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_DEVTOOLS)
endif()
