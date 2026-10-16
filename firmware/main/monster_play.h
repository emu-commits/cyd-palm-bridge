/* monster_play.h -- the monster game's rules: creatures, battles, the world
 * and the save, on top of the file reader (monster.h).
 *
 * Pure C and stdio like monster.c: no LVGL, no ESP-IDF, no statics. The rules
 * are Tuxemon's as GAME_FORMAT.md section 2 writes them down, in INTEGER
 * arithmetic with one seeded generator, so a battle replays exactly on every
 * platform (the host gate's recorded battle is the same on the device). Every
 * constant comes from the file's META, not from here.
 *
 * The screens (G2) drive it: a step, a button, a battle action; each returns
 * a list of events (MpEvent) that the screens turn into words and pictures.
 *
 * MEMORY: an MpWorld is about 6 KB (the party, the box of MP_BOX creatures,
 * four chunks and the area's head buffer pointer); the caller allocates it
 * with the screen and frees it on leaving. The area's head needs up to
 * MG_HEAD_MAX bytes, also the caller's, or head_cap if it sets that.
 */
#ifndef MONSTER_PLAY_H
#define MONSTER_PLAY_H

#include <stdint.h>
#include "monster.h"

#define MP_PARTY     6
#define MP_BOX       30
#define MP_MOVES     4
#define MP_CHUNKS    4          /* chunks kept: a screen spans at most 2 x 2 */
#define MP_EVENTS    48         /* the most one action can say */
#define MP_NO_MOVE   0xFFFF

/* a creature, as the rules hold it (positions in the file, not ids) */
typedef struct {
    uint16_t mon;               /* position in MONS */
    uint8_t  level;
    uint8_t  status;            /* position in STAT + 1; 0 none */
    uint16_t hp;
    uint32_t exp;               /* total */
    uint8_t  iv[MG_NSTATS];
    uint16_t moves[MP_MOVES];   /* positions in MOVE, or MP_NO_MOVE */
    uint16_t stat[MG_NSTATS];   /* derived: mp_stats() */
} MpCreature;

/* what happened: the screens' script */
enum {
    MP_EV_NONE,
    /* the world */
    MP_EV_TURN,         /* faced a new way without moving */
    MP_EV_STEP,         /* moved to (a, b) */
    MP_EV_BUMP,         /* couldn't move */
    MP_EV_HOP,          /* jumped a ledge to (a, b) */
    MP_EV_WARP,         /* entered area n at (a, b) */
    MP_EV_SIGN,         /* a sign: text n (offset), b its length */
    MP_EV_ENCOUNTER,    /* a wild creature appears: mon a at level b */
    /* a battle; side 0 the player's, 1 the other */
    MP_EV_USE,          /* side uses move a */
    MP_EV_MISS,
    MP_EV_DAMAGE,       /* side's target loses n HP; a = multiplier (1/100) */
    MP_EV_HEAL,         /* side gains n HP */
    MP_EV_STATUS,       /* side gets status a (position) */
    MP_EV_IMMUNE,       /* side is immune to status a */
    MP_EV_STATUS_HURT,  /* side loses n HP to status a */
    MP_EV_STATUS_HEAL,  /* side gains n HP from status a */
    MP_EV_STATUS_END,   /* side's status a ends */
    MP_EV_RECHARGE,     /* side has nothing it can use this turn */
    MP_EV_FAINT,        /* side's creature faints */
    MP_EV_SWAP,         /* the player sends out party member a */
    MP_EV_NEED_SWAP,    /* the player's creature fainted: choose another */
    MP_EV_THROW,        /* the player throws a capture device */
    MP_EV_SHAKE,        /* it shakes (a = which, from 1) */
    MP_EV_BREAK_FREE,
    MP_EV_CAUGHT,       /* caught: into the party (b = 0) or the box (b = 1), at a */
    MP_EV_RUN_FAIL,
    MP_EV_RUN,          /* got away */
    MP_EV_EXP,          /* party member a gains n experience */
    MP_EV_LEVEL,        /* party member a reaches level b */
    MP_EV_LEARN,        /* party member a learns move b */
    MP_EV_LEARN_FULL,   /* party member a could learn move b but knows four: mp_learn() */
    MP_EV_EVOLVE,       /* party member a becomes creature b */
    MP_EV_WIN,
    MP_EV_LOSE,         /* every creature fainted: healed, back at the start */
};

typedef struct {
    uint8_t  kind, side;
    uint16_t a, b;
    uint32_t n;
} MpEvent;

typedef struct { MpEvent ev[MP_EVENTS]; uint8_t n, overflow; } MpLog;

