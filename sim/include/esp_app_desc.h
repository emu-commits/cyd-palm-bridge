/* esp_app_desc.h -- simulator shim for ESP-IDF's app description.
 *
 * On the device, the build stamps the version into the image: ESP-IDF sets
 * PROJECT_VER from `git describe --always --tags --dirty` when the project
 * sets none (this one does not), and tools/package_firmware.py reads the same
 * string back out of the build. The simulator gets it from sim/Makefile, which
 * runs the same `git describe` and passes it in, unquoted, as
 * SIM_APP_VERSION_RAW (a describe string is only hex, digits, '-', '.', 'g' and
 * "dirty", so it survives as tokens; see the Makefile for why unquoted).
 * Header-only, like the other shims here. */
#ifndef ESP_APP_DESC_H
#define ESP_APP_DESC_H

#define SIM_APP_STR2(x) #x
#define SIM_APP_STR(x)  SIM_APP_STR2(x)
#ifdef SIM_APP_VERSION_RAW
#define SIM_APP_VERSION SIM_APP_STR(SIM_APP_VERSION_RAW)
#else
#define SIM_APP_VERSION "sim"
#endif

typedef struct {
    char version[32];
    char project_name[32];
} esp_app_desc_t;

static inline const esp_app_desc_t *esp_app_get_description(void){
    static const esp_app_desc_t d = { SIM_APP_VERSION, "cyd_palm_bridge" };
    return &d;
}

#endif
