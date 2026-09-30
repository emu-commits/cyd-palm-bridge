/* srs.h -- Study's schedulers and the progress kept on the card.
 *
 * Pure C and stdio, like course.c: no LVGL, no ESP-IDF, no statics, and THE
 * TIME IS PASSED IN (UNIX seconds, UTC, and the local offset for anything
 * that goes by the calendar day). The host gate (make -C sim srs) runs a year
 * of reviews through both schedulers, truncates the log at every byte, and
 * remaps progress across a course update.
 *
 * TWO SCHEDULERS, chosen and tuned by the course's META (COURSE_FORMAT.md
 * 2.2, SRS_PLAN.md section 5):
 *   stages  WaniKani-style: right moves up one stage, wrong moves down `drop`
 *           (or `drop_high` from `high_from` up); each stage has an interval;
 *           the last stage retires the item.
 *   sm2     Anki-style, as the Albanian course's web app does it: a ladder of
 *           minimum intervals, an ease, grade multipliers, a relearn delay,
 *           and optionally due dates at the start of a day. Integer
 *           arithmetic in hundredths, so every platform agrees to the second.
 *
 * PROGRESS ON THE CARD, in the course's folder:
 *   progress.dat  a 20-byte header (with the records' CRC-32 and the course's
 *                 fingerprint), then one 16-byte record per started item,
 *                 sorted by id. Replaced whole (safefile.h).
 *   progress.log  20-byte entries (a record and its CRC-32), appended and
 *                 flushed to the card on every grade. A power cut can only tear
 *                 the last entry, which fails its CRC and is ignored.
 * Opening folds the log into progress.dat and empties it. Entries are whole
 * records, so folding the same log twice (a crash between the swap and the
 * truncate) changes nothing.
 *
 * MEMORY while open: 2 bits per item (2.3 KB for 9,000 items; 16 KB at the
 * format's 65,535 limit) and the due list (1 KB); 5 KB more while folding,
 * and 4.6 KB while srs_lessons() reads an item's links.
 */
#ifndef SRS_H
#define SRS_H

#include <stdint.h>
#include <stdio.h>
#include "course.h"

#define SRS_REC_SIZE   16
#define SRS_LOG_ENTRY  20
#define SRS_DAT_HEADER 20
#define SRS_DUE_MAX    500      /* due items collected per scan (2 B each) */
#define SRS_FOLD_CHUNK 256      /* log entries merged per pass while folding */

/* grades. A two-button course uses AGAIN (wrong) and GOOD (right). */
enum { SRS_AGAIN = 0, SRS_HARD, SRS_GOOD, SRS_EASY };

#define SRS_F_STARTED   0x01    /* the lesson is done; always set in a record */
#define SRS_F_RETIRED   0x02    /* stages: past the last stage, never due again */
#define SRS_F_SUSPENDED 0x04    /* left out of reviews (a later menu item) */

/* The record, as on the card (COURSE_FORMAT has the course; this is ours):
 *   0 u32 id  4 u32 due  8 u16 interval in hours  10 u8 ease-130 (hundredths)
 *  11 u8 stage (stages: 1..n; sm2: the rung + 1, 0 before the first pass)
 *  12 u8 flags  13 u8 lapses  14 u16 reviews   -- little-endian */
typedef struct {
    uint32_t id;
    uint32_t due;               /* UNIX seconds; 0 = never (retired) */
    uint16_t ivl_h;             /* the current interval, hours (saturating) */
    uint8_t  ease;              /* sm2: (ease - 1.30) / 0.05, so 1.30..14.05 */
    uint8_t  stage;
    uint8_t  flags;
    uint8_t  lapses;            /* saturating */
    uint16_t reviews;           /* saturating */
} SrsRec;

/* Ease is kept in steps of 0.05 so one byte reaches 14.05: the web app the
 * sm2 scheduler copies lets ease grow without limit, and a streak of Easy
 * passes 3.85 (1.30 + 2.55) within months. Courses give eases in steps of
 * 0.05 (Anki's and the web app's all are); the reader refuses others. */
#define SRS_EASE(b)       (130 + 5 * (int32_t)(b))          /* hundredths */
#define SRS_EASE_BYTE(e)  ((uint8_t)(((e) - 130) / 5))
#define SRS_EASE_MAX      (130 + 5 * 255)

void srs_pack(const SrsRec *r, uint8_t out[SRS_REC_SIZE]);
void srs_unpack(const uint8_t in[SRS_REC_SIZE], SrsRec *r);

/* ---- the schedulers: pure functions of the course's settings and the time ---- */

/* The lesson for item `id` is done at `now`: its first record. */
void srs_start(const Course *c, SrsRec *r, uint32_t id, uint32_t now, int32_t tz);
/* A review answered with `grade` at `now` (tz = local offset from UTC, s). */
void srs_grade(const Course *c, SrsRec *r, int grade, uint32_t now, int32_t tz);
/* Known (for unlocking): at or past the course's `known` stage or rung. */
int  srs_known(const Course *c, const SrsRec *r);
/* Which of the course's groups (c->groups) the record is in, for the
 * dashboard; 0 if the course names none that fit. */
int  srs_group(const Course *c, const SrsRec *r);

/* ---- progress on the card ---- */

enum { SRS_NONE = 0, SRS_LEARNING = 1, SRS_KNOWN = 2 };   /* the 2-bit state */

typedef struct {
    Course  *c;
    char     dat[128], log[128];
    FILE    *logf;                      /* open for appending while in use */
    uint8_t *state;                     /* 2 bits per item number */
    uint16_t *due;                      /* item numbers due now (up to SRS_DUE_MAX) */
    uint16_t n_due;
    uint32_t due_total;                 /* all of them, even past SRS_DUE_MAX */
    uint32_t next_due;                  /* the soonest due time after `now`; 0 = none */
    uint32_t started;                   /* items with a record */
    uint32_t group_count[COURSE_MAX_GROUPS];
    uint32_t remapped, dropped;         /* what the last open did to old progress */
    char     err[80];
} Srs;

#define SRS_EIO   (-1)
#define SRS_EBAD  (-2)                  /* progress.dat is damaged: left alone */
#define SRS_ENOMEM (-4)

/* Open a course's progress in `dir`: fold the log in, remap if the course
 * has changed since, and scan what's due at `now`. Creates the files if the
 * course has never been opened. */
int  srs_open(Srs *s, Course *c, const char *dir, uint32_t now, int32_t tz);
void srs_close(Srs *s);

/* The record of item number n: 1 found, 0 not started, < 0 an error. */
int  srs_get(Srs *s, uint32_t n, SrsRec *r);
/* Save a new or changed record (a lesson, a grade, an undo): appended to the
 * log and on the card before this returns. Updates the state and counts. */
int  srs_put(Srs *s, uint32_t n, const SrsRec *r, uint32_t now);
/* Recount what's due, e.g. when the clock has moved on: folds this session's
 * log into progress.dat first, then scans it. */
int  srs_scan(Srs *s, uint32_t now);

/* ---- what's coming: the reviews of the next week, for the lock screen, the
 * launcher and the Week screen's forecast (SRS_PLAN.md S5). study.c keeps it
 * on the card as summary.bin, so they needn't open the course. */

#define SRS_SOON_MAX  1024      /* due times kept to the minute (2 B each) */
#define SRS_SOON_MIN  (7 * 1440)

typedef struct {
    uint32_t at;                /* when it was made */
    uint32_t due;               /* due at `at` */
    uint32_t next;              /* the soonest due time after `at`, to the second; 0 = none */
    uint32_t in24;              /* due in (at, at + 24 h] */
    uint16_t day[7];            /* due on the local day of `at` (after `at`) and the six after */
    uint16_t n_soon;
    uint8_t  full;              /* more than SRS_SOON_MAX fell in the week: the latest were left out */
    /* minutes after `at` (rounded up, so never early) of each review due in
     * the next seven days, soonest first: the SRS_SOON_MAX soonest */
    uint16_t soon[SRS_SOON_MAX];
} SrsSum;

/* srs_scan(), and the week ahead into *sum as well. */
int  srs_scan_sum(Srs *s, uint32_t now, int32_t tz, SrsSum *sum);
/* What the scan does with each due time (zero *u and set u->at first), and
 * the sort at the end: open for the tests. */
void srs_sum_add(SrsSum *u, uint32_t due, int32_t tz);
void srs_sum_done(SrsSum *u);

/* Lessons available now, in teaching order: up to `max` item numbers into
 * `out` (which may be NULL to count). Returns how many there are in all. An
 * item is available when its level is open, it has no record, and (with
 * the course's by_links) everything it's built from is known, and everything
 * it must unlock after is known. A level opens when the one before has its
 * level_percent of level-up items known (90 % rounded up); the first always
 * is. *level gets the highest open level's number. */
int  srs_lessons(Srs *s, uint16_t *out, int max, uint16_t *level);

#endif
