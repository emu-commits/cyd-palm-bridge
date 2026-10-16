/* monster_install.c -- the web flasher's optional Critters install
 * (docs/GAME_PLAN.md §13 Q1).
 *
 * The flasher's "with Critters" manifest (tools/package_firmware.py) also
 * writes courses/critters/game.bin into the "game" data partition
 * (partitions.csv). At boot, once the card is mounted, a game there that the
 * card hasn't had yet is copied to /sdcard/games/monster/game.bin. "Yet" is
 * the file's fingerprint (its section table's CRC), kept in installed.txt
 * beside it: so a game.bin copied onto the card by hand is replaced only by a
 * newer flash, never at every boot, and a flash without Critters leaves the
 * card alone.
 *
 * The copy goes through safefile.h, so a power cut leaves the old file or the
 * new one, never half of one. The reader checks everything again on open. */
#include "monster_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "safefile.h"
#include "monster.h"
#include "ui_monster.h"

#define GAME_SUBTYPE 0x40
#define MARK MON_DIR "/installed.txt"
#define CHUNK 4096

static const char *TAG = "critters";

static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void monster_install(void){
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, GAME_SUBTYPE, "game");
    uint8_t h[32];
    if(!part || esp_partition_read(part, 0, h, sizeof h) != ESP_OK) return;
    if(memcmp(h, "CYDG", 4)) return;                     /* flashed without Critters: erased or old */
    uint32_t total = le32(h + 8), fp = le32(h + 24);
    if(total < sizeof h || total > part->size){
        ESP_LOGW(TAG, "the game partition holds %u B, more than it can: not installed", (unsigned)total);
        return;
    }
    char mark[16] = "";
    FILE *f = fopen(MARK, "r");
    if(f){ if(!fgets(mark, sizeof mark, f)) mark[0] = 0; fclose(f); }
    char want[16];
    snprintf(want, sizeof want, "%08x\n", (unsigned)fp);
    if(!strcmp(mark, want)) return;                      /* this one is on the card already */

    mkdir("/sdcard/games", 0777);
    mkdir(MON_DIR, 0777);
    uint8_t *buf = malloc(CHUNK);
    if(!buf) return;
    SafeFile sf;
    int ok = sf_open(&sf, MON_DIR "/game.bin", "wb") != NULL;
    for(uint32_t off = 0; ok && off < total; off += CHUNK){
        uint32_t n = total - off < CHUNK ? total - off : CHUNK;
        ok = esp_partition_read(part, off, buf, n) == ESP_OK && fwrite(buf, 1, n, sf.f) == n;
    }
    free(buf);
    if(sf_commit(&sf, ok)){
        ESP_LOGW(TAG, "couldn't copy the game to the card");
        return;
    }
    MonGame *g = malloc(sizeof *g);
    if(g){
        if(mg_open(g, MON_DIR "/game.bin")) ESP_LOGW(TAG, "the installed game doesn't open: %s", g->err);
        else { ESP_LOGI(TAG, "installed %s (%u B)", g->title, (unsigned)total); mg_close(g); }
        free(g);
    }
    f = fopen(MARK, "w");
    if(f){ fputs(want, f); fclose(f); }
}
