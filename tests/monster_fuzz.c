/* monster_fuzz.c -- the monster game's reader and rules against damaged and
 * hostile game files, under AddressSanitizer and UBSan (make ftest).
 *
 * On the fixture (tests/data/game/game.bin):
 *
 *   1. TRUNCATION at every length: must fail on open.
 *   2. ONE FLIPPED BIT: every bit of the header, the section table and the
 *      small sections, and thousands at random in the rest: open or
 *      mg_verify() MUST fail. Every byte is under a CRC or a zero-padding rule
 *      (GAME_FORMAT.md section 6).
 *   3. HOSTILE: bytes changed and then every CRC fixed up to match (chunks,
 *      tiles, characters, area heads, sprites, sections, the table, the
 *      header), so the damage reaches the checks behind them. If the file
 *      still opens, everything is read into buffers of exactly the size the
 *      API promises, and then it's PLAYED: a new game, steps, battles with
 *      random actions, a save and a load. Nothing may read outside a buffer
 *      or trip UBSan.
 *
 * Deterministic (a fixed-seed LCG), so a failure reproduces. Run from the
 * repo root. */
#include "monster.h"
#include "monster_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_PATH "tests/data/game/game.bin"

static uint32_t seed = 4242;
static uint32_t rnd(void){ seed = seed * 1103515245u + 12345u; return seed >> 8; }

