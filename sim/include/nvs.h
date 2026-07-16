/* nvs.h -- simulator shim so the REAL firmware/main/clock.c compiles unchanged.
 * Every open fails, so clock_restore()/clock_checkpoint() no-op cleanly -- the
 * host/browser system clock is already correct, and the sim needs no NVS. */
#ifndef SIM_NVS_H
#define SIM_NVS_H
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h){
    (void)ns; (void)m; (void)h; return ESP_FAIL;
}
static inline esp_err_t nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *v){
    (void)h; (void)k; (void)v; return ESP_FAIL;
}
static inline esp_err_t nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v){
    (void)h; (void)k; (void)v; return ESP_FAIL;
}
static inline esp_err_t nvs_commit(nvs_handle_t h){ (void)h; return ESP_FAIL; }
static inline void      nvs_close(nvs_handle_t h){ (void)h; }

#endif
