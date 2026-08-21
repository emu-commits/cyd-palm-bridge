/* esp_system.h -- simulator shim so the REAL firmware/main/clock.c compiles
 * unchanged. Only esp_reset_reason() is needed: the drift meter asks it whether
 * this boot started from a state that had lost the RTC domain. A host process
 * always starts cold, so POWERON is the honest answer here. */
#ifndef SIM_ESP_SYSTEM_H
#define SIM_ESP_SYSTEM_H

typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void){ return ESP_RST_POWERON; }

#endif
