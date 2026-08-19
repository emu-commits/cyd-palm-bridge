/* esp_timer.h -- simulator shim so the REAL firmware/main/clock.c compiles
 * unchanged. Timer creation fails, so clock_start_autosave() no-ops (the sim
 * has no NVS to checkpoint into anyway). */
#ifndef SIM_ESP_TIMER_H
#define SIM_ESP_TIMER_H
#include "nvs.h"   /* esp_err_t / ESP_OK / ESP_FAIL */

typedef void *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *arg);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out){
    (void)a; (void)out; return ESP_FAIL;
}
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us){
    (void)t; (void)period_us; return ESP_FAIL;
}

/* Monotonic microseconds since start. The drift meter needs a clock that
 * settimeofday() cannot move, so this one is real even in the sim. */
#include <sys/time.h>
static inline int64_t esp_timer_get_time(void){
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

#endif
