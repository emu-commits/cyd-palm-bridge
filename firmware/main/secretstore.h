/* secretstore.h -- the device's passwords, kept OFF the SD card.
 *
 * config.ini used to hold the Wi-Fi passwords and the Apple app-specific
 * password in plain text, on a card that pops out of the device and into any
 * computer. They live in the ESP32's own flash now (NVS), and config.ini holds
 * everything else. A password typed into config.ini on a computer still works:
 * appcfg_load() moves it into this store and rewrites the file without it.
 *
 * Keys: "dav" for the account password, "w" + an 8-hex-digit hash of the SSID
 * for a Wi-Fi password -- keyed by network rather than by slot, so reordering
 * the four networks (a join promotes one) can never pair a password with the
 * wrong one.
 *
 * NOT encryption. NVS is plain flash unless flash encryption is enabled; what
 * this buys is that the secrets are not on the removable card. See SECURITY.md.
 *
 * The simulator keeps them in RAM for the life of the process, which is the
 * rule it already had: a browser never persists a password. */
#ifndef SECRETSTORE_H
#define SECRETSTORE_H
#include <stddef.h>

/* 1 and fills `out` if the key holds a value; 0 (and out = "") if not. */
int  secret_get(const char *key, char *out, size_t cap);
/* store `val`, or erase the key when val is NULL or "". 0 on success. */
int  secret_set(const char *key, const char *val);
/* forget every stored secret (before rewriting the whole set). */
void secret_clear_all(void);
/* the key for a Wi-Fi network's password */
void secret_wifi_key(const char *ssid, char out[12]);

#endif