static uint32_t le32(const uint8_t *p){ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static void put32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static const char *save_path;

static int open_bytes(MonGame *g, uint8_t *b, long n){
    FILE *f = fmemopen(b, (size_t)n, "rb");
    if(!f){ perror("fmemopen"); exit(2); }
    return mg_open_fp(g, f);
}

/* ---- fixing every CRC after a change ---- */

/* the sections' places, from the table (as they now stand) */
static int sect(const uint8_t *b, long n, int k, uint32_t *off, uint32_t *len){
    static const char *T = "METATYPESHAPSTATMOVEMONSLRNSENCTAREAADATSPRTTEXTCRED";
    uint32_t ns = le16(b + 12);
    if(ns > 64 || 32 + 16 * ns > (uint32_t)n) return 0;
    for(uint32_t i = 0; i < ns; i++){
        const uint8_t *e = b + 32 + 16 * i;
        if(memcmp(e, T + 4 * k, 4)) continue;
        *off = le32(e + 4); *len = le32(e + 8);
        return *off <= (uint32_t)n && *len <= (uint32_t)n - *off;
    }
    return 0;
}

static void fix_crcs(uint8_t *b, long n){
    uint32_t aoff, alen, doff, dlen, moff, mlen, soff, slen;
    if(n < 32) return;
    if(sect(b, n, MG_AREA, &aoff, &alen) && sect(b, n, MG_ADAT, &doff, &dlen)){
        for(uint32_t i = 0; i + 32 <= alen; i += 32){
            uint8_t *r = b + aoff + i;
            uint32_t off = le32(r + 8), len = le32(r + 12), hl = le32(r + 28);
            if(off > dlen || len > dlen - off || hl < 64 || hl > len) continue;
            uint8_t *blk = b + doff + off;
            uint32_t ccols = le16(blk + 8), crows = le16(blk + 10), toff = le32(blk + 24), tlen = le32(blk + 28);
            uint32_t coff = le32(blk + 36), clen = le32(blk + 40);
            if(toff <= len && tlen <= len - toff) put32(blk + 32, mg_crc32(0, blk + toff, tlen));
            if(coff <= len && clen <= len - coff) put32(blk + 44, mg_crc32(0, blk + coff, clen));
            uint64_t nch = (uint64_t)ccols * crows;
            if(8 * nch <= hl){
                uint8_t *cdir = blk + hl - 8 * nch;
                for(uint64_t k = 0; k < nch; k++){
                    uint32_t co = le32(cdir + 8 * k);
                    if(co <= len && MG_CHUNK_BYTES <= len - co) put32(cdir + 8 * k + 4, mg_crc32(0, blk + co, MG_CHUNK_BYTES));
                }
            }
            put32(r + 16, mg_crc32(0, blk, hl));
        }
    }
    if(sect(b, n, MG_MONS, &moff, &mlen) && sect(b, n, MG_SPRT, &soff, &slen)){
        for(uint32_t i = 0; i + 48 <= mlen; i += 48){
            uint32_t so = le32(b + moff + i + 24), sl = le32(b + moff + i + 28);
            if(so <= slen && sl <= slen - so && sl >= 8) put32(b + soff + so + 4, mg_crc32(0, b + soff + so + 8, sl - 8));
        }
    }
    uint32_t ns = le16(b + 12);
    if(ns <= 64 && 32 + 16 * ns <= (uint32_t)n){
        for(uint32_t i = 0; i < ns; i++){
            uint8_t *e = b + 32 + 16 * i;
            uint32_t off = le32(e + 4), len = le32(e + 8);
            if(off <= (uint32_t)n && len <= (uint32_t)n - off) put32(e + 12, mg_crc32(0, b + off, len));
        }
        put32(b + 24, mg_crc32(0, b + 32, 16 * ns));
    }
    put32(b + 28, mg_crc32(0, b, 28));
}

/* ---- reading and playing everything ---- */

static long walked;

static void play(MonGame *g){
    MpWorld *w = calloc(1, sizeof *w);
    uint8_t *head = malloc(MG_HEAD_MAX);
    MpLog log;
    MpBattle b;
    for(uint16_t starter = 0; starter < g->n_mons && starter < 3; starter++){
        if(mp_new(w, g, head, rnd() | 1, starter)) continue;
        for(int i = 0; i < 300; i++){
            mp_step(w, (int)(rnd() % 4), &b, &log);
            if(b.over && rnd() % 8 == 0) mp_interact(w, &b, &log);
            for(int t = 0; t < 60 && !b.over; t++){
                int act = (int)(rnd() % 4), arg = (int)(rnd() % 6) - 1;
                mp_turn(w, &b, act, arg, &log);
                for(int k = 0; k < log.n; k++)
                    if(log.ev[k].kind == MP_EV_LEARN_FULL && log.ev[k].a < w->n_party)
                        mp_learn(g, &w->party[log.ev[k].a], log.ev[k].b, (int)(rnd() % 5) - 1);
            }
        }
        walked += w->x + w->y + w->n_party;
        if(!mp_save(w, save_path)) mp_load(w, g, head, save_path);
    }
    free(head);
    free(w);
}

static void walk(MonGame *g){
    uint8_t *scratch = malloc(MG_HEAD_MAX + MG_SPRITE_MAX);
    mg_verify(g, scratch, MG_HEAD_MAX + MG_SPRITE_MAX);
    free(scratch);
    char small[3], big[300];
    for(uint32_t i = 0; i < g->n_moves; i++){
        MgMove m;
        if(!mg_move(g, i, &m)){ mg_text(g, m.name, small, sizeof small); mg_text(g, m.name, big, sizeof big); }
    }
    for(uint32_t i = 0; i < g->n_mons; i++){
        MgMon m;
        if(mg_mon(g, i, &m)) continue;
        mg_text(g, m.name, small, sizeof small);
        for(uint32_t j = 0; j < m.n_learn; j++){ uint8_t lv; uint16_t mv; mg_learn(g, &m, j, &lv, &mv); }
        uint8_t *sp = malloc(m.sprite_len ? m.sprite_len : 1);
        if(mg_sprite(g, &m, sp, m.sprite_len) > 0) walked += sp[m.sprite_len - 1];
        free(sp);
    }
    for(uint32_t t = 0; t < g->n_tables; t++){
        uint16_t n;
        if(mg_enc_rows(g, t, &n)) continue;
        for(uint32_t j = 0; j < n; j++){ MgEncRow r; mg_enc_row(g, t, j, &r); }
    }
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgAreaDir d;
        if(mg_area_dir(g, i, &d)) continue;
        uint8_t *head = malloc(d.head_len >= 64 && d.head_len <= MG_HEAD_MAX ? d.head_len : 64);
        MgArea a;
        if(mg_area_open(g, i, &a, head, d.head_len >= 64 && d.head_len <= MG_HEAD_MAX ? d.head_len : 64)){ free(head); continue; }
        for(uint32_t k = 0; k <= a.colours + 2u; k++) walked += mg_palette(&a, k);
        for(uint32_t k = 0; k < a.n_warps; k++){ MgWarp x; mg_warp(&a, k, &x); walked += x.to; }
        for(uint32_t k = 0; k < a.n_zones; k++){ MgZone x; mg_zone(&a, k, &x); walked += x.table; }
        for(uint32_t k = 0; k < a.n_signs; k++){ MgSign x; mg_sign(&a, k, &x); mg_text(g, x.text, big, sizeof big); }
        for(uint32_t k = 0; k < a.n_overlays; k++){ uint16_t c, t; mg_overlay(&a, k, &c, &t); walked += t; }
        uint8_t *chunk = malloc(MG_CHUNK_BYTES);
        for(uint32_t k = 0; k < (uint32_t)a.ccols * a.crows; k++) mg_chunk(g, &a, k, chunk);
        free(chunk);
        uint32_t tsz = MG_TILE * MG_TILE * a.bpp / 8, fsz = MG_CHAR_W * MG_CHAR_H * a.bpp / 8;
        uint8_t *tile = malloc(tsz), *fr = malloc(fsz);
        for(uint32_t t = 0; t <= a.n_tiles + 1u; t++) if(!mg_tile(g, &a, t, tile, tsz)) walked += tile[tsz - 1];
        for(uint32_t k = 0; k < a.n_chars * 12u + 1; k++) if(!mg_frame(g, &a, k, fr, fsz)) walked += fr[fsz - 1];
        free(tile); free(fr); free(head);
    }
    play(g);
}

