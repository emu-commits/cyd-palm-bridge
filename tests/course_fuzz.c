/* course_fuzz.c -- the Study course reader against damaged and hostile files,
 * under AddressSanitizer and UBSan (make ftest).
 *
 * Three kinds of damage, on the committed course files:
 *
 *   1. TRUNCATION at every length (every 97th for the big demo): must fail on
 *      open. A card pulled mid-copy leaves exactly this.
 *   2. ONE FLIPPED BIT anywhere (every bit of the small decks; thousands at
 *      random in the demo): open or course_verify() MUST fail. Every byte is
 *      covered by a CRC or a zero-padding rule (COURSE_FORMAT.md section 6), so
 *      "Check course" catches any corruption, not just most.
 *   3. HOSTILE: bytes changed and then every CRC fixed up to match (the item
 *      and level CRCs, the section CRCs, the table's, the header's), so that the
 *      damage gets past the CRCs to the checks behind them. The result may open
 *      or not; what matters is that nothing reads outside a buffer or trips
 *      UBSan while every item, field, link and picture is walked.
 *
 * Deterministic (a fixed-seed LCG), so a failure reproduces. Run from the repo
 * root. */
#include "course.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *FILES[] = {
    "tests/data/study/features/course.srs",
    "tests/data/study/cards/course.srs",
    "courses/demo-kanji/course.srs",
};

static uint32_t seed = 12345;
static uint32_t rnd(void){ seed = seed * 1103515245u + 12345u; return seed >> 8; }

