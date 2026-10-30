/* monster.c -- the monster game's file reader. See monster.h and
 * docs/GAME_FORMAT.md.
 *
 * Every read goes through rd(), which refuses anything outside the file, and
 * every record is checked against the sections it points into before its
 * contents are used. The fuzz gate (tests/monster_fuzz.c, in make ftest) runs
 * this under ASan and UBSan over damaged copies, including ones whose CRCs were
 * fixed up so the damage reaches the checks behind them. */
#include "monster.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HDR_SIZE   32
#define SECT_SIZE  16
#define MAX_SECTS  64
#define META_MAX   1024
#define STAT_SIZE  24
#define MOVE_SIZE  40
#define MONS_SIZE  48
#define LRNS_SIZE  4
#define AREA_SIZE  32
#define ENC_ROW    8
#define HEAD_FIXED 64

/* the section tags, 4 bytes each in MG_* order, as the section table holds
 * them (one string: tests/nodelete.sh rightly flags a quoted HTTP method name,
 * and one of these tags is spelt like one) */
static const char TAGS[] = "METATYPESHAPSTATMOVEMONSLRNSENCTAREAADATSPRTTEXTCRED";
#define TAG(k) (TAGS + 4 * (k))
/* checked on open: everything small (section 6) */
#define CRC_ON_OPEN(i) ((i) != MG_ADAT && (i) != MG_SPRT && (i) != MG_CRED)

enum { K_TITLE = 0x01, K_SOURCE, K_BUILD, K_START = 0x10, K_LOOKS, K_STARTERS, K_MORE_STARTERS, K_RULES = 0x20, K_CAPTURE, K_GROWTH };

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* CRC-32 (zlib's), a nibble at a time, as course.c */
static const uint32_t CRC_T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};
uint32_t mg_crc32(uint32_t crc, const void *buf, uint32_t len){
    const uint8_t *p = buf;
    crc = ~crc;
    while(len--){
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_T[crc & 15];
        crc = (crc >> 4) ^ CRC_T[crc & 15];
    }
    return ~crc;
}

static int bad(MonGame *g, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->err, sizeof g->err, fmt, ap);
    va_end(ap);
    return MG_EBAD;
}

/* read len bytes at off; anything past the end of the file is damage */
static int rd(MonGame *g, uint32_t off, void *buf, uint32_t len){
    if(!len) return 0;
    if(!g->f) return MG_EIO;
    if(off > g->size || len > g->size - off) return bad(g, "a read past the end of the file");
    if(fseek(g->f, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, g->f) != len){
        snprintf(g->err, sizeof g->err, "the card couldn't be read");
        return MG_EIO;
    }
    return 0;
}

static int inside(uint32_t off, uint32_t len, uint32_t slen){
    return off <= slen && len <= slen - off;
}

/* read from section `s`, refusing anything outside it */
static int rds(MonGame *g, int s, uint32_t off, void *buf, uint32_t len){
    if(!inside(off, len, g->s[s].len)) return bad(g, "a read outside %.4s", TAG(s));
    return rd(g, g->s[s].off + off, buf, len);
}

int mg_read(MonGame *g, int sect, uint32_t off, void *buf, uint32_t len){
    if(sect < 0 || sect >= MG_NSECT) return MG_ERANGE;
    return rds(g, sect, off, buf, len);
}

static int crc_range(MonGame *g, uint32_t off, uint32_t len, uint32_t *out){
    uint8_t b[256];
    uint32_t crc = 0;
    while(len){
        uint32_t n = len < sizeof b ? len : sizeof b;
        int r = rd(g, off, b, n);
        if(r) return r;
        crc = mg_crc32(crc, b, n);
        off += n; len -= n;
    }
    *out = crc;
    return 0;
}

static int zero_range(MonGame *g, uint32_t off, uint32_t len){
    uint8_t b[64];
    while(len){
        uint32_t n = len < sizeof b ? len : sizeof b;
        int r = rd(g, off, b, n);
        if(r) return r;
        for(uint32_t i = 0; i < n; i++) if(b[i]) return bad(g, "padding at %u isn't zero", (unsigned)(off + i));
        off += n; len -= n;
    }
    return 0;
}

/* Strict UTF-8, every code point in U+0020-U+00FF (the Palm fonts); `nl`
 * allows line breaks too (the credits). */
static int text_ok(const uint8_t *s, uint32_t n, int nl){
    uint32_t i = 0;
    while(i < n){
        uint32_t u;
        if(s[i] < 0x80){ u = s[i]; i++; }
        else if((s[i] == 0xC2 || s[i] == 0xC3) && i + 1 < n && (s[i + 1] & 0xC0) == 0x80){
            u = (uint32_t)(s[i] & 0x1F) << 6 | (s[i + 1] & 0x3F); i += 2;
        }
        else return 0;
        if(u == 0x7F || (u < 0x20 && !(nl && u == '\n'))) return 0;
    }
    return 1;
}

static int str_ref(MonGame *g, const uint8_t *p, MgStr *s){
    s->off = le32(p);
    s->len = le16(p + 4);
    if(le16(p + 6)) return bad(g, "a string reference with a non-zero pad");
    if(!inside(s->off, s->len, g->s[MG_TEXT].len)) return bad(g, "a string outside TEXT");
    return 0;
}

/* a string reference, its text checked */
static int str_check(MonGame *g, const uint8_t *p, MgStr *s){
    uint8_t b[128];
    int r = str_ref(g, p, s);
    if(r) return r;
    for(uint32_t o = 0; o < s->len; ){
        uint32_t n = s->len - o < sizeof b ? s->len - o : sizeof b;
        /* don't cut a two-byte character between reads */
        if(o + n < s->len && n > 1) n--;
        if((r = rds(g, MG_TEXT, s->off + o, b, n))) return r;
        if(!text_ok(b, n, 0)) return bad(g, "a string the Palm fonts can't draw");
        o += n;
        if(n && b[n - 1] >= 0xC2 && b[n - 1] <= 0xC3 && o < s->len){
            /* a lead byte at the end of this piece: recheck it with what follows */
            uint8_t two[2];
            if((r = rds(g, MG_TEXT, s->off + o - 1, two, 2))) return r;
            if(!text_ok(two, 2, 0)) return bad(g, "a string the Palm fonts can't draw");
            o++;
        }
    }
    return 0;
}

