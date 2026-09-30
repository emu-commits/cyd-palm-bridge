/* course.c -- the Study course reader. See course.h and docs/COURSE_FORMAT.md.
 *
 * Every read goes through rd(), which refuses anything outside the file, and
 * every structure is checked against its section before its contents are used.
 * The fuzz gate (tests/course_fuzz.c, in make ftest) runs this under ASan and
 * UBSan over thousands of damaged copies, including ones whose CRCs were fixed
 * up so that the damage reaches the checks behind them. */
#include "course.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HDR_SIZE   32
#define SECT_SIZE  16
#define LEVL_SIZE  24
#define ITEM_SIZE  32
#define LINK_SIZE  8
#define IDX_SIZE   8
#define PIC_HDR    12
#define SEP        0x1F

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* CRC-32 (zlib's), a nibble at a time: a 64-byte table instead of 1 KB */
static const uint32_t CRC_T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};
uint32_t course_crc32(uint32_t crc, const void *buf, uint32_t len){
    const uint8_t *p = buf;
    crc = ~crc;
    while(len--){
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_T[crc & 15];
        crc = (crc >> 4) ^ CRC_T[crc & 15];
    }
    return ~crc;
}

static int bad(Course *c, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    return COURSE_EBAD;
}

/* read len bytes at off; anything past the end of the file is damage */
static int rd(Course *c, uint32_t off, void *buf, uint32_t len){
    if(!len) return 0;
    if(!c->f) return COURSE_EIO;
    if(off > c->size || len > c->size - off) return bad(c, "a read past the end of the file");
    if(fseek(c->f, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, c->f) != len){
        snprintf(c->err, sizeof c->err, "the card couldn't be read");
        return COURSE_EIO;
    }
    return 0;
}

/* does [off, off+len) lie inside a section of size slen? (no overflow) */
static int inside(uint32_t off, uint32_t len, uint32_t slen){
    return off <= slen && len <= slen - off;
}

static int crc_range(Course *c, uint32_t off, uint32_t len, uint32_t *out){
    uint8_t b[256];
    uint32_t crc = 0;
    while(len){
        uint32_t n = len < sizeof b ? len : sizeof b;
        int r = rd(c, off, b, n);
        if(r) return r;
        crc = course_crc32(crc, b, n);
        off += n; len -= n;
    }
    *out = crc;
    return 0;
}

static int zero_range(Course *c, uint32_t off, uint32_t len){
    uint8_t b[64];
    while(len){
        uint32_t n = len < sizeof b ? len : sizeof b;
        int r = rd(c, off, b, n);
        if(r) return r;
        for(uint32_t i = 0; i < n; i++) if(b[i]) return bad(c, "padding at %u isn't zero", (unsigned)(off + i));
        off += n; len -= n;
    }
    return 0;
}

/* ---- text rules ---------------------------------------------------------- */

enum { TX_PALM, TX_KANA, TX_ANY };

static int is_kana(uint32_t u){
    return (u >= 0x3041 && u <= 0x3096) || (u >= 0x309B && u <= 0x309E) || (u >= 0x30A1 && u <= 0x30FE);
}

/* Strict UTF-8, no NUL or control characters (U+001F only where sep is
 * allowed), and the code points the way the field is drawn allows. */
static int text_ok(const uint8_t *s, uint32_t n, int mode, int sep){
    uint32_t i = 0;
    while(i < n){
        uint32_t u, need;
        uint8_t b = s[i];
        if(b < 0x80){ u = b; need = 0; }
        else if(b >= 0xC2 && b <= 0xDF){ u = b & 0x1F; need = 1; }
        else if(b >= 0xE0 && b <= 0xEF){ u = b & 0x0F; need = 2; }
        else if(b >= 0xF0 && b <= 0xF4){ u = b & 0x07; need = 3; }
        else return 0;
        if(need > n - i - 1) return 0;
        for(uint32_t k = 1; k <= need; k++){
            if((s[i + k] & 0xC0) != 0x80) return 0;
            u = u << 6 | (s[i + k] & 0x3F);
        }
        if((need == 2 && u < 0x800) || (need == 3 && (u < 0x10000 || u > 0x10FFFF))) return 0;
        if(u >= 0xD800 && u <= 0xDFFF) return 0;
        if(u < 0x20 && !(sep && u == SEP)) return 0;
        if(u == 0x7F) return 0;
        if(mode == TX_PALM && u > 0xFF) return 0;
        if(mode == TX_KANA && !is_kana(u)) return 0;
        i += need + 1;
    }
    return 1;
}

