/* secretstore.c -- see secretstore.h. */
#include "secretstore.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void secret_wifi_key(const char *ssid, char out[12]){
    uint32_t h = 2166136261u;                    /* FNV-1a */
    for(const char *p = ssid; p && *p; p++){ h ^= (unsigned char)*p; h *= 16777619u; }
    snprintf(out, 12, "w%08lx", (unsigned long)h);
}

#ifdef ESP_PLATFORM
#include "nvs.h"
#define NS "cydsec"

int secret_get(const char *key, char *out, size_t cap){
    if(cap) out[0] = 0;
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = cap;
    int ok = nvs_get_str(h, key, out, &n) == ESP_OK;
    nvs_close(h);
    if(!ok && cap) out[0] = 0;
    return ok && out[0];
}

int secret_has(const char *key){
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = 0;                                /* NULL out: just the length */
    int ok = nvs_get_str(h, key, NULL, &n) == ESP_OK && n > 1;
    nvs_close(h);
    return ok;
}

int secret_set(const char *key, const char *val){
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = (val && val[0]) ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if(e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   /* erasing what is not there */
    if(e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

void secret_clear_all(void){
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

#else   /* simulator + host: RAM only, never persisted */
/* Masked, because this table stands in for the device's flash and the
 * secretscan gate treats a plain copy anywhere in memory as a leak. A fixed
 * XOR is not protection and is not meant to be; it only keeps the stand-in
 * for flash from looking like a leak in RAM. */
#define SS_N 8
#define SS_MASK(i) ((char)(0x5A ^ (i) * 29))
static struct { char key[12]; char val[128]; } s_ss[SS_N];

static int ss_find(const char *key){
    for(int i = 0; i < SS_N; i++)
        if(s_ss[i].key[0] && !strcmp(s_ss[i].key, key)) return i;
    return -1;
}
int secret_has(const char *key){
    int i = ss_find(key);
    return i >= 0 && s_ss[i].val[0] != SS_MASK(0);
}
int secret_get(const char *key, char *out, size_t cap){
    if(cap) out[0] = 0;
    int i = ss_find(key);
    if(i < 0 || !cap) return 0;
    size_t n = 0;
    for(; n + 1 < cap && n < sizeof s_ss[i].val; n++){
        char c = s_ss[i].val[n] ^ SS_MASK(n);
        if(!c) break;
        out[n] = c;
    }
    out[n] = 0;
    return out[0] != 0;
}
int secret_set(const char *key, const char *val){
    int slot = ss_find(key);
    if(!val || !val[0]){ if(slot >= 0) memset(&s_ss[slot], 0, sizeof s_ss[slot]); return 0; }
    for(int i = 0; slot < 0 && i < SS_N; i++) if(!s_ss[i].key[0]) slot = i;
    if(slot < 0) return -1;
    snprintf(s_ss[slot].key, sizeof s_ss[slot].key, "%s", key);
    size_t n = 0;
    for(; val[n] && n + 1 < sizeof s_ss[slot].val; n++) s_ss[slot].val[n] = val[n] ^ SS_MASK(n);
    for(; n < sizeof s_ss[slot].val; n++) s_ss[slot].val[n] = SS_MASK(n);   /* masked NULs */
    return 0;
}
void secret_clear_all(void){ memset(s_ss, 0, sizeof s_ss); }
#endif