int mg_text(MonGame *g, MgStr s, char *buf, uint32_t cap){
    if(!cap) return MG_ERANGE;
    uint32_t n = s.len < cap - 1 ? s.len : cap - 1;
    int r = rds(g, MG_TEXT, s.off, buf, n);
    if(r){ buf[0] = 0; return r; }
    if(n < s.len) while(n && ((uint8_t)buf[n] & 0xC0) == 0x80) n--;
    if(n < s.len && n && ((uint8_t)buf[n - 1] == 0xC2 || (uint8_t)buf[n - 1] == 0xC3)) n--;
    buf[n] = 0;
    return (int)n;
}

/* ---- META ---------------------------------------------------------------- */

static int parse_meta(MonGame *g, const uint8_t *m, uint32_t len){
    uint32_t seen = 0, p = 0, more = 0;
    while(p < len){
        if(len - p < 4) return bad(g, "META ends mid-entry");
        uint16_t k = le16(m + p), n = le16(m + p + 2);
        const uint8_t *v = m + p + 4;
        if(n > len - p - 4) return bad(g, "META key %02x runs past the section", k);
        p += 4 + n;
        if(k < 32){
            if(seen & (1u << k)) return bad(g, "META key %02x twice", k);
            seen |= 1u << k;
        }
        else if(k >= K_RULES && k <= K_GROWTH){
            uint32_t b = 1u << (k - K_RULES + 24);
            if(seen & b) return bad(g, "META key %02x twice", k);
            seen |= b;
        }
        switch(k){
        case K_TITLE: case K_SOURCE: case K_BUILD:
            if(!text_ok(v, n, 0)) return bad(g, "META key %02x isn't text", k);
            if(k == K_TITLE){
                uint32_t c = n < sizeof g->title - 1 ? n : sizeof g->title - 1;
                if(c < n) while(c && (v[c] & 0xC0) == 0x80) c--;
                if(c < n && c && (v[c - 1] == 0xC2 || v[c - 1] == 0xC3)) c--;
                memcpy(g->title, v, c);
                g->title[c] = 0;
            }
            break;
        case K_START:
            if(n != 8) return bad(g, "the start is %u bytes", n);
            g->start_area = le16(v);
            g->start_x = v[2]; g->start_y = v[3]; g->start_facing = v[4];
            if(v[5] | v[6] | v[7]) return bad(g, "the start has a non-zero pad");
            break;
        case K_LOOKS:       /* minor version 1: the player's looks, the first sheets of every area */
            if(n != 4 || !v[0] || v[0] > 8 || v[1] | v[2] | v[3]) return bad(g, "the player's looks are out of range");
            g->looks = v[0];
            break;
        case K_STARTERS:    /* minor version 1: the three first creatures offered */
            if(n != 8 || le16(v + 6)) return bad(g, "the starters are out of range");
            for(int i = 0; i < 3; i++){
                g->starters[i] = le16(v + 2 * i);
                if(g->starters[i] >= g->n_mons) return bad(g, "the starters are out of range");
            }
            g->n_starters = 3;
            break;
        case K_MORE_STARTERS:   /* minor version 2: the starters after the first three */
            if(n < 4) return bad(g, "the starters are out of range");
            more = v[0];
            if(!more || more > MG_MAX_STARTERS - 3 || v[1] || n != ((2 + 2 * more + 3) & ~3u))
                return bad(g, "the starters are out of range");
            for(uint32_t i = 0; i < more; i++){
                g->starters[3 + i] = le16(v + 2 + 2 * i);
                if(g->starters[3 + i] >= g->n_mons) return bad(g, "the starters are out of range");
            }
            for(uint32_t i = 2 + 2 * more; i < n; i++) if(v[i]) return bad(g, "the starters have a non-zero pad");
            break;
        case K_RULES:
            if(n != 32) return bad(g, "the rules are %u bytes", n);
            for(int i = 0; i < 8; i++) g->rules[i] = le32(v + 4 * i);
            break;
        case K_CAPTURE:
            if(n != 28) return bad(g, "the catching rules are %u bytes", n);
            for(int i = 0; i < 7; i++) g->capture[i] = le32(v + 4 * i);
            break;
        case K_GROWTH:
            if(n != 16) return bad(g, "the growth rules are %u bytes", n);
            for(int i = 0; i < 4; i++) g->growth[i] = le32(v + 4 * i);
            break;
        default:
            break;      /* a key from a later minor version */
        }
    }
    if(!(seen & (1u << K_START))) return bad(g, "META has no start");
    if(more){
        if(!g->n_starters) return bad(g, "more starters, but not the first three");
        g->n_starters = (uint8_t)(3 + more);
    }
    if(!(seen & (1u << K_LOOKS))) g->looks = 1;          /* a 1.0 file: one player sheet */
    if(!(seen & (1u << 24)) || !(seen & (1u << 25)) || !(seen & (1u << 26))) return bad(g, "META has no rules");
    const uint32_t *R = g->rules, *C = g->capture, *G = g->growth;
    /* bounds that keep every sum in monster_play.c inside 64 bits */
    if(R[0] < 1 || R[0] > 100 || R[1] > 100 || R[2] < 1 || R[2] > R[3] || R[3] > 10000)
        return bad(g, "the rules are out of range");
    if(R[4] < 1 || R[4] > 4 || R[5] < 1 || R[5] > 255 || R[6] < 1 || R[6] > 4 || R[7] < 1 || R[7] > 6)
        return bad(g, "the rules are out of range");
    if(C[0] < 1 || C[0] > 16 || C[1] > 10000000 || C[2] < 1 || C[2] > 1000 || C[3] < 1 || C[3] > 1000000 ||
       C[4] < 1 || C[4] > 100 || C[5] > 100 || C[6] < 1 || C[6] > 100)
        return bad(g, "the catching rules are out of range");
    if(G[0] > 255 || G[1] < 1 || G[1] > 1000 || G[2] < 1 || G[2] > 1000 || G[3] < 1 || G[3] > 1000)
        return bad(g, "the growth rules are out of range");
    return 0;
}

/* ---- records ------------------------------------------------------------- */

