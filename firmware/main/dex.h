/* dex.h -- what the Dex knows (docs/DEX_PLAN.md): the game file and the last
 * save, opened read-only, and what the speaker says from them. No LVGL and no
 * statics, so `make -C sim dex` runs all of it on the host; ui_dex.c draws it.
 *
 * It never writes either file. Opening any screen closes Critters and saves it
 * (content_clear()), so the save it reads is current. */
#ifndef DEX_H
#define DEX_H

#include <stddef.h>
#include <stdint.h>
#include "monster.h"
#include "monster_play.h"

enum { DX_NEVER, DX_SEEN, DX_CAUGHT };

typedef struct {
    MonGame  g;
    MpMarks  marks;
    uint8_t  has_save;              /* a save was read */
    uint16_t n;                     /* creatures */
    uint16_t n_seen, n_caught;      /* caught ones count as seen */
    uint32_t head_cap;              /* the largest area head: what dx_where and dx_greeting need */
    uint16_t *mon;                  /* by Dex position (number - 1): the position in MONS */
    uint8_t  *state;                /* by Dex position: DX_* */
    uint16_t *caught;               /* the Dex positions of the caught ones, n_caught of them */
    uint16_t said;                  /* how many times he's spoken on the list since opening */
    int16_t  talk_pos;              /* the entry last asked about (position + 1), and how often */
    uint16_t talk_n;
} Dex;

/* mg_open's results; a missing or unreadable save isn't an error (has_save 0).
 * On failure d->g.err says why and nothing stays allocated. */
int  dx_open(Dex *d, const char *game, const char *save);
void dx_close(Dex *d);

/* Where a critter lives in the save's act (DEX_PLAN.md 4.3). */
#define DX_AREAS 8
#define DX_KEEPERS 3
#define DX_NAME 32
typedef struct {
    uint8_t n_areas, more_areas, lo, hi, n_keepers, more_keepers;
    char    area[DX_AREAS][DX_NAME];
    char    keeper[DX_KEEPERS][DX_NAME];
} DxWhere;
/* head: head_cap bytes, the caller's for as long as the search takes */
int  dx_where(Dex *d, uint16_t mon, DxWhere *w, uint8_t *head);

/* The speaker's lines (DEX_PLAN.md 3), each to fit the lower screen. */
#define DX_LINE 240
/* The opening, or Talk on the list. Each one moves on (d->said): what to do next,
 * a piece of who he is (more of it opens as the Dex fills), where he met one of
 * yours; a count of nearly done or full has its own lines. */
void dx_greeting(Dex *d, char *out, size_t cap, uint8_t *head);
/* Just what to do next: a hint, a line to finish, how it stands */
void dx_progress(Dex *d, char *out, size_t cap, uint8_t *head);
/* Where he met this critter, by its name; NULL if he hasn't said */
const char *dx_memory(const char *name);
unsigned dx_memory_count(void);
const char *dx_memory_name(unsigned i);
/* Talk on an entry: where it lives; a second Talk, where he met one, if he did */
void dx_talk(Dex *d, int pos, char *out, size_t cap, uint8_t *head);

/* "Six", "Thirty-three": for his lines */
void dx_number(unsigned n, char *out, size_t cap);

#endif