/* battle actions */
enum { MP_ACT_MOVE, MP_ACT_CATCH, MP_ACT_RUN, MP_ACT_SWAP };

typedef struct {
    uint8_t    active;          /* the player's creature in battle (party position) */
    uint8_t    fought;          /* party members that took part (bits) */
    uint8_t    need_swap, over, run_tries;
    uint8_t    recharge[2][MP_MOVES];
    MpCreature wild;
} MpBattle;

typedef struct {
    MonGame   *g;
    MgArea     area;
    uint8_t   *head;            /* the area's head: the caller's buffer */
    uint32_t   head_cap;        /* its size; 0 means MG_HEAD_MAX. Set before mp_new
                                   or mp_load, which keep it (the screens size it
                                   to the file's largest head) */
    uint8_t    x, y, facing;
    uint32_t   rng;
    uint8_t    n_party;
    uint16_t   n_box;
    MpCreature party[MP_PARTY];
    MpCreature box[MP_BOX];
    /* the chunks near the player: tiles and movement, as mg_chunk reads them */
    int32_t    chunk_ix[MP_CHUNKS];          /* -1 empty */
    uint8_t    chunk[MP_CHUNKS][MG_CHUNK_BYTES];
    uint8_t    chunk_next;
    char       err[80];
} MpWorld;

/* ---- the generator: xorshift32, seeded; never 0 ---- */
uint32_t mp_rand(uint32_t *s);
uint32_t mp_below(uint32_t *s, uint32_t n);         /* 0 .. n-1 */

/* ---- creatures ---- */
/* the experience total that reaches `level` for creature position `mon` */
uint32_t mp_exp_for(MonGame *g, uint16_t mon, uint32_t level);
int  mp_stats(MonGame *g, MpCreature *c);          /* fills c->stat from mon, level, iv */
/* a new creature: ivs drawn, experience at the level's floor, full HP, and the
 * last moves of its learnset up to its level (as Tuxemon's set_moves) */
int  mp_make(MonGame *g, uint32_t *rng, uint16_t mon, uint8_t level, MpCreature *c);
/* learn move `move` into slot (0-3), or slot -1 to let it go */
int  mp_learn(MonGame *g, MpCreature *c, uint16_t move, int slot);
/* the numbers a battle uses, exposed for the gate */
uint32_t mp_multiplier(MonGame *g, const MgMove *m, const MpCreature *def, MgMon *def_mon);  /* 1/100 */
uint32_t mp_damage(MonGame *g, const MpCreature *att, const MgMove *m, const MpCreature *def, uint32_t *mult);
uint32_t mp_speed_score(const MpCreature *c, const MgMove *m);
uint32_t mp_shake_check(MonGame *g, const MpCreature *c, const MgMon *m, uint32_t u);   /* u in 1/100 */

/* ---- the world ---- */
/* A new game: the start, and the given creature at level 5 as the first of the party. */
int  mp_new(MpWorld *w, MonGame *g, uint8_t *head, uint32_t seed, uint16_t starter);
/* the movement byte at (x, y) of the current area (0x0F outside it) */
int  mp_cell(MpWorld *w, int x, int y, uint16_t *tile, uint8_t *move);
/* One press of a direction: turn, step, hop, warp, maybe meet a wild creature
 * (then *b is filled and the battle starts). */
int  mp_step(MpWorld *w, int dir, MpBattle *b, MpLog *log);
/* The A button: the sign the player faces, if any. */
int  mp_interact(MpWorld *w, MpLog *log);
/* heal the party: full HP, no status */
void mp_heal_party(MpWorld *w);

/* ---- battles ---- */
int  mp_battle_start(MpWorld *w, MpBattle *b, const MpCreature *wild);
/* One action. MP_ERANGE (nothing done) for an action that can't be taken: a
 * move recharging or empty, a fainted creature, a full box, or anything but
 * a swap while one is needed. */
int  mp_turn(MpWorld *w, MpBattle *b, int act, int arg, MpLog *log);
/* what the other side does: a move slot it can use, or -1 */
int  mp_wild_choice(MpWorld *w, MpBattle *b);

/* ---- the save ---- */
#define MP_SAVE_HEADER 32
#define MP_SAVE_REC    40
int  mp_save(MpWorld *w, const char *path);
/* Load into a world made with mp_new's arguments' game; refuses a damaged
 * file, and one naming creatures, moves or areas this game file doesn't have. */
int  mp_load(MpWorld *w, MonGame *g, uint8_t *head, const char *path);

#endif