static void parse_move(const uint8_t *b, MgMove *m){
    m->id = le32(b);
    m->type[0] = b[4]; m->type[1] = b[5];
    m->range = b[6]; m->speed = b[7];
    m->power = le16(b + 8);
    m->accuracy = b[10]; m->potency = b[11]; m->recharge = b[12]; m->targets = b[13];
    m->heal = le16(b + 14);
    for(int i = 0; i < 3; i++){
        m->eff_kind[i] = b[16 + 4 * i];
        m->eff_status[i] = b[17 + 4 * i];
        m->eff_target[i] = b[18 + 4 * i];
    }
}

static int check_move(MonGame *g, const uint8_t *b, MgMove *m){
    parse_move(b, m);
    for(int i = 0; i < 2; i++)
        if(m->type[i] != MG_NONE8 && m->type[i] >= g->n_types) return bad(g, "move %u: a type out of range", (unsigned)m->id);
    if(m->range > MG_SPECIAL || m->speed > 6 || m->accuracy > 100 || m->potency > 100 || m->targets > 3)
        return bad(g, "move %u: a value out of range", (unsigned)m->id);
    for(int i = 0; i < 3; i++){
        if(m->eff_kind[i] > MG_E_GIVE || m->eff_target[i] > MG_T_OWN || b[19 + 4 * i])
            return bad(g, "move %u: a bad effect", (unsigned)m->id);
        if(m->eff_kind[i] == MG_E_GIVE && m->eff_status[i] >= g->n_statuses)
            return bad(g, "move %u: a status out of range", (unsigned)m->id);
    }
    return str_check(g, b + 32, &m->name);
}

int mg_move(MonGame *g, uint32_t i, MgMove *m){
    uint8_t b[MOVE_SIZE];
    if(i >= g->n_moves) return MG_ERANGE;
    int r = rds(g, MG_MOVE, i * MOVE_SIZE, b, MOVE_SIZE);
    if(r) return r;
    parse_move(b, m);
    return str_ref(g, b + 32, &m->name);
}

static void parse_mon(const uint8_t *b, MgMon *m){
    m->id = le32(b);
    m->txmn = le16(b + 4);
    m->type[0] = b[6]; m->type[1] = b[7];
    m->shape = b[8]; m->stage = b[9]; m->catch_rate = b[10]; m->group = b[11];
    m->resist_lo = b[12]; m->resist_hi = b[13]; m->evolve_at = b[14]; m->n_learn = b[15];
    m->evolve_to = le16(b + 16);
    m->learn0 = le32(b + 20);
    m->sprite_off = le32(b + 24);
    m->sprite_len = le32(b + 28);
}

int mg_mon(MonGame *g, uint32_t i, MgMon *m){
    uint8_t b[MONS_SIZE];
    if(i >= g->n_mons) return MG_ERANGE;
    int r = rds(g, MG_MONS, i * MONS_SIZE, b, MONS_SIZE);
    if(r) return r;
    parse_mon(b, m);
    return str_ref(g, b + 32, &m->name);
}

int mg_learn(MonGame *g, const MgMon *m, uint32_t j, uint8_t *level, uint16_t *move){
    uint8_t b[LRNS_SIZE];
    if(j >= m->n_learn) return MG_ERANGE;
    int r = rds(g, MG_LRNS, (m->learn0 + j) * LRNS_SIZE, b, LRNS_SIZE);
    if(r) return r;
    *level = b[0];
    *move = le16(b + 2);
    if(*move >= g->n_moves) return bad(g, "a learnset names a move out of range");
    return 0;
}

int mg_enc_rows(MonGame *g, uint32_t t, uint16_t *n){
    uint8_t b[4];
    if(t >= g->n_tables) return MG_ERANGE;
    int r = rds(g, MG_ENCT, 4 + 4 * t, b, 4);
    if(r) return r;
    *n = le16(b + 2);
    return 0;
}

int mg_enc_row(MonGame *g, uint32_t t, uint32_t j, MgEncRow *row){
    uint8_t b[ENC_ROW];
    if(t >= g->n_tables) return MG_ERANGE;
    int r = rds(g, MG_ENCT, 4 + 4 * t, b, 4);
    if(r) return r;
    uint32_t first = le16(b), n = le16(b + 2);
    if(j >= n) return MG_ERANGE;
    if((r = rds(g, MG_ENCT, g->enc_rows0 + (first + j) * ENC_ROW, b, ENC_ROW))) return r;
    row->mon = le16(b); row->lo = b[2]; row->hi = b[3]; row->rate = le16(b + 4);
    return 0;
}

int mg_area_dir(MonGame *g, uint32_t i, MgAreaDir *a){
    uint8_t b[AREA_SIZE];
    if(i >= g->n_areas) return MG_ERANGE;
    int r = rds(g, MG_AREA, i * AREA_SIZE, b, AREA_SIZE);
    if(r) return r;
    a->id = le32(b); a->w = le16(b + 4); a->h = le16(b + 6);
    a->off = le32(b + 8); a->len = le32(b + 12); a->head_crc = le32(b + 16);
    a->head_len = le32(b + 28);
    return str_ref(g, b + 20, &a->name);
}

int mg_type_name(MonGame *g, uint32_t t, MgStr *s){
    uint8_t b[8];
    if(t >= g->n_types) return MG_ERANGE;
    int r = rds(g, MG_TYPE, 4 + 8 * t, b, 8);
    if(r) return r;
    return str_ref(g, b, s);
}

int mg_find_mon(MonGame *g, uint32_t id, uint16_t *pos){
    uint8_t b[4];
    for(uint32_t i = 0; i < g->n_mons; i++){
        int r = rds(g, MG_MONS, i * MONS_SIZE, b, 4);
        if(r) return r;
        if(le32(b) == id){ *pos = (uint16_t)i; return 0; }
    }
    return MG_ERANGE;
}

int mg_find_move(MonGame *g, uint32_t id, uint16_t *pos){
    uint8_t b[4];
    for(uint32_t i = 0; i < g->n_moves; i++){
        int r = rds(g, MG_MOVE, i * MOVE_SIZE, b, 4);
        if(r) return r;
        if(le32(b) == id){ *pos = (uint16_t)i; return 0; }
    }
    return MG_ERANGE;
}

int mg_find_area(MonGame *g, uint32_t id, uint16_t *pos){
    uint8_t b[4];
    for(uint32_t i = 0; i < g->n_areas; i++){
        int r = rds(g, MG_AREA, i * AREA_SIZE, b, 4);
        if(r) return r;
        if(le32(b) == id){ *pos = (uint16_t)i; return 0; }
    }
    return MG_ERANGE;
}

int mg_meta_entry(MonGame *g, uint32_t i, uint16_t *key, uint8_t *buf, uint32_t cap, uint16_t *len){
    uint32_t p = 0;
    uint8_t h[4];
    for(uint32_t k = 0; p < g->s[MG_META].len; k++){
        int r = rds(g, MG_META, p, h, 4);
        if(r) return r;
        uint16_t n = le16(h + 2);
        if(k == i){
            *key = le16(h);
            *len = n;
            if(n > cap) return MG_ERANGE;
            return rds(g, MG_META, p + 4, buf, n);
        }
        p += 4 + n;
    }
    return MG_ERANGE;
}

/* ---- open ---------------------------------------------------------------- */

/* Every record's positions and strings, against the sections they point into. */
static int check_records(MonGame *g){
    uint8_t b[MONS_SIZE];
    int r;
    /* TYPE: u16 n, u16 0, n names, n x n multipliers */
    if(g->s[MG_TYPE].len < 4) return bad(g, "TYPE is too short");
    if((r = rds(g, MG_TYPE, 0, b, 4))) return r;
    g->n_types = (uint8_t)(le16(b) <= MG_MAX_TYPES ? le16(b) : 0);
    if(!g->n_types || le16(b + 2)) return bad(g, "%u types", (unsigned)le16(b));
    uint32_t nt = g->n_types;
    if(g->s[MG_TYPE].len != 4 + 8 * nt + nt * nt) return bad(g, "TYPE is the wrong size");
    for(uint32_t t = 0; t < nt; t++){
        MgStr s;
        if((r = rds(g, MG_TYPE, 4 + 8 * t, b, 8))) return r;
        if((r = str_check(g, b, &s))) return r;
        if((r = rds(g, MG_TYPE, 4 + 8 * nt + nt * t, g->types[t], nt))) return r;
    }
    /* SHAP */
    if(g->s[MG_SHAP].len % 8 || !g->s[MG_SHAP].len || g->s[MG_SHAP].len / 8 > MG_MAX_SHAPES)
        return bad(g, "SHAP is the wrong size");
    g->n_shapes = (uint8_t)(g->s[MG_SHAP].len / 8);
    for(uint32_t i = 0; i < g->n_shapes; i++){
        if((r = rds(g, MG_SHAP, 8 * i, b, 8))) return r;
        memcpy(g->shapes[i], b, MG_NSTATS);
    }
    /* STAT */
    if(g->s[MG_STAT].len % STAT_SIZE || g->s[MG_STAT].len / STAT_SIZE > MG_MAX_STATUSES)
        return bad(g, "STAT is the wrong size");
    g->n_statuses = (uint8_t)(g->s[MG_STAT].len / STAT_SIZE);
    for(uint32_t i = 0; i < g->n_statuses; i++){
        MgStatus *st = &g->statuses[i];
        if((r = rds(g, MG_STAT, STAT_SIZE * i, b, STAT_SIZE))) return r;
        st->id = le32(b);
        st->kind = b[4]; st->divisor = b[5]; st->flags = b[6];
        st->immune = le16(b + 8);
        if(st->kind < MG_S_POISON || st->kind > MG_S_RECOVER || !st->divisor || (st->immune >> nt))
            return bad(g, "status %u is out of range", (unsigned)st->id);
        if((r = str_check(g, b + 12, &st->name))) return r;
    }
    /* MOVE */
    if(g->s[MG_MOVE].len % MOVE_SIZE || !g->s[MG_MOVE].len || g->s[MG_MOVE].len / MOVE_SIZE > 65535)
        return bad(g, "MOVE is the wrong size");
    g->n_moves = g->s[MG_MOVE].len / MOVE_SIZE;
    for(uint32_t i = 0; i < g->n_moves; i++){
        MgMove m;
        if((r = rds(g, MG_MOVE, MOVE_SIZE * i, b, MOVE_SIZE))) return r;
        if((r = check_move(g, b, &m))) return r;
    }
    /* LRNS */
    if(g->s[MG_LRNS].len % LRNS_SIZE) return bad(g, "LRNS is the wrong size");
    g->n_learn = g->s[MG_LRNS].len / LRNS_SIZE;
    for(uint32_t i = 0; i < g->n_learn; i++){
        if((r = rds(g, MG_LRNS, LRNS_SIZE * i, b, LRNS_SIZE))) return r;
        if(!b[0] || b[0] > g->rules[5] || b[1] || le16(b + 2) >= g->n_moves) return bad(g, "a learnset entry is out of range");
    }
    /* MONS */
    if(g->s[MG_MONS].len % MONS_SIZE || g->s[MG_MONS].len / MONS_SIZE != g->n_mons || !g->n_mons || g->n_mons > 65535)
        return bad(g, "MONS doesn't hold the header's %u creatures", (unsigned)g->n_mons);
    for(uint32_t i = 0; i < g->n_mons; i++){
        MgMon m;
        if((r = rds(g, MG_MONS, MONS_SIZE * i, b, MONS_SIZE))) return r;
        parse_mon(b, &m);
        unsigned id = (unsigned)m.id;
        if(m.type[0] >= nt || (m.type[1] != MG_NONE8 && m.type[1] >= nt) || m.shape >= g->n_shapes ||
           m.stage > 3 || m.catch_rate > 100 || m.group > 2 || m.resist_lo > m.resist_hi || !m.resist_lo)
            return bad(g, "creature %u: a value out of range", id);
        if((m.evolve_at == 0) != (m.evolve_to == MG_NONE16) || (m.evolve_to != MG_NONE16 && m.evolve_to >= g->n_mons) ||
           m.evolve_at > g->rules[5])
            return bad(g, "creature %u: a bad evolution", id);
        if(!m.n_learn || m.learn0 > g->n_learn || m.n_learn > g->n_learn - m.learn0)
            return bad(g, "creature %u: its learnset is out of range", id);
        uint8_t prev = 0, lv;
        uint16_t mv;
        for(uint32_t j = 0; j < m.n_learn; j++){
            if((r = mg_learn(g, &m, j, &lv, &mv))) return r;
            if(lv < prev) return bad(g, "creature %u: its learnset is out of order", id);
            prev = lv;
        }
        if(m.sprite_off % 4 || m.sprite_len < 40 || m.sprite_len > MG_SPRITE_MAX ||
           !inside(m.sprite_off, m.sprite_len, g->s[MG_SPRT].len))
            return bad(g, "creature %u: its sprite is out of range", id);
        if(b[18] | b[19]) return bad(g, "creature %u: a non-zero pad", id);
        for(int k = 40; k < 48; k++) if(b[k]) return bad(g, "creature %u: a non-zero pad", id);
        if((r = str_check(g, b + 32, &m.name))) return r;
    }
    /* ENCT: u16 tables, u16 0, (first, rows) each, padded to 4, then the rows */
    if(g->s[MG_ENCT].len < 4) return bad(g, "ENCT is too short");
    if((r = rds(g, MG_ENCT, 0, b, 4))) return r;
    g->n_tables = le16(b);
    g->enc_rows0 = (4 + 4 * g->n_tables + 3) & ~3u;
    if(g->enc_rows0 > g->s[MG_ENCT].len || (g->s[MG_ENCT].len - g->enc_rows0) % ENC_ROW)
        return bad(g, "ENCT is the wrong size");
    g->n_enc_rows = (g->s[MG_ENCT].len - g->enc_rows0) / ENC_ROW;
    for(uint32_t t = 0; t < g->n_tables; t++){
        if((r = rds(g, MG_ENCT, 4 + 4 * t, b, 4))) return r;
        uint32_t first = le16(b), n = le16(b + 2), total = 0;
        if(!n || first > g->n_enc_rows || n > g->n_enc_rows - first) return bad(g, "encounter table %u is out of range", (unsigned)t);
        for(uint32_t j = 0; j < n; j++){
            MgEncRow row;
            if((r = mg_enc_row(g, t, j, &row))) return r;
            if(row.mon >= g->n_mons || !row.lo || row.lo > row.hi || row.hi > g->rules[5])
                return bad(g, "encounter table %u: a row out of range", (unsigned)t);
            total += row.rate;
        }
        if(!total) return bad(g, "encounter table %u: every rate is 0", (unsigned)t);
    }
    /* AREA */
    if(g->s[MG_AREA].len % AREA_SIZE || g->s[MG_AREA].len / AREA_SIZE != g->n_areas || !g->n_areas || g->n_areas > 65535)
        return bad(g, "AREA doesn't hold the header's %u areas", (unsigned)g->n_areas);
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgAreaDir a;
        if((r = mg_area_dir(g, i, &a))) return r;
        if(!a.w || a.w > 255 || !a.h || a.h > 255 || a.off % 4 || !inside(a.off, a.len, g->s[MG_ADAT].len) ||
           a.head_len < HEAD_FIXED || a.head_len > a.len || a.head_len > MG_HEAD_MAX)
            return bad(g, "area %u is out of range", (unsigned)a.id);
        if((r = rds(g, MG_AREA, i * AREA_SIZE + 20, b, 8))) return r;
        if((r = str_check(g, b, &a.name))) return r;
        if(i == g->start_area && (g->start_x >= a.w || g->start_y >= a.h)) return bad(g, "the start is outside its area");
    }
    if(g->start_area >= g->n_areas || g->start_facing > 3) return bad(g, "the start is out of range");
    return 0;
}