/* copy text into a fixed field, cut at a character boundary */
static void copy_text(char *dst, uint32_t cap, const uint8_t *src, uint32_t n){
    if(n > cap - 1){
        n = cap - 1;
        while(n && (src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- META ---------------------------------------------------------------- */

enum {
    K_ID = 1, K_TITLE, K_VERSION, K_AUTHOR, K_LICENCE, K_LANGUAGE, K_DESC, K_SOURCE,
    K_KIND = 0x10, K_ROLE, K_GROUP,
    K_SCHED = 0x20, K_GRADING, K_BATCH, K_LEVEL_PCT, K_BY_LINKS, K_KNOWN,
    K_STAGES = 0x30, K_DROPS,
    K_LADDER = 0x40, K_MULT, K_EASE, K_EASE_CHANGE, K_RELEARN, K_DAY_ALIGNED,
    K_RENDER = 0x50,
};

/* the keys that may appear once, as bits: which have been seen */
static int once_bit(uint16_t k){
    if(k >= K_ID && k <= K_SOURCE) return k - K_ID;
    if(k >= K_SCHED && k <= K_KNOWN) return 8 + k - K_SCHED;
    if(k >= K_STAGES && k <= K_DROPS) return 14 + k - K_STAGES;
    if(k >= K_LADDER && k <= K_DAY_ALIGNED) return 16 + k - K_LADDER;
    if(k == K_RENDER) return 22;
    return -1;
}

static int nums(Course *c, uint16_t k, uint16_t n, uint32_t lo_count, uint32_t hi_count){
    if(n % 4 || n / 4 < lo_count || n / 4 > hi_count)
        return bad(c, "META key %04x has %u values", k, (unsigned)(n / 4));
    return 0;
}

static int parse_meta(Course *c, const uint8_t *m, uint32_t len){
    uint32_t seen = 0, p = 0;
    int have_known = 0, have_drops = 0, have_ec = 0;
    while(p < len){
        if(len - p < 4) return bad(c, "META ends mid-entry");
        uint16_t k = le16(m + p), n = le16(m + p + 2);
        const uint8_t *v = m + p + 4;
        if(n > len - p - 4) return bad(c, "META key %04x runs past the section", k);
        p += 4 + n;
        int b = once_bit(k);
        if(b >= 0){
            if(seen & (1u << b)) return bad(c, "META key %04x twice", k);
            seen |= 1u << b;
        }
        switch(k){
        case K_ID: case K_TITLE: case K_VERSION: case K_AUTHOR: case K_LICENCE:
        case K_LANGUAGE: case K_DESC: case K_SOURCE: case K_ROLE:
            if(!text_ok(v, n, TX_PALM, 0)) return bad(c, "META key %04x isn't text", k);
            if(k == K_ID){
                if(n < 1 || n > 31) return bad(c, "the course id is %u bytes", n);
                for(uint16_t i = 0; i < n; i++){
                    uint8_t ch = v[i];
                    if(!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
                        return bad(c, "the course id has a character a folder can't");
                }
                copy_text(c->id, sizeof c->id, v, n);
            }
            else if(k == K_TITLE) copy_text(c->title, sizeof c->title, v, n);
            else if(k == K_VERSION) copy_text(c->version, sizeof c->version, v, n);
            else if(k == K_LICENCE) copy_text(c->licence, sizeof c->licence, v, n);
            else if(k == K_LANGUAGE) copy_text(c->language, sizeof c->language, v, n);
            else if(k == K_ROLE){
                if(c->n_roles >= COURSE_MAX_ROLES) return bad(c, "more than %d roles", COURSE_MAX_ROLES);
                copy_text(c->roles[c->n_roles++], sizeof c->roles[0], v, n);
            }
            break;
        case K_KIND:
            if(n < 3 || !text_ok(v + 2, n - 2, TX_PALM, 0)) return bad(c, "a bad kind in META");
            if(c->n_kinds >= COURSE_MAX_KINDS) return bad(c, "more than %d kinds", COURSE_MAX_KINDS);
            if(v[0] < 1 || v[0] > 3) return bad(c, "a kind quizzed on nothing");
            c->kinds[c->n_kinds].quiz = v[0];
            c->kinds[c->n_kinds].flags = v[1];
            copy_text(c->kinds[c->n_kinds].name, sizeof c->kinds[0].name, v + 2, n - 2);
            c->n_kinds++;
            break;
        case K_GROUP:
            if(n < 5 || !text_ok(v + 4, n - 4, TX_PALM, 0)) return bad(c, "a bad group in META");
            if(c->n_groups >= COURSE_MAX_GROUPS) return bad(c, "more than %d groups", COURSE_MAX_GROUPS);
            c->groups[c->n_groups].from = le32(v);
            copy_text(c->groups[c->n_groups].name, sizeof c->groups[0].name, v + 4, n - 4);
            c->n_groups++;
            break;
        case K_SCHED:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) != CS_STAGES && le32(v) != CS_SM2) return bad(c, "an unknown scheduler %u", (unsigned)le32(v));
            c->scheduler = (uint8_t)le32(v);
            break;
        case K_GRADING:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) != 2 && le32(v) != 4) return bad(c, "grading with %u buttons", (unsigned)le32(v));
            c->grading = (uint8_t)le32(v);
            break;
        case K_BATCH:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) < 1 || le32(v) > 20) return bad(c, "a lesson batch of %u", (unsigned)le32(v));
            c->batch = (uint8_t)le32(v);
            break;
        case K_LEVEL_PCT:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) > 100) return bad(c, "a level unlock of %u%%", (unsigned)le32(v));
            c->level_pct = (uint8_t)le32(v);
            break;
        case K_BY_LINKS: case K_DAY_ALIGNED:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) > 1) return bad(c, "META key %04x isn't 0 or 1", k);
            if(k == K_BY_LINKS) c->by_links = (uint8_t)le32(v); else c->day_aligned = (uint8_t)le32(v);
            break;
        case K_KNOWN:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            if(le32(v) < 1 || le32(v) > COURSE_MAX_STEPS) return bad(c, "\"known\" is %u", (unsigned)le32(v));
            c->known = (uint8_t)le32(v); have_known = 1;
            break;
        case K_STAGES: case K_LADDER:
            if(nums(c, k, n, k == K_STAGES ? 2 : 1, COURSE_MAX_STEPS)) return COURSE_EBAD;
            c->n_steps = (uint8_t)(n / 4);
            for(uint8_t i = 0; i < c->n_steps; i++){
                c->steps[i] = le32(v + 4 * i);
                int last = i == c->n_steps - 1;
                if(k == K_STAGES && (last ? c->steps[i] != 0 : c->steps[i] == 0))
                    return bad(c, "stage intervals must end with one 0");
                if(k == K_LADDER && !c->steps[i]) return bad(c, "a ladder rung of 0");
            }
            break;
        case K_DROPS:
            if(nums(c, k, n, 3, 3)) return COURSE_EBAD;
            if(le32(v) > 255 || le32(v + 4) > 255 || le32(v + 8) > 255) return bad(c, "bad drops");
            c->drop = (uint8_t)le32(v); c->drop_high = (uint8_t)le32(v + 4); c->high_from = (uint8_t)le32(v + 8);
            have_drops = 1;
            break;
        case K_MULT:
            if(nums(c, k, n, 3, 3)) return COURSE_EBAD;
            for(int i = 0; i < 3; i++){
                if(le32(v + 4 * i) < 1 || le32(v + 4 * i) > 1000) return bad(c, "a multiplier out of range");
                c->mult[i] = (uint16_t)le32(v + 4 * i);
            }
            break;
        case K_EASE:
            if(nums(c, k, n, 2, 2)) return COURSE_EBAD;
            c->ease[0] = (uint16_t)(le32(v) > 1405 ? 0 : le32(v));
            c->ease[1] = (uint16_t)(le32(v + 4) > 1405 ? 0 : le32(v + 4));
            if(c->ease[1] < 130 || c->ease[0] < c->ease[1] || c->ease[0] % 5 || c->ease[1] % 5)
                return bad(c, "ease out of range");
            break;
        case K_EASE_CHANGE:
            if(nums(c, k, n, 4, 4)) return COURSE_EBAD;
            for(int i = 0; i < 4; i++){
                int32_t d = (int32_t)le32(v + 4 * i);
                if(d < -1000 || d > 1000 || d % 5) return bad(c, "an ease change out of range");
                c->ease_change[i] = (int16_t)d;
            }
            have_ec = 1;
            break;
        case K_RELEARN:
            if(nums(c, k, n, 1, 1)) return COURSE_EBAD;
            c->relearn = le32(v);
            break;
        case K_RENDER:
            if(nums(c, k, n, 4, 4)) return COURSE_EBAD;
            for(int i = 0; i < 4; i++) c->render[i] = (uint16_t)(le32(v + 4 * i) > 1024 ? 0 : le32(v + 4 * i));
            break;
        default:
            break;                      /* a newer key: skip it */
        }
    }
    if(!c->id[0] || !c->title[0]) return bad(c, "META has no id or title");
    if(!c->n_kinds) return bad(c, "META has no kinds");
    if(!c->scheduler) return bad(c, "META has no scheduler");
    if(!c->n_steps) return bad(c, "META has no %s", c->scheduler == CS_STAGES ? "stages" : "ladder");
    if((seen & (1u << once_bit(K_STAGES))) && (seen & (1u << once_bit(K_LADDER))))
        return bad(c, "META has both stage intervals and a ladder");
    if(c->scheduler == CS_STAGES){
        if(!(seen & (1u << once_bit(K_STAGES)))) return bad(c, "a stages course without stage intervals");
        if(!have_drops){ c->drop = 1; c->drop_high = 2; c->high_from = 5; }
        if(!have_known) c->known = 5;
        if(c->high_from < 1 || c->high_from > c->n_steps) c->high_from = c->n_steps;
        if(!c->grading) c->grading = 2;
    } else {
        if(!(seen & (1u << once_bit(K_LADDER))) || !c->mult[0] || !c->ease[0])
            return bad(c, "an sm2 course without its ladder, multipliers or ease");
        if(!have_known) c->known = 1;
        if(!have_ec){ c->ease_change[0] = -20; c->ease_change[1] = -15; c->ease_change[3] = 15; }
        if(!(seen & (1u << once_bit(K_RELEARN)))) c->relearn = 600;
        if(!c->grading) c->grading = 4;
    }
    if(c->known > c->n_steps) return bad(c, "\"known\" is past the last %s", c->scheduler == CS_STAGES ? "stage" : "rung");
    if(!(seen & (1u << once_bit(K_BATCH)))) c->batch = 5;
    if(!(seen & (1u << once_bit(K_LEVEL_PCT)))) c->level_pct = 90;
    if(!(seen & (1u << once_bit(K_BY_LINKS)))) c->by_links = 1;
    return 0;
}

/* ---- open ---------------------------------------------------------------- */

void course_close(Course *c){
    if(c->f){ fclose(c->f); c->f = NULL; }
}

static int fail_open(Course *c, int r){
    course_close(c);
    return r;
}

int course_open(Course *c, const char *path){
    FILE *f = fopen(path, "rb");
    if(!f){
        memset(c, 0, sizeof *c);
        snprintf(c->err, sizeof c->err, "can't open the course file");
        return COURSE_EIO;
    }
    return course_open_fp(c, f);
}

int course_open_fp(Course *c, FILE *f){
    memset(c, 0, sizeof *c);
    c->f = f;
    if(fseek(c->f, 0, SEEK_END) != 0) return fail_open(c, COURSE_EIO);
    long sz = ftell(c->f);
    if(sz < HDR_SIZE || sz > 0x7FFFFFFFL) return fail_open(c, bad(c, "not a course file (%ld bytes)", sz));
    c->size = (uint32_t)sz;

    uint8_t h[HDR_SIZE];
    int r = rd(c, 0, h, HDR_SIZE);
    if(r) return fail_open(c, r);
    if(memcmp(h, "SRSC", 4)) return fail_open(c, bad(c, "not a course file"));
    if(le16(h + 4) != COURSE_FORMAT_MAJOR){
        snprintf(c->err, sizeof c->err, "this course needs a newer Study (format %u)", le16(h + 4));
        return fail_open(c, COURSE_EVERSION);
    }
    if(course_crc32(0, h, 28) != le32(h + 28)) return fail_open(c, bad(c, "the header is damaged"));
    c->minor = le16(h + 6);
    if(le32(h + 8) != c->size)
        return fail_open(c, bad(c, "the file is %u bytes, not %u: copied only in part?", (unsigned)c->size, (unsigned)le32(h + 8)));
    uint16_t nsec = le16(h + 12);
    c->n_items = le32(h + 16);
    c->n_levels = le32(h + 20);
    c->fingerprint = le32(h + 24);
    if(nsec < 5 || nsec > 16 || HDR_SIZE + SECT_SIZE * (uint32_t)nsec > c->size)
        return fail_open(c, bad(c, "a bad section count"));
    if(c->n_items < 1 || c->n_items > COURSE_MAX_ITEMS) return fail_open(c, bad(c, "%u items", (unsigned)c->n_items));
    if(c->n_levels < 1 || c->n_levels > 65535 || c->n_levels > c->n_items)
        return fail_open(c, bad(c, "%u levels", (unsigned)c->n_levels));

    uint8_t t[SECT_SIZE * 16];
    r = rd(c, HDR_SIZE, t, SECT_SIZE * nsec);
    if(r) return fail_open(c, r);
    if(course_crc32(0, t, SECT_SIZE * nsec) != c->fingerprint) return fail_open(c, bad(c, "the section table is damaged"));
    uint32_t end = HDR_SIZE + SECT_SIZE * nsec;
    uint8_t found = 0;
    static const char TAGS[7][5] = { "META", "LEVL", "ITEM", "TEXT", "BMP ", "LINK", "IDX " };
    CourseSect *slot[7] = { &c->meta, &c->levl, &c->item, &c->text, &c->bmp, &c->link, &c->idx };
    for(uint16_t i = 0; i < nsec; i++){
        const uint8_t *e = t + SECT_SIZE * i;
        uint32_t off = le32(e + 4), len = le32(e + 8);
        if(off % 4 || off < end || !inside(off, len, c->size))
            return fail_open(c, bad(c, "section %u is out of place", i));
        end = off + len;
        for(int k = 0; k < 7; k++) if(!memcmp(e, TAGS[k], 4)){
            if(found & (1u << k)) return fail_open(c, bad(c, "section %.4s twice", TAGS[k]));
            found |= 1u << k;
            slot[k]->off = off; slot[k]->len = len; slot[k]->crc = le32(e + 12);
        }
    }
    if((found & 0x4F) != 0x4F) return fail_open(c, bad(c, "a required section is missing"));
    if(c->levl.len != LEVL_SIZE * c->n_levels || c->item.len != ITEM_SIZE * c->n_items
       || c->idx.len != IDX_SIZE * c->n_items || c->link.len % LINK_SIZE)
        return fail_open(c, bad(c, "a section's size doesn't match its count"));

    /* META */
    if(!c->meta.len || c->meta.len > COURSE_META_MAX) return fail_open(c, bad(c, "META is %u bytes", (unsigned)c->meta.len));
    uint8_t *m = malloc(c->meta.len);
    if(!m){ snprintf(c->err, sizeof c->err, "out of memory"); return fail_open(c, COURSE_ENOMEM); }
    r = rd(c, c->meta.off, m, c->meta.len);
    if(!r && course_crc32(0, m, c->meta.len) != c->meta.crc) r = bad(c, "META is damaged");
    if(!r) r = parse_meta(c, m, c->meta.len);
    free(m);
    if(r) return fail_open(c, r);

    /* LEVL: its CRC, and that the levels cover the items in order */
    uint32_t crc;
    if((r = crc_range(c, c->levl.off, c->levl.len, &crc))) return fail_open(c, r);
    if(crc != c->levl.crc) return fail_open(c, bad(c, "the level table is damaged"));
    uint32_t next = 0, last = 0;
    for(uint32_t i = 0; i < c->n_levels; i++){
        CourseLevel l;
        if((r = course_level(c, i, &l))) return fail_open(c, r);
        if(l.number <= last || l.first != next || l.count < 1 || l.count > c->n_items - next)
            return fail_open(c, bad(c, "level %u is out of order", l.number));
        last = l.number; next += l.count;
    }
    if(next != c->n_items) return fail_open(c, bad(c, "the levels don't cover every item"));

    /* IDX: its CRC, sorted ids, item numbers in range */
    if((r = crc_range(c, c->idx.off, c->idx.len, &crc))) return fail_open(c, r);
    if(crc != c->idx.crc) return fail_open(c, bad(c, "the id index is damaged"));
    uint32_t prev = 0;
    for(uint32_t j = 0; j < c->n_items; j++){
        uint32_t id, n;
        if((r = course_idx(c, j, &id, &n))) return fail_open(c, r);
        if(id <= prev) return fail_open(c, bad(c, "the id index isn't sorted"));
        prev = id;
    }
    return 0;
}

/* ---- items, levels, fields ----------------------------------------------- */

int course_item(Course *c, uint32_t n, CourseItem *it){
    uint8_t e[ITEM_SIZE];
    if(n >= c->n_items) return COURSE_ERANGE;
    int r = rd(c, c->item.off + ITEM_SIZE * n, e, ITEM_SIZE);
    if(r) return r;
    it->id = le32(e);
    it->level = le16(e + 4);
    it->kind = e[6];
    it->flags = e[7];
    it->text_off = le32(e + 8);
    it->text_len = le16(e + 12);
    it->n_links = le16(e + 14);
    it->link_off = le32(e + 16);
    it->rank = le16(e + 20);
    it->crc = le32(e + 24);
    it->n = n;
    if(!it->id || it->kind >= c->n_kinds || !it->text_len || it->text_len > COURSE_TEXT_MAX
       || !inside(it->text_off, it->text_len, c->text.len) || it->n_links > COURSE_LINKS_MAX
       || (it->n_links && (it->link_off % LINK_SIZE || !inside(it->link_off, (uint32_t)LINK_SIZE * it->n_links, c->link.len))))
        return bad(c, "item %u is damaged", (unsigned)n);
    return 0;
}

/* the checks every loaded run of fields gets: bounds, text rules, pictures */
static int fields_ok(Course *c, const uint8_t *t, uint32_t len, int is_item){
    uint32_t pos = 0;
    int terms = 0, first = 1, r;
    CourseField f;
    while((r = course_field_next(t, len, &pos, &f)) == 1){
        if(is_item && first && f.tag != CF_TERM) return 0;
        first = 0;
        if(f.tag == CF_TERM) terms++;
        if(!is_item && f.tag == CF_TERM) return 0;
        int sep = f.tag == CF_SENSE || f.tag == CF_FORM;
        int mode = (f.attr & CF_A_PICTURE) ? TX_ANY : (f.attr & CF_A_KANA) ? TX_KANA : TX_PALM;
        if(!text_ok((const uint8_t *)f.text, f.len, mode, sep)) return 0;
        if((f.attr & CF_A_PICTURE) && (f.pic % 4 || !inside(f.pic, PIC_HDR, c->bmp.len))) return 0;
    }
    if(r < 0) return 0;
    return is_item ? terms == 1 : 1;
}

int course_item_load(Course *c, const CourseItem *it, uint8_t *buf, uint32_t cap){
    uint32_t links = (uint32_t)LINK_SIZE * it->n_links;
    uint32_t need = it->text_len + links;
    if(need > cap) return COURSE_ERANGE;
    int r = rd(c, c->text.off + it->text_off, buf, it->text_len);
    if(!r && links) r = rd(c, c->link.off + it->link_off, buf + it->text_len, links);
    if(r) return r;
    if(course_crc32(0, buf, need) != it->crc) return bad(c, "item %u is damaged", (unsigned)it->n);
    if(!fields_ok(c, buf, it->text_len, 1)) return bad(c, "item %u has a bad field", (unsigned)it->n);
    for(uint16_t i = 0; i < it->n_links; i++){
        CourseLink l;
        course_link(buf, it, i, &l);
        if(l.target >= c->n_items || l.target == it->n || l.type < CL_UNLOCK_AFTER || l.type > CL_RELATED
           || l.role > c->n_roles)
            return bad(c, "item %u has a bad link", (unsigned)it->n);
    }
    return (int)need;
}

int course_field_next(const uint8_t *t, uint32_t len, uint32_t *pos, CourseField *f){
    uint32_t p = *pos;
    if(p >= len) return 0;
    if(len - p < 4) return -1;
    f->tag = t[p];
    f->attr = t[p + 1];
    f->len = le16(t + p + 2);
    f->pic = 0;
    p += 4;
    if(f->attr & CF_A_PICTURE){
        if(len - p < 4) return -1;
        f->pic = le32(t + p);
        p += 4;
    }
    if(f->len > len - p) return -1;
    f->text = (const char *)t + p;
    *pos = p + f->len;
    return 1;
}

void course_link(const uint8_t *buf, const CourseItem *it, uint16_t i, CourseLink *l){
    const uint8_t *e = buf + it->text_len + (uint32_t)LINK_SIZE * i;
    l->target = le32(e);
    l->type = e[4];
    l->role = e[5];
}

int course_level(Course *c, uint32_t i, CourseLevel *l){
    uint8_t e[LEVL_SIZE];
    if(i >= c->n_levels) return COURSE_ERANGE;
    int r = rd(c, c->levl.off + LEVL_SIZE * i, e, LEVL_SIZE);
    if(r) return r;
    l->number = le16(e);
    l->part = e[2];
    l->parts = e[3];
    l->first = le32(e + 4);
    l->count = le32(e + 8);
    l->text_off = le32(e + 12);
    l->text_len = le16(e + 16);
    l->crc = le32(e + 20);
    if(!l->number || l->text_len > COURSE_TEXT_MAX || !inside(l->text_off, l->text_len, c->text.len))
        return bad(c, "level %u is damaged", (unsigned)i);
    return 0;
}

int course_level_load(Course *c, const CourseLevel *l, uint8_t *buf, uint32_t cap){
    if(l->text_len > cap) return COURSE_ERANGE;
    int r = rd(c, c->text.off + l->text_off, buf, l->text_len);
    if(r) return r;
    if(course_crc32(0, buf, l->text_len) != l->crc || !fields_ok(c, buf, l->text_len, 0))
        return bad(c, "level %u's text is damaged", l->number);
    return l->text_len;
}

int course_idx(Course *c, uint32_t j, uint32_t *id, uint32_t *n){
    uint8_t e[IDX_SIZE];
    if(j >= c->n_items) return COURSE_ERANGE;
    int r = rd(c, c->idx.off + IDX_SIZE * j, e, IDX_SIZE);
    if(r) return r;
    *id = le32(e);
    *n = le32(e + 4);
    if(*n >= c->n_items) return bad(c, "the id index is damaged");
    return 0;
}

int course_find(Course *c, uint32_t id, uint32_t *n){
    uint32_t lo = 0, hi = c->n_items;
    while(lo < hi){
        uint32_t mid = lo + (hi - lo) / 2, mid_id, mid_n;
        int r = course_idx(c, mid, &mid_id, &mid_n);
        if(r) return r;
        if(mid_id == id){
            CourseItem it;
            if((r = course_item(c, mid_n, &it))) return r;
            if(it.id != id) return bad(c, "the id index doesn't match the items");
            *n = mid_n;
            return 0;
        }
        if(mid_id < id) lo = mid + 1; else hi = mid;
    }
    return COURSE_ERANGE;
}

/* ---- pictures ------------------------------------------------------------ */

int course_pic(Course *c, uint32_t off, CoursePic *p){
    uint8_t h[PIC_HDR];
    if(off % 4 || !inside(off, PIC_HDR, c->bmp.len)) return bad(c, "a picture is out of range");
    int r = rd(c, c->bmp.off + off, h, PIC_HDR);
    if(r) return r;
    p->off = off;
    p->w = le16(h);
    p->h = le16(h + 2);
    p->bpp = h[4];
    p->stride = le16(h + 6);
    p->crc = le32(h + 8);
    if(p->w < 1 || p->w > COURSE_PIC_MAX_W || p->h < 1 || p->h > COURSE_PIC_MAX_H || (p->bpp != 1 && p->bpp != 2)
       || p->stride != (p->w * p->bpp + 7) / 8 || !inside(off + PIC_HDR, (uint32_t)p->h * p->stride, c->bmp.len))
        return bad(c, "a picture is damaged");
    return 0;
}

int course_pic_rows(Course *c, const CoursePic *p, uint16_t y0, uint16_t rows, uint8_t *buf, uint32_t cap){
    if(y0 >= p->h || rows > p->h - y0 || (uint32_t)rows * p->stride > cap) return COURSE_ERANGE;
    return rd(c, c->bmp.off + p->off + PIC_HDR + (uint32_t)y0 * p->stride, buf, (uint32_t)rows * p->stride);
}

int course_pic_check(Course *c, const CoursePic *p, uint8_t *scratch, uint32_t cap){
    uint32_t crc = 0;
    uint16_t per = p->stride ? (uint16_t)(cap / p->stride) : 0;
    if(!per) return COURSE_ERANGE;
    for(uint16_t y = 0; y < p->h; ){
        uint16_t n = p->h - y < per ? p->h - y : per;
        int r = course_pic_rows(c, p, y, n, scratch, cap);
        if(r) return r;
        crc = course_crc32(crc, scratch, (uint32_t)n * p->stride);
        y += n;
    }
    return crc == p->crc ? 0 : bad(c, "a picture is damaged");
}

/* ---- META on demand -------------------------------------------------------- */

int course_meta_entry(Course *c, uint32_t i, uint16_t *key, uint8_t *buf, uint32_t cap, uint16_t *len){
    uint32_t p = 0;
    uint8_t h[4];
    for(;;){
        if(p >= c->meta.len) return COURSE_ERANGE;
        if(c->meta.len - p < 4) return bad(c, "META is damaged");
        int r = rd(c, c->meta.off + p, h, 4);
        if(r) return r;
        uint16_t n = le16(h + 2);
        if(n > c->meta.len - p - 4) return bad(c, "META is damaged");
        if(!i--){
            *key = le16(h);
            *len = n;
            if(n > cap) return COURSE_ERANGE;
            return rd(c, c->meta.off + p + 4, buf, n);
        }
        p += 4 + n;
    }
}

int course_meta_text(Course *c, uint16_t key, char *buf, uint32_t cap){
    uint32_t p = 0;
    uint8_t h[4];
    if(!cap) return 0;
    buf[0] = 0;
    while(c->meta.len - p >= 4){
        if(rd(c, c->meta.off + p, h, 4)) return 0;
        uint16_t n = le16(h + 2);
        if(n > c->meta.len - p - 4) return 0;
        if(le16(h) == key){
            /* read one byte past the cut, when there is a cut, to see whether
             * it falls inside a character */
            uint32_t take = n < cap - 1 ? n : cap - 1;
            if(rd(c, c->meta.off + p + 4, buf, take < n ? take + 1 : take)) return 0;
            if(take < n) while(take && ((uint8_t)buf[take] & 0xC0) == 0x80) take--;
            buf[take] = 0;
            return (int)take;
        }
        p += 4 + n;
    }
    return 0;
}

/* ---- the whole file ---------------------------------------------------------- */

int course_verify(Course *c, uint8_t *scratch, uint32_t cap){
    if(cap < COURSE_VERIFY_BUF) return COURSE_ERANGE;
    uint8_t *pics = scratch + COURSE_ITEM_BUF;          /* picture rows go here */
    uint32_t pcap = cap - COURSE_ITEM_BUF;
    int r;
    uint32_t crc;
    /* every section's CRC, and zero padding between them */
    uint8_t h[HDR_SIZE], t[SECT_SIZE * 16];
    if((r = rd(c, 0, h, HDR_SIZE))) return r;
    uint16_t nsec = le16(h + 12);
    if(nsec < 5 || nsec > 16) return bad(c, "a bad section count");
    if((r = rd(c, HDR_SIZE, t, SECT_SIZE * nsec))) return r;
    if(course_crc32(0, t, SECT_SIZE * nsec) != c->fingerprint) return bad(c, "the section table changed");
    uint32_t end = HDR_SIZE + SECT_SIZE * nsec;
    for(uint16_t i = 0; i < nsec; i++){
        const uint8_t *e = t + SECT_SIZE * i;
        uint32_t off = le32(e + 4), len = le32(e + 8);
        if(off % 4 || off < end || !inside(off, len, c->size)) return bad(c, "section %u is out of place", i);
        if((r = zero_range(c, end, off - end))) return r;
        if((r = crc_range(c, off, len, &crc))) return r;
        if(crc != le32(e + 12)) return bad(c, "section %.4s is damaged", (const char *)e);
        end = off + len;
    }
    if(end != c->size) return bad(c, "there are bytes after the last section");

    /* levels, and every item in them */
    uint32_t n = 0;
    for(uint32_t i = 0; i < c->n_levels; i++){
        CourseLevel l;
        if((r = course_level(c, i, &l))) return r;
        if((r = course_level_load(c, &l, scratch, COURSE_ITEM_BUF)) < 0) return r;
        uint32_t pos = 0;
        CourseField f;
        while(course_field_next(scratch, (uint32_t)r, &pos, &f) == 1) if(f.attr & CF_A_PICTURE){
            CoursePic p;
            int q = course_pic(c, f.pic, &p);
            if(!q) q = course_pic_check(c, &p, pics, pcap);
            if(q) return q;
        }
        for(uint32_t k = 0; k < l.count; k++, n++){
            CourseItem it;
            if((r = course_item(c, n, &it))) return r;
            if(it.level != l.number) return bad(c, "item %u is in the wrong level", (unsigned)n);
            if((r = course_item_load(c, &it, scratch, COURSE_ITEM_BUF)) < 0) return r;
            pos = 0;
            while(course_field_next(scratch, it.text_len, &pos, &f) == 1) if(f.attr & CF_A_PICTURE){
                CoursePic p;
                int q = course_pic(c, f.pic, &p);
                if(!q) q = course_pic_check(c, &p, pics, pcap);
                if(q) return q;
            }
        }
    }
    /* the index names every item exactly once */
    for(uint32_t j = 0; j < c->n_items; j++){
        uint32_t id, k;
        CourseItem it;
        if((r = course_idx(c, j, &id, &k))) return r;
        if((r = course_item(c, k, &it))) return r;
        if(it.id != id) return bad(c, "the id index doesn't match the items");
    }
    return 0;
}
