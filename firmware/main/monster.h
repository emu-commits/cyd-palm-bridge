/* monster.h -- read the monster game's data file (game.bin), a piece at a time.
 *
 * The format is docs/GAME_FORMAT.md; tools/mkgame.py writes it. Like course.c,
 * this reader is pure C and stdio: no LVGL, no ESP-IDF, no statics, so it runs
 * the same on the device, in the simulator and in the host gates
 * (make -C sim monster; the fuzz gate in make ftest).
 *
 * THE FILE IS UNTRUSTED. Every offset, length and position is checked before
 * it's used, and a damaged or hostile file gets an error and a message in
 * g->err, never a crash or a read outside a buffer (GAME_FORMAT.md section 6):
 *   on open      the header, the section table, the CRCs of every section but
 *                ADAT, SPRT and CRED, and every record's positions and strings;
 *   on an area   the head's CRC, that its lists fill it exactly, every warp,
 *                zone, sign, overlay and person; each chunk's CRC and cells
 *                when read;
 *   a sprite     its record's CRC, when read;
 *   mg_verify()  all of that for every area and sprite, plus the CRCs of the
 *                tiles, the character sheets, ADAT, SPRT and CRED.
 *
 * MEMORY: a MonGame is about 1.6 KB (the type table and the shapes live in
 * it); records are read into the caller's structs as they're needed. An
 * area's head is read whole into the caller's buffer (MG_HEAD_MAX is the
 * most this reader accepts), a chunk into MG_CHUNK_BYTES, a tile into 256 B.
 */
#ifndef MONSTER_H
#define MONSTER_H

#include <stdint.h>
#include <stdio.h>

#define MG_FORMAT_MAJOR 2                   /* 2.0: the player's looks once, in LOOK */
#define MG_MAX_TYPES    16
#define MG_MAX_SHAPES   32
#define MG_MAX_STATUSES 8
#define MG_MAX_STARTERS 8                   /* META 0x12's three and 0x13's up to five */
#define MG_MAX_STEPS    32                  /* META 0x14: encounter tables that change with a flag */
#define MG_HEAD_MAX     16384
#define MG_CHUNK        16                  /* cells per chunk side */
#define MG_CHUNK_BYTES  (MG_CHUNK * MG_CHUNK * 3)
#define MG_TILE         16
#define MG_CHAR_W       16
#define MG_CHAR_H       32
#define MG_CHAR_FRAMES  12
#define MG_SPRITE_MAX   (40 + 2048 * 2)     /* a 64 x 64, 4-bit, two-frame record */
#define MG_NONE8        0xFF
#define MG_NONE16       0xFFFF
#define MG_MAX_PEOPLE   32                  /* in one area */
#define MG_FLAGS        256                 /* the story's flags are 1 .. MG_FLAGS - 1 */
#define MG_TEAM         3                   /* a trainer's creatures */

/* results: 0 is success; these are all < 0 */
#define MG_EIO      (-1)    /* the card couldn't be read */
#define MG_EBAD     (-2)    /* the file is damaged or not a game file */
#define MG_EVERSION (-3)    /* a file for a newer game */
#define MG_ERANGE   (-5)    /* the caller asked for something out of range */

enum { MG_META, MG_TYPE, MG_SHAP, MG_STAT, MG_MOVE, MG_MONS, MG_LRNS, MG_ENCT, MG_AREA,
       MG_ADAT, MG_SPRT, MG_TEXT, MG_CRED, MG_LOOK, MG_NSECT };

/* move ranges (GAME_FORMAT.md 3.6) */
enum { MG_MELEE, MG_TOUCH, MG_RANGED, MG_REACH, MG_RELIABLE, MG_SPECIAL };
/* effect kinds, and their targets */
enum { MG_E_NONE, MG_E_DAMAGE, MG_E_HEALING, MG_E_GIVE };
enum { MG_T_ENEMY, MG_T_OWN };
/* status kinds */
enum { MG_S_POISON = 1, MG_S_BURN, MG_S_RECOVER };
/* stats, in the order SHAP stores them */
enum { MG_HP, MG_ARMOUR, MG_DODGE, MG_MELEE_ST, MG_RANGED_ST, MG_SPEED, MG_NSTATS };
/* facing: N E S W */
enum { MG_N, MG_E, MG_S, MG_W };
/* what a person does when spoken to (GAME_FORMAT.md 3.11, minor version 1) */
enum { MG_P_TALK, MG_P_TRAINER, MG_P_HEALER };

typedef struct { uint32_t off, len, crc; } MgSect;
typedef struct { uint32_t off; uint16_t len; } MgStr;      /* a string reference into TEXT */

typedef struct {
    uint32_t id;
    uint8_t  kind, divisor, flags;          /* flags: 1 negative, 2 persists after a battle */
    uint16_t immune;                        /* bit t: type t is immune */
    MgStr    name;
} MgStatus;

typedef struct {
    uint32_t id;
    uint8_t  type[2];                       /* MG_NONE8 for none */
    uint8_t  range, speed;                  /* speed 0..6, 3 normal */
    uint16_t power;                         /* hundredths */
    uint8_t  accuracy, potency, recharge, targets;
    uint16_t heal;                          /* hundredths */
    uint8_t  eff_kind[3], eff_status[3], eff_target[3];
    MgStr    name;
} MgMove;

typedef struct {
    uint32_t id;
    uint16_t txmn;
    uint8_t  type[2], shape, stage, catch_rate, group, resist_lo, resist_hi, evolve_at, n_learn;
    uint16_t evolve_to;                     /* MG_NONE16 for none */
    uint32_t learn0, sprite_off, sprite_len;
    MgStr    name;
} MgMon;

typedef struct { uint16_t mon; uint8_t lo, hi; uint16_t rate; } MgEncRow;

typedef struct {
    uint32_t id;
    uint16_t w, h;
    uint32_t off, len, head_crc, head_len;
    MgStr    name;
} MgAreaDir;

typedef struct { uint8_t x, y, w, h; uint16_t to; uint8_t tx, ty, facing; } MgWarp;
typedef struct { uint8_t x, y, w, h; uint16_t table, chance; } MgZone;  /* chance: 1/100 % */
typedef struct { uint8_t x, y, w, h, facing; MgStr text; } MgSign;
/* A person standing in an area. Flags are numbered 1 .. MG_FLAGS - 1; 0 is none.
 * Present while show_if is 0 or set, and hide_if is 0 or clear. */
typedef struct {
    uint8_t  x, y, facing, sheet, kind, sight;   /* sheet: the area's character sheet, after the looks */
    uint16_t flag, hide_if, show_if;             /* flag: set once beaten (a trainer) or spoken to */
    MgStr    name, text, after;                  /* after: what's said once flag is set (may be empty) */
    uint16_t mon[MG_TEAM];                       /* a trainer's creatures; MG_NONE16 past the last */
    uint8_t  level[MG_TEAM];
} MgPerson;

/* An area that's been entered: its head lives in the caller's buffer. */
typedef struct {
    uint16_t index;                         /* its position in AREA */
    MgAreaDir dir;
    uint16_t w, h;
    uint8_t  bpp, colours;
    uint16_t n_tiles, ccols, crows, n_warps, n_zones, n_signs, n_overlays, n_chars, n_people;
    uint32_t tiles_off, tiles_len, tiles_crc, chars_off, chars_len, chars_crc, chunk0;
    const uint8_t *palette, *tflags, *warps, *zones, *signs, *overlays, *people, *cdir;   /* into the head */
} MgArea;

typedef struct {
    FILE    *f;
    uint32_t size, fingerprint, n_mons, n_areas;
    uint16_t minor;
    MgSect   s[MG_NSECT];

    /* META */
    char     title[40];
    uint16_t start_area;
    uint8_t  start_x, start_y, start_facing;
    uint8_t  looks;         /* the player's looks, in LOOK: 12 frames each, 8 bits, LOOK's own palette */
    uint8_t  look_colours;
    uint32_t look_frames;   /* the frames' offset in LOOK */
    uint8_t  n_starters;    /* 3-8 if the file names the first creatures to choose from, else 0 */
    uint16_t starters[MG_MAX_STARTERS];
    uint32_t rules[8];      /* COEFF_DAMAGE, stat coefficient, multiplier min, max (1/100),
                               experience exponent, maximum level, moves known, party size */
    uint32_t capture[7];    /* shakes, constant, denominator, divisor, HP mult, current-HP mult, HP divisor */
    uint32_t growth[4];     /* IV maximum; experience multipliers (1/100): default, fast, slow */
    uint32_t keeper_bonus;  /* experience from a keeper's creatures, percent (100 in a file without 0x23) */
    uint8_t  n_steps;       /* once flag is set, a zone naming table uses to (in order: a table may step twice) */
    struct { uint16_t flag, table, to; } steps[MG_MAX_STEPS];

    uint8_t  n_types, n_shapes, n_statuses;
    uint8_t  types[MG_MAX_TYPES][MG_MAX_TYPES];     /* attacker by row, quarters */
    uint8_t  shapes[MG_MAX_SHAPES][MG_NSTATS];
    MgStatus statuses[MG_MAX_STATUSES];
    uint32_t n_moves, n_learn, n_tables, n_enc_rows, enc_rows0;

    char     err[80];
} MonGame;

/* Open and check (section 6). On failure g->err says why and the file is closed. */
int  mg_open(MonGame *g, const char *path);
/* The same on a stream already open (the fuzz gate's fmemopen); g owns it. */
int  mg_open_fp(MonGame *g, FILE *f);
void mg_close(MonGame *g);

int  mg_move(MonGame *g, uint32_t i, MgMove *m);
int  mg_mon(MonGame *g, uint32_t i, MgMon *m);
/* entry j of creature m's learnset: the level and the move's position */
int  mg_learn(MonGame *g, const MgMon *m, uint32_t j, uint8_t *level, uint16_t *move);
/* encounter table t: its rows' count, and row j */
int  mg_enc_rows(MonGame *g, uint32_t t, uint16_t *n);
int  mg_enc_row(MonGame *g, uint32_t t, uint32_t j, MgEncRow *r);
int  mg_area_dir(MonGame *g, uint32_t i, MgAreaDir *a);
/* A string into buf, NUL-terminated and cut to fit at a character boundary. */
int  mg_text(MonGame *g, MgStr s, char *buf, uint32_t cap);
/* A type's name (its string reference). */
int  mg_type_name(MonGame *g, uint32_t t, MgStr *s);

/* Positions by stable id (game_ids.tsv), for saves: MG_ERANGE if there's none. */
int  mg_find_mon(MonGame *g, uint32_t id, uint16_t *pos);
int  mg_find_move(MonGame *g, uint32_t id, uint16_t *pos);
int  mg_find_area(MonGame *g, uint32_t id, uint16_t *pos);

/* Enter area i: read and check its head into buf (head_len bytes; MG_HEAD_MAX
 * is always enough), and fill a. The pointers in a point into buf. */
int  mg_area_open(MonGame *g, uint32_t i, MgArea *a, uint8_t *buf, uint32_t cap);
void mg_warp(const MgArea *a, uint32_t i, MgWarp *w);
void mg_zone(const MgArea *a, uint32_t i, MgZone *z);
void mg_sign(const MgArea *a, uint32_t i, MgSign *s);
void mg_overlay(const MgArea *a, uint32_t i, uint16_t *cell, uint16_t *tile);
void mg_person(const MgArea *a, uint32_t i, MgPerson *p);
/* palette entry i as RGB565 (0 for index 0 and past the colours: black) */
uint16_t mg_palette(const MgArea *a, uint32_t i);
/* Chunk ci (row by row): 256 u16 tiles then 256 movement bytes, CRC checked,
 * every tile at most n_tiles. buf needs MG_CHUNK_BYTES. */
int  mg_chunk(MonGame *g, const MgArea *a, uint32_t ci, uint8_t *buf);
/* Tile t (1..n_tiles) into buf (256 or 128 B); character frame k likewise
 * (512 or 256 B). Not CRC checked one by one: see section 6. */
int  mg_tile(MonGame *g, const MgArea *a, uint32_t t, uint8_t *buf, uint32_t cap);
int  mg_frame(MonGame *g, const MgArea *a, uint32_t k, uint8_t *buf, uint32_t cap);
/* The player's looks: frame k (look * MG_CHAR_FRAMES + f) into buf (512 B, 8 bits),
 * and their palette as RGB565 into lut[256] (0 for index 0 and past the colours). */
int  mg_look_frame(MonGame *g, uint32_t k, uint8_t *buf, uint32_t cap);
int  mg_look_palette(MonGame *g, uint16_t *lut);
/* Creature m's battle sprite record (header, palette, frames), CRC checked.
 * Returns its length. buf needs MG_SPRITE_MAX. */
int  mg_sprite(MonGame *g, const MgMon *m, uint8_t *buf, uint32_t cap);

/* Raw META entry i (the dump's title and source): key, value into buf. */
int  mg_meta_entry(MonGame *g, uint32_t i, uint16_t *key, uint8_t *buf, uint32_t cap, uint16_t *len);
/* A section's bytes from off, for the dump and the credits screen. */
int  mg_read(MonGame *g, int sect, uint32_t off, void *buf, uint32_t len);

/* Everything (section 6). scratch needs MG_HEAD_MAX + MG_SPRITE_MAX bytes. */
int  mg_verify(MonGame *g, uint8_t *scratch, uint32_t cap);

uint32_t mg_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