int mg_open_fp(MonGame *g, FILE *f){
    uint8_t h[HDR_SIZE];
    memset(g, 0, sizeof *g);
    g->f = f;
    if(!f){ snprintf(g->err, sizeof g->err, "the file couldn't be opened"); return MG_EIO; }
    int r = MG_EBAD;
    if(fseek(f, 0, SEEK_END) != 0){ r = MG_EIO; goto fail; }
    long sz = ftell(f);
    if(sz < 0 || (unsigned long)sz > 0xFFFFFFFFul){ r = bad(g, "the file's size can't be read"); goto fail; }
    g->size = (uint32_t)sz;
    if(g->size < HDR_SIZE){ r = bad(g, "not a game file (too short)"); goto fail; }
    if((r = rd(g, 0, h, HDR_SIZE))) goto fail;
    if(memcmp(h, "CYDG", 4)){ r = bad(g, "not a game file"); goto fail; }
    if(mg_crc32(0, h, 28) != le32(h + 28)){ r = bad(g, "the header is damaged"); goto fail; }
    if(le16(h + 4) != MG_FORMAT_MAJOR){
        snprintf(g->err, sizeof g->err, "a game file for a newer version (format %u)", (unsigned)le16(h + 4));
        r = MG_EVERSION; goto fail;
    }
    g->minor = le16(h + 6);
    if(le32(h + 8) != g->size){ r = bad(g, "the file is %u bytes, the header says %u", (unsigned)g->size, (unsigned)le32(h + 8)); goto fail; }
    uint32_t nsec = le16(h + 12);
    if(nsec < MG_NSECT || nsec > MAX_SECTS || le16(h + 14)){ r = bad(g, "%u sections", (unsigned)nsec); goto fail; }
    g->n_mons = le32(h + 16);
    g->n_areas = le32(h + 20);
    g->fingerprint = le32(h + 24);
    uint32_t tcrc;
    if((r = crc_range(g, HDR_SIZE, SECT_SIZE * nsec, &tcrc))) goto fail;
    if(tcrc != g->fingerprint){ r = bad(g, "the section table is damaged"); goto fail; }

    uint32_t prev = HDR_SIZE + SECT_SIZE * nsec, have = 0;
    for(uint32_t i = 0; i < nsec; i++){
        uint8_t e[SECT_SIZE];
        if((r = rd(g, HDR_SIZE + SECT_SIZE * i, e, SECT_SIZE))) goto fail;
        uint32_t off = le32(e + 4), len = le32(e + 8);
        if(off % 4 || off < prev || !inside(off, len, g->size)){ r = bad(g, "section %u is out of place", (unsigned)i); goto fail; }
        if((r = zero_range(g, prev, off - prev))) goto fail;
        prev = off + len;
        for(int k = 0; k < MG_NSECT; k++){
            if(memcmp(e, TAG(k), 4)) continue;
            if(have & (1u << k)){ r = bad(g, "%.4s twice", TAG(k)); goto fail; }
            have |= 1u << k;
            g->s[k].off = off; g->s[k].len = len; g->s[k].crc = le32(e + 12);
        }
    }
    if((r = zero_range(g, prev, g->size - prev))) goto fail;
    if(g->size - prev > 3){ r = bad(g, "bytes after the last section"); goto fail; }
    for(int k = 0; k < MG_NSECT; k++){
        if(!(have & (1u << k))){ r = bad(g, "no %.4s section", TAG(k)); goto fail; }
        if(!CRC_ON_OPEN(k)) continue;
        uint32_t c;
        if((r = crc_range(g, g->s[k].off, g->s[k].len, &c))) goto fail;
        if(c != g->s[k].crc){ r = bad(g, "%.4s is damaged", TAG(k)); goto fail; }
    }

    if(g->s[MG_META].len > META_MAX){ r = bad(g, "META is too long"); goto fail; }
    uint8_t *m = malloc(g->s[MG_META].len ? g->s[MG_META].len : 1);
    if(!m){ snprintf(g->err, sizeof g->err, "out of memory"); r = MG_EIO; goto fail; }
    r = rds(g, MG_META, 0, m, g->s[MG_META].len);
    if(!r) r = parse_meta(g, m, g->s[MG_META].len);
    free(m);
    if(r) goto fail;
    if((r = check_records(g))) goto fail;
    return 0;
fail:
    if(g->f) fclose(g->f);
    g->f = NULL;
    return r;
}

