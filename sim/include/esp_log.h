/* esp_log.h -- simulator shim. Maps the ESP-IDF log macros used by the firmware
 * sources we compile (graffiti.c, clock.c) onto stderr. Kept ahead of
 * firmware/main in the include order so it always wins in the sim build. */
#ifndef SIM_ESP_LOG_H
#define SIM_ESP_LOG_H
#include <stdio.h>

#define SIM_LOG(lvl, tag, fmt, ...) \
    fprintf(stderr, "%s (%s) " fmt "\n", lvl, tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) SIM_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) SIM_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SIM_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif
