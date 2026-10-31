/* monster_bot.h -- Critters' Auto-play: a bot that plays the game
 * (docs/GAME_PLAN.md §8).
 *
 * Pure C like monster_play.c: no LVGL, no statics. It decides; the caller
 * does. On the map it says which way to press or to press A, in a battle
 * which action to take, and over a full set of moves which to forget. The
 * screens turn those into the same presses a person makes (the cursor moves,
 * the menus open), and the host gate (make -C sim monster) feeds them
 * straight to the rules and checks that the bot beats every keeper in the
 * region from a fixed seed: a map change that blocks the way, or a fight that
 * can't be won, fails it.
 *
 * HOW IT PLAYS. Its goals, in order: rest at a healer when the party is
 * worn down; else the nearest keeper not yet beaten (fewest areas away, then
 * fewest steps; once chosen, kept until beaten), training on an encounter
 * zone first while its best creature is below that keeper's best; when every
 * keeper is beaten it's done. It
 * finds its way with a breadth-first search over the current area's cells
 * (people stand in the way, a warp is a door it only goes through on
 * purpose, a ledge hops), and between areas over the warps. Nothing about
 * the region is written here: every keeper, healer, warp and zone is read
 * from the file, so a new region needs no change.
 *
 * It rests before taking on a keeper unless the party is near full, and
 * after losing to a keeper it trains a level higher before trying them
 * again (mb_events tells it of the loss).
 *
 * In a battle it uses the move with the most expected damage (damage x
 * accuracy, the rules' own numbers), heals with a move when low, swaps in a
 * stronger creature, tries to catch a wild creature it hasn't got once
 * it's weak, and runs from wild ones when the party is worn down.
 *
 * MEMORY: an MbBot is about 3 KB; mb_init needs a scratch buffer as big as
 * the largest area head while it reads the region. Each area the player is in
 * takes 6 B a cell more while the bot is on (9.6 KB for the Brideswood's
 * 40 x 40), allocated as the area is entered, freed by mb_free. An area over
 * MB_MAX_CELLS cells is refused (mb_world says MB_STUCK). */
#ifndef MONSTER_BOT_H
#define MONSTER_BOT_H

#include <stdint.h>
#include "monster.h"
#include "monster_play.h"

#define MB_MAX_AREAS  32
#define MB_MAX_WARPS  128
#define MB_MAX_PEOPLE 96            /* keepers and healers, region-wide */
#define MB_MAX_ZONES  64
#define MB_MAX_TABLES 64            /* encounter tables whose levels it knows */
#define MB_MAX_CELLS  (64 * 64)

/* what mb_world says to do on the map */
enum { MB_STEP, MB_TALK, MB_DONE, MB_STUCK };

typedef struct { uint8_t area, x, y, w, h, facing; uint16_t to; } MbWarp;
typedef struct {
    uint8_t  area, x, y, kind, top;      /* top: a keeper's best level */
    uint8_t  index;                      /* its position in its area */
    uint8_t  margin;                     /* levels over top to train to: one more per loss to them */
    uint16_t flag, hide_if, show_if;
} MbPerson;
typedef struct { uint8_t area, x, y, w, h; uint16_t table; } MbZone;    /* an encounter zone and its table */

typedef struct {
    MonGame *g;
    uint32_t rng;
    uint16_t n_areas, n_warps, n_people, n_zones;
    MbWarp   warps[MB_MAX_WARPS];
    MbPerson people[MB_MAX_PEOPLE];
    MbZone   zones[MB_MAX_ZONES];
    uint8_t  table_hi[MB_MAX_TABLES];   /* each table's highest level (the steps choose the table) */
    /* the current area's cells, read when it's entered */
    int32_t  grid_area;             /* -1 none */
    uint16_t gw, gh;
    uint8_t *cell;                  /* per cell: the movement byte */
    uint8_t *kind;                  /* per cell: 1 a ledge */
    uint8_t *prev;                  /* the search: how it was reached to stand on (0 not yet) */
    uint8_t *door;                  /* the search: how it was reached as a warp */
    uint16_t *queue;
    uint8_t  chunk[MG_CHUNK_BYTES];
    /* a battle's catch tries, and the way it paces on grass */
    uint8_t  tries, pace;
    uint8_t  fighting;              /* the keeper in this battle: people[] + 1, or 0 */
    uint8_t  goal;                  /* the keeper it's after: people[] + 1, or 0 */
    uint32_t foe_mon;               /* the wild creature the tries are at */
    char     why[48];               /* what it's doing now, in words */
} MbBot;

/* Read the region (every area's head, into scratch: cap is at least the
 * largest head_len). 0, or an MG_ error with g->err saying why. */
int  mb_init(MbBot *b, MonGame *g, uint32_t seed, uint8_t *scratch, uint32_t cap);
void mb_free(MbBot *b);
/* On the map: MB_STEP with *dir (N E S W; a turn first if it faces another
 * way), MB_TALK (press A), MB_DONE (every keeper is beaten), or MB_STUCK (no
 * way to anything: *dir is a step at random). < 0 if the card can't be read. */
int  mb_world(MbBot *b, MpWorld *w, int *dir);
/* In a battle (b->over 0): the action for mp_turn and its argument. */
void mb_battle(MbBot *b, MpWorld *w, MpBattle *bt, int *act, int *arg);
/* Party member who could learn `move` but knows four: the slot to forget, or -1. */
int  mb_learn(MbBot *b, MpWorld *w, int who, uint16_t move);
/* What the rules said after each action (a lost battle teaches it). */
void mb_events(MbBot *b, const MpBattle *bt, const MpLog *log);
/* every keeper in the region beaten */
int  mb_done(const MbBot *b, const MpWorld *w);

#endif