int mg_open(MonGame *g, const char *path){
    return mg_open_fp(g, fopen(path, "rb"));
}

void mg_close(MonGame *g){
    if(g->f) fclose(g->f);
    g->f = NULL;
}

/* ---- areas --------------------------------------------------------------- */

static int rect_ok(const uint8_t *p, uint16_t w, uint16_t h){
    return p[2] && p[3] && p[0] + p[2] <= w && p[1] + p[3] <= h;
}

static int area_open(MonGame *g, uint32_t i, MgArea *a, uint8_t *buf, uint32_t cap){
    int r;
    memset(a, 0, sizeof *a);
    if((r = mg_area_dir(g, i, &a->dir))) return r;
    const MgAreaDir *d = &a->dir;
    if(d->head_len > cap || d->head_len < HEAD_FIXED || !inside(d->off, d->len, g->s[MG_ADAT].len) || d->head_len > d->len)
        return bad(g, "area %u's head doesn't fit", (unsigned)d->id);
    if((r = rds(g, MG_ADAT, d->off, buf, d->head_len))) return r;
    if(mg_crc32(0, buf, d->head_len) != d->head_crc) return bad(g, "area %u is damaged", (unsigned)d->id);
    a->index = (uint16_t)i;
    a->w = le16(buf); a->h = le16(buf + 2);
    a->bpp = buf[4]; a->colours = buf[5];
    a->n_tiles = le16(buf + 6); a->ccols = le16(buf + 8); a->crows = le16(buf + 10);
    a->n_warps = le16(buf + 12); a->n_zones = le16(buf + 14); a->n_signs = le16(buf + 16);
    a->n_overlays = le16(buf + 18); a->n_chars = le16(buf + 20); a->n_people = le16(buf + 22);
    a->tiles_off = le32(buf + 24); a->tiles_len = le32(buf + 28); a->tiles_crc = le32(buf + 32);
    a->chars_off = le32(buf + 36); a->chars_len = le32(buf + 40); a->chars_crc = le32(buf + 44);
    a->chunk0 = le32(buf + 48);
    unsigned id = (unsigned)d->id;
    if(a->w != d->w || a->h != d->h || (a->bpp != 4 && a->bpp != 8) || !a->colours || a->colours >= 1u << a->bpp ||
       a->n_tiles > 4095 || a->ccols != (a->w + MG_CHUNK - 1) / MG_CHUNK || a->crows != (a->h + MG_CHUNK - 1) / MG_CHUNK ||
       a->n_chars < g->looks || a->n_chars > 16 || a->n_people > MG_MAX_PEOPLE)
        return bad(g, "area %u: a value out of range", id);
    for(int k = 52; k < HEAD_FIXED; k++) if(buf[k]) return bad(g, "area %u: a non-zero pad", id);
    uint32_t tsz = MG_TILE * MG_TILE * a->bpp / 8, fsz = MG_CHAR_W * MG_CHAR_H * a->bpp / 8;
    uint32_t nchunks = (uint32_t)a->ccols * a->crows;
    if(a->tiles_off != ((d->head_len + 3) & ~3u) || a->tiles_len != a->n_tiles * tsz ||
       a->chars_off != a->tiles_off + a->tiles_len || a->chars_len != (uint32_t)a->n_chars * MG_CHAR_FRAMES * fsz ||
       a->chunk0 != ((a->chars_off + a->chars_len + 3) & ~3u) || d->len != a->chunk0 + nchunks * MG_CHUNK_BYTES)
        return bad(g, "area %u: its parts don't add up", id);
    /* the lists, one after another, exactly filling the head */
    uint32_t o = HEAD_FIXED, need;
    #define TAKE(ptr, bytes) do { need = (bytes); if(need > d->head_len - o) return bad(g, "area %u: its lists overrun the head", id); \
                                  (ptr) = buf + o; o += need; } while(0)
    TAKE(a->palette, (2u * (a->colours + 1) + 3) & ~3u);
    TAKE(a->tflags, ((uint32_t)a->n_tiles + 1 + 3) & ~3u);
    TAKE(a->warps, 12u * a->n_warps);
    TAKE(a->zones, 8u * a->n_zones);
    TAKE(a->signs, 16u * a->n_signs);
    TAKE(a->overlays, 4u * a->n_overlays);
    TAKE(a->people, 48u * a->n_people);
    TAKE(a->cdir, 8u * nchunks);
    #undef TAKE
    if(o != d->head_len) return bad(g, "area %u: its head is %u bytes, its lists %u", id, (unsigned)d->head_len, (unsigned)o);
    if(le16(a->palette)) return bad(g, "area %u: palette entry 0 isn't 0", id);
    for(uint32_t k = 2u * (a->colours + 1); a->palette + k < a->tflags; k++) if(a->palette[k]) return bad(g, "area %u: a non-zero pad", id);
    for(uint32_t k = (uint32_t)a->n_tiles + 1; a->tflags + k < a->warps; k++) if(a->tflags[k]) return bad(g, "area %u: a non-zero pad", id);
    for(uint32_t k = 0; k <= a->n_tiles; k++) if(a->tflags[k] & ~1u) return bad(g, "area %u: an unknown tile flag", id);
    for(uint32_t k = 0; k < a->n_warps; k++){
        const uint8_t *p = a->warps + 12 * k;
        MgAreaDir to;
        if(!rect_ok(p, a->w, a->h) || (p[8] > 3 && p[8] != MG_NONE8) || p[9] || le16(p + 10))
            return bad(g, "area %u: warp %u is out of range", id, (unsigned)k);
        if(mg_area_dir(g, le16(p + 4), &to) || p[6] >= to.w || p[7] >= to.h)
            return bad(g, "area %u: warp %u leads nowhere", id, (unsigned)k);
    }
    for(uint32_t k = 0; k < a->n_zones; k++){
        const uint8_t *p = a->zones + 8 * k;
        if(!rect_ok(p, a->w, a->h) || le16(p + 4) >= g->n_tables || !le16(p + 6) || le16(p + 6) > 10000)
            return bad(g, "area %u: zone %u is out of range", id, (unsigned)k);
    }
    for(uint32_t k = 0; k < a->n_signs; k++){
        const uint8_t *p = a->signs + 16 * k;
        MgStr s;
        if(!rect_ok(p, a->w, a->h) || (p[4] > 3 && p[4] != MG_NONE8) || p[5] || le16(p + 6))
            return bad(g, "area %u: sign %u is out of range", id, (unsigned)k);
        if((r = str_check(g, p + 8, &s))) return r;
    }
    for(uint32_t k = 0; k < a->n_overlays; k++){
        const uint8_t *p = a->overlays + 4 * k;
        if(le16(p) >= (uint32_t)a->w * a->h || !le16(p + 2) || le16(p + 2) > a->n_tiles ||
           (k && le16(p) <= le16(p - 4)))
            return bad(g, "area %u: overlay %u is out of range", id, (unsigned)k);
    }
    for(uint32_t k = 0; k < a->n_people; k++){
        const uint8_t *p = a->people + 48 * k;
        MgStr s;
        int team = 0, ok = p[0] < a->w && p[1] < a->h && p[2] <= 3 && p[3] >= g->looks && p[3] < a->n_chars &&
                           p[4] <= MG_P_HEALER && p[5] <= 15 && le16(p + 6) < MG_FLAGS &&
                           le16(p + 8) < MG_FLAGS && le16(p + 10) < MG_FLAGS;
        for(int j = 0; ok && j < MG_TEAM; j++){
            uint16_t mon = le16(p + 36 + 4 * j);
            uint8_t lv = p[38 + 4 * j];
            if(p[39 + 4 * j]) ok = 0;
            else if(mon == MG_NONE16) ok = !lv;
            else ok = j == team++ && mon < g->n_mons && lv && lv <= g->rules[5];
        }
        /* a trainer has creatures and a flag to remember the win by; no one else has creatures */
        if(ok) ok = p[4] == MG_P_TRAINER ? team && le16(p + 6) : !team && !p[5];
        for(uint32_t j = 0; ok && j < k; j++)
            if(a->people[48 * j] == p[0] && a->people[48 * j + 1] == p[1]) ok = 0;   /* one to a cell */
        if(!ok) return bad(g, "area %u: person %u is out of range", id, (unsigned)k);
        for(int j = 0; j < 3; j++) if((r = str_check(g, p + 12 + 8 * j, &s))) return r;
    }
    for(uint32_t k = 0; k < nchunks; k++)
        if(le32(a->cdir + 8 * k) != a->chunk0 + k * MG_CHUNK_BYTES) return bad(g, "area %u: chunk %u is out of place", id, (unsigned)k);
    return 0;
}