static uint8_t *slurp(const char *path, long *n){
    FILE *f = fopen(path, "rb");
    if(!f){ fprintf(stderr, "monster_fuzz: can't read %s (run from the repo root)\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if(fread(b, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

static int open_and_verify(uint8_t *b, long n){
    MonGame g;
    if(open_bytes(&g, b, n)) return 1;
    uint8_t *scratch = malloc(MG_HEAD_MAX + MG_SPRITE_MAX);
    int r = mg_verify(&g, scratch, MG_HEAD_MAX + MG_SPRITE_MAX);
    free(scratch);
    mg_close(&g);
    return r != 0;
}

int main(void){
    long n;
    uint8_t *good = slurp(FILE_PATH, &n), *b = malloc((size_t)n);
    int fails = 0;
    save_path = getenv("MONSTER_SAVE") ? getenv("MONSTER_SAVE") : "build/monster_fuzz_save.bin";
    {
        MonGame g;
        memcpy(b, good, (size_t)n);
        if(open_bytes(&g, b, n) || (walk(&g), 0)){ printf("monster_fuzz: the fixture doesn't open: %s\n", g.err); return 1; }
        mg_close(&g);
    }

    /* 1. every truncation */
    long trunc = 0;
    for(long k = 0; k < n; k++){
        MonGame g;
        memcpy(b, good, (size_t)n);
        if(!open_bytes(&g, b, k)){ mg_close(&g); printf("FAIL: truncated to %ld bytes, it opened\n", k); fails++; }
        trunc++;
    }

    /* 2. flipped bits: all of the first part, random ones after */
    uint32_t aoff, alen;
    long dense = 0, flips = 0;
    sect(good, n, MG_ADAT, &aoff, &alen);
    dense = aoff;
    for(long k = 0; k < dense * 8 + 30000; k++){
        long bit = k < dense * 8 ? k : (long)(((uint64_t)rnd() << 8 | (rnd() & 255)) % (uint64_t)(n * 8));
        memcpy(b, good, (size_t)n);
        b[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if(!open_and_verify(b, n)){ printf("FAIL: bit %ld flipped, and the file still checks out\n", bit); fails++; }
        flips++;
    }

    /* 3. hostile: the CRCs fixed */
    long opened = 0, hostile = 0;
    for(int k = 0; k < 2500; k++){
        memcpy(b, good, (size_t)n);
        int changes = 1 + (int)(rnd() % 4);
        for(int c = 0; c < changes; c++){
            long at;
            if(rnd() % 3) at = (long)(rnd() % (uint32_t)(dense > 0 ? dense : n));     /* the small sections */
            else at = (long)(rnd() % (uint32_t)n);
            static const uint8_t V[] = { 0, 1, 2, 3, 4, 15, 16, 64, 127, 128, 254, 255 };
            b[at] = rnd() % 2 ? V[rnd() % sizeof V] : (uint8_t)rnd();
        }
        fix_crcs(b, n);
        MonGame g;
        hostile++;
        if(open_bytes(&g, b, n)) continue;
        opened++;
        walk(&g);
        mg_close(&g);
    }
    remove(save_path);
    printf("monster_fuzz: %ld truncations, %ld bit flips, %ld hostile files (%ld opened and were played); %s\n",
           trunc, flips, hostile, opened, fails ? "FAILED" : "OK");
    free(b);
    free(good);
    return fails ? 1 : 0;
}