static uint32_t le32(const uint8_t *p){ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static void put32(uint8_t *p, uint32_t v){ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *slurp(const char *path, long *n){
    FILE *f = fopen(path, "rb");
    if(!f){ fprintf(stderr, "course_fuzz: can't read %s (run from the repo root)\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if(fread(b, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

static int open_bytes(Course *c, uint8_t *b, long n){
    FILE *f = fmemopen(b, n, "rb");
    if(!f){ perror("fmemopen"); exit(2); }
    return course_open_fp(c, f);
}

/* walk everything the app could touch, into buffers of exactly the size the
 * API promises is enough, so ASan sees any overrun */
static long walked;
static void walk(Course *c){
    uint8_t *buf = malloc(COURSE_ITEM_BUF);
    char small[5], big[300];
    for(uint16_t k = 0; k < 0x60; k++){
        course_meta_text(c, k, small, sizeof small);
        course_meta_text(c, k, big, sizeof big);
    }
    for(uint32_t i = 0; ; i++){
        uint16_t k, n;
        if(course_meta_entry(c, i, &k, buf, COURSE_ITEM_BUF, &n)) break;
    }
    for(uint32_t i = 0; i < c->n_levels; i++){
        CourseLevel l;
        if(course_level(c, i, &l)) continue;
        uint8_t *t = malloc(l.text_len ? l.text_len : 1);
        int r = course_level_load(c, &l, t, l.text_len);
        if(r >= 0){
            uint32_t pos = 0;
            CourseField f;
            while(course_field_next(t, (uint32_t)r, &pos, &f) == 1) walked += f.len;
        }
        free(t);
    }
    for(uint32_t n = 0; n < c->n_items; n++){
        CourseItem it;
        if(course_item(c, n, &it)) continue;
        uint32_t need = it.text_len + 8u * it.n_links;
        uint8_t *t = malloc(need);
        int r = course_item_load(c, &it, t, need);
        if(r < 0){ free(t); continue; }
        uint32_t pos = 0;
        CourseField f;
        while(course_field_next(t, it.text_len, &pos, &f) == 1){
            for(uint16_t j = 0; j < f.len; j++) walked += (uint8_t)f.text[j];
            if(!(f.attr & CF_A_PICTURE)) continue;
            CoursePic p;
            if(course_pic(c, f.pic, &p)) continue;
            uint32_t sz = (uint32_t)p.h * p.stride;
            uint8_t *rows = malloc(sz);
            if(!course_pic_rows(c, &p, 0, p.h, rows, sz)) walked += rows[sz - 1];
            course_pic_check(c, &p, rows, sz);
            free(rows);
        }
        for(uint16_t j = 0; j < it.n_links; j++){
            CourseLink l;
            course_link(t, &it, j, &l);
            walked += l.target;
        }
        uint32_t m;
        course_find(c, it.id, &m);
        free(t);
    }
    for(uint32_t j = 0; j < c->n_items; j++){
        uint32_t id, m;
        course_idx(c, j, &id, &m);
    }
    free(buf);
}

static uint8_t *vbuf;
static int open_and_verify(uint8_t *b, long n, int do_walk){
    Course c;
    int r = open_bytes(&c, b, n);
    if(r) return r;
    if(do_walk) walk(&c);
    r = course_verify(&c, vbuf, COURSE_VERIFY_BUF);
    course_close(&c);
    return r;
}

/* fix every CRC in the file after a change: items, levels, sections, table,
 * header -- as far as the (possibly damaged) structure allows */
static void fixup(uint8_t *b, long n){
    if(n < 32) return;
    uint16_t nsec = le16(b + 12);
    if(nsec > 16 || 32 + 16L * nsec > n) return;
    uint32_t item_off = 0, item_len = 0, text_off = 0, text_len = 0, link_off = 0, link_len = 0, levl_off = 0, levl_len = 0;
    for(int i = 0; i < nsec; i++){
        uint8_t *e = b + 32 + 16 * i;
        uint32_t off = le32(e + 4), len = le32(e + 8);
        if(off > (uint32_t)n || len > (uint32_t)n - off) continue;
        if(!memcmp(e, "ITEM", 4)){ item_off = off; item_len = len; }
        if(!memcmp(e, "TEXT", 4)){ text_off = off; text_len = len; }
        if(!memcmp(e, "LINK", 4)){ link_off = off; link_len = len; }
        if(!memcmp(e, "LEVL", 4)){ levl_off = off; levl_len = len; }
    }
    for(uint32_t p = 0; p + 32 <= item_len; p += 32){
        uint8_t *e = b + item_off + p;
        uint32_t to = le32(e + 8), tl = le16(e + 12), nl = le16(e + 14), lo = le32(e + 16);
        if(to > text_len || tl > text_len - to) continue;
        uint32_t crc = course_crc32(0, b + text_off + to, tl);
        if(nl && lo <= link_len && 8u * nl <= link_len - lo) crc = course_crc32(crc, b + link_off + lo, 8u * nl);
        put32(e + 24, crc);
    }
    for(uint32_t p = 0; p + 24 <= levl_len; p += 24){
        uint8_t *e = b + levl_off + p;
        uint32_t to = le32(e + 12), tl = le16(e + 16);
        if(to <= text_len && tl <= text_len - to) put32(e + 20, course_crc32(0, b + text_off + to, tl));
    }
    for(int i = 0; i < nsec; i++){
        uint8_t *e = b + 32 + 16 * i;
        uint32_t off = le32(e + 4), len = le32(e + 8);
        if(off <= (uint32_t)n && len <= (uint32_t)n - off) put32(e + 12, course_crc32(0, b + off, len));
    }
    put32(b + 24, course_crc32(0, b + 32, 16u * nsec));
    put32(b + 28, course_crc32(0, b, 28));
}

int main(void){
    vbuf = malloc(COURSE_VERIFY_BUF);
    long total_trunc = 0, total_flip = 0, total_hostile = 0, hostile_opened = 0;
    int failures = 0;
    for(size_t fi = 0; fi < sizeof FILES / sizeof FILES[0]; fi++){
        long n;
        uint8_t *orig = slurp(FILES[fi], &n);
        uint8_t *b = malloc(n);
        int small = n < 16384;
        memcpy(b, orig, n);
        if(open_and_verify(b, n, 1)){ fprintf(stderr, "course_fuzz: %s doesn't verify as committed\n", FILES[fi]); return 1; }

        /* 1. truncation */
        for(long len = 0; len < n; len += (small || len < 2048) ? 1 : 97){
            memcpy(b, orig, n);
            if(open_and_verify(b, len, 0) == 0){
                fprintf(stderr, "FAIL %s: cut to %ld bytes, and it opened and verified\n", FILES[fi], len);
                failures++;
            }
            total_trunc++;
        }
        /* 2. one flipped bit: always caught */
        long flips = small ? n * 8 : 4000;
        for(long k = 0; k < flips; k++){
            long at = small ? k / 8 : (long)(rnd() % n);
            int bit = small ? k % 8 : rnd() % 8;
            memcpy(b, orig, n);
            b[at] ^= 1 << bit;
            if(open_and_verify(b, n, (k % 16) == 0) == 0){
                fprintf(stderr, "FAIL %s: bit %d of byte %ld flipped, and nothing noticed\n", FILES[fi], bit, at);
                failures++;
            }
            total_flip++;
        }
        /* 3. hostile: changed, then every CRC made to match */
        long rounds = small ? 20000 : 3000;
        for(long k = 0; k < rounds; k++){
            memcpy(b, orig, n);
            int changes = 1 + rnd() % 4;
            for(int j = 0; j < changes; j++){
                long at = (k % 3 == 0) ? (long)(rnd() % (n < 400 ? n : 400)) : (long)(rnd() % n);
                static const uint8_t special[] = { 0x00, 0xFF, 0x7F, 0x80, 0x01, 0x20, 0x1F, 0xC0, 0xF4 };
                b[at] = (rnd() & 1) ? special[rnd() % sizeof special] : (uint8_t)rnd();
            }
            fixup(b, n);
            Course c;
            if(open_bytes(&c, b, n) == 0){
                hostile_opened++;
                walk(&c);
                course_verify(&c, vbuf, COURSE_VERIFY_BUF);
                course_close(&c);
            }
            total_hostile++;
        }
        free(b);
        free(orig);
    }
    free(vbuf);
    printf("course_fuzz: %ld truncations, %ld bit flips (all caught), %ld hostile files (%ld opened and walked): %s\n",
           total_trunc, total_flip, total_hostile, hostile_opened, failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