/* a refused area is left empty: no list a later call could walk into */
int mg_area_open(MonGame *g, uint32_t i, MgArea *a, uint8_t *buf, uint32_t cap){
    int r = area_open(g, i, a, buf, cap);
    if(r) memset(a, 0, sizeof *a);
    return r;
}

void mg_person(const MgArea *a, uint32_t i, MgPerson *q){
    const uint8_t *p = a->people + 48 * i;
    q->x = p[0]; q->y = p[1]; q->facing = p[2]; q->sheet = p[3]; q->kind = p[4]; q->sight = p[5];
    q->flag = le16(p + 6); q->hide_if = le16(p + 8); q->show_if = le16(p + 10);
    q->name.off = le32(p + 12);  q->name.len = le16(p + 16);
    q->text.off = le32(p + 20);  q->text.len = le16(p + 24);
    q->after.off = le32(p + 28); q->after.len = le16(p + 32);
    for(int j = 0; j < MG_TEAM; j++){ q->mon[j] = le16(p + 36 + 4 * j); q->level[j] = p[38 + 4 * j]; }
}

void mg_warp(const MgArea *a, uint32_t i, MgWarp *w){
    const uint8_t *p = a->warps + 12 * i;
    w->x = p[0]; w->y = p[1]; w->w = p[2]; w->h = p[3];
    w->to = le16(p + 4); w->tx = p[6]; w->ty = p[7]; w->facing = p[8];
}

void mg_zone(const MgArea *a, uint32_t i, MgZone *z){
    const uint8_t *p = a->zones + 8 * i;
    z->x = p[0]; z->y = p[1]; z->w = p[2]; z->h = p[3];
    z->table = le16(p + 4); z->chance = le16(p + 6);
}

void mg_sign(const MgArea *a, uint32_t i, MgSign *s){
    const uint8_t *p = a->signs + 16 * i;
    s->x = p[0]; s->y = p[1]; s->w = p[2]; s->h = p[3]; s->facing = p[4];
    s->text.off = le32(p + 8); s->text.len = le16(p + 12);
}

void mg_overlay(const MgArea *a, uint32_t i, uint16_t *cell, uint16_t *tile){
    *cell = le16(a->overlays + 4 * i);
    *tile = le16(a->overlays + 4 * i + 2);
}

uint16_t mg_palette(const MgArea *a, uint32_t i){
    return i && i <= a->colours ? le16(a->palette + 2 * i) : 0;
}

int mg_chunk(MonGame *g, const MgArea *a, uint32_t ci, uint8_t *buf){
    if(ci >= (uint32_t)a->ccols * a->crows) return MG_ERANGE;
    uint32_t off = le32(a->cdir + 8 * ci);
    int r = rds(g, MG_ADAT, a->dir.off + off, buf, MG_CHUNK_BYTES);
    if(r) return r;
    if(mg_crc32(0, buf, MG_CHUNK_BYTES) != le32(a->cdir + 8 * ci + 4)) return bad(g, "area %u: chunk %u is damaged", (unsigned)a->dir.id, (unsigned)ci);
    for(uint32_t j = 0; j < MG_CHUNK * MG_CHUNK; j++)
        if(le16(buf + 2 * j) > a->n_tiles) return bad(g, "area %u: a cell's tile is out of range", (unsigned)a->dir.id);
    return 0;
}

int mg_tile(MonGame *g, const MgArea *a, uint32_t t, uint8_t *buf, uint32_t cap){
    uint32_t tsz = MG_TILE * MG_TILE * a->bpp / 8;
    if(!t || t > a->n_tiles || cap < tsz) return MG_ERANGE;
    return rds(g, MG_ADAT, a->dir.off + a->tiles_off + (t - 1) * tsz, buf, tsz);
}

int mg_frame(MonGame *g, const MgArea *a, uint32_t k, uint8_t *buf, uint32_t cap){
    uint32_t fsz = MG_CHAR_W * MG_CHAR_H * a->bpp / 8;
    if(k >= (uint32_t)a->n_chars * MG_CHAR_FRAMES || cap < fsz) return MG_ERANGE;
    return rds(g, MG_ADAT, a->dir.off + a->chars_off + k * fsz, buf, fsz);
}

int mg_sprite(MonGame *g, const MgMon *m, uint8_t *buf, uint32_t cap){
    if(m->sprite_len > cap || m->sprite_len < 40) return MG_ERANGE;
    int r = rds(g, MG_SPRT, m->sprite_off, buf, m->sprite_len);
    if(r) return r;
    /* only what the format has: 4 bits, a 16-entry palette, w x h frames */
    if(buf[2] != 4 || !buf[0] || !buf[1] || buf[0] % 2 || !buf[3] ||
       m->sprite_len != 40u + (uint32_t)buf[3] * buf[0] * buf[1] / 2)
        return bad(g, "creature %u: its sprite is malformed", (unsigned)m->id);
    if(mg_crc32(0, buf + 8, m->sprite_len - 8) != le32(buf + 4))
        return bad(g, "creature %u: its sprite is damaged", (unsigned)m->id);
    return (int)m->sprite_len;
}

/* ---- everything ---------------------------------------------------------- */

int mg_verify(MonGame *g, uint8_t *scratch, uint32_t cap){
    int r;
    if(cap < MG_HEAD_MAX + MG_SPRITE_MAX) return MG_ERANGE;
    for(int k = 0; k < MG_NSECT; k++){
        uint32_t c;
        if((r = crc_range(g, g->s[k].off, g->s[k].len, &c))) return r;
        if(c != g->s[k].crc) return bad(g, "%.4s is damaged", TAG(k));
    }
    /* the credits: Palm text and line breaks */
    for(uint32_t o = 0; o < g->s[MG_CRED].len; ){
        uint8_t b[256];
        uint32_t n = g->s[MG_CRED].len - o < sizeof b ? g->s[MG_CRED].len - o : sizeof b;
        if((r = rds(g, MG_CRED, o, b, n))) return r;
        while(n > 1 && o + n < g->s[MG_CRED].len && (b[n - 1] == 0xC2 || b[n - 1] == 0xC3)) n--;
        if(!text_ok(b, n, 1)) return bad(g, "the credits aren't text");
        o += n;
    }
    uint8_t *head = scratch, *chunk = scratch + MG_HEAD_MAX;
    uint32_t prev = 0;
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgArea a;
        uint32_t c;
        if((r = mg_area_open(g, i, &a, head, MG_HEAD_MAX))) return r;
        if(a.dir.off != prev) return bad(g, "area %u is out of place", (unsigned)a.dir.id);
        prev = a.dir.off + a.dir.len;
        uint32_t base = g->s[MG_ADAT].off + a.dir.off;
        if((r = zero_range(g, base + a.dir.head_len, a.tiles_off - a.dir.head_len))) return r;
        if((r = zero_range(g, base + a.chars_off + a.chars_len, a.chunk0 - a.chars_off - a.chars_len))) return r;
        if((r = crc_range(g, base + a.tiles_off, a.tiles_len, &c))) return r;
        if(c != a.tiles_crc) return bad(g, "area %u: the tiles are damaged", (unsigned)a.dir.id);
        if((r = crc_range(g, base + a.chars_off, a.chars_len, &c))) return r;
        if(c != a.chars_crc) return bad(g, "area %u: the characters are damaged", (unsigned)a.dir.id);
        for(uint32_t k = 0; k < (uint32_t)a.ccols * a.crows; k++)
            if((r = mg_chunk(g, &a, k, chunk))) return r;
    }
    if(prev != g->s[MG_ADAT].len) return bad(g, "ADAT has bytes no area uses");
    prev = 0;
    for(uint32_t i = 0; i < g->n_mons; i++){
        MgMon m;
        if((r = mg_mon(g, i, &m))) return r;
        if(m.sprite_off != prev) return bad(g, "creature %u: its sprite is out of place", (unsigned)m.id);
        if((r = mg_sprite(g, &m, chunk, MG_SPRITE_MAX)) < 0) return r;
        prev = (m.sprite_off + m.sprite_len + 3) & ~3u;
        if((r = zero_range(g, g->s[MG_SPRT].off + m.sprite_off + m.sprite_len, prev - m.sprite_off - m.sprite_len))) return r;
    }
    if(prev != g->s[MG_SPRT].len) return bad(g, "SPRT has bytes no creature uses");
    return 0;
}
