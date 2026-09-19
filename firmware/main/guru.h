/* guru.h -- pure Guru logic (no LVGL, no ESP-IDF, no stdio), for the Guru app.
 *
 * Guru is a treadmill of specific daily longevity habits. You check items off a
 * fixed pool; she keeps the count, works out what a fair target is for you, and
 * (later) reads the week back in her own voice.
 *
 * The target is the whole idea, so it is worth stating plainly: **it is the user's
 * own recent average, not a number this app picked.** Averaging the last GU_WIN
 * local days means a good week raises the bar and a bad one lowers it, with a
 * floor of one so the treadmill never stops entirely. Two rules keep that from
 * becoming a trap:
 *
 *   - **Today is excluded from its own average.** Counting today would raise the
 *     target as you worked toward it -- a treadmill that speeds up while you run.
 *   - **Empty days count as zeros.** A lapse pulls the target down, so coming
 *     back after a bad week is met with an achievable number instead of the bar
 *     you cleared before you stopped. That is the point of a rolling window.
 *
 * Everything here is pure and clock-injected: every call that needs the wall clock
 * or the local zone takes `now`/`tz_off_min` rather than calling time(), so the
 * engine is deterministic and host-testable (sim/tests/guru_test.c). ui.c owns all
 * file I/O and all user-facing copy; this file owns the numbers. Same split as
 * coach.c / minesweeper.c / sudoku.c.
 */
#ifndef GURU_H
#define GURU_H
#include <stdint.h>

/* The averaging window, in local days. A week is the natural unit here: it is the
 * period the user already thinks in, it matches the week screen the analysis will
 * live on, and it is short enough that a changed routine shows up within days
 * rather than being diluted by a month of history. */
#define GU_WIN 7

/* The floor. "At least one thing, every day" is the promise the app makes; an
 * average that rounds to zero would otherwise let the treadmill stop. */
#define GU_MIN_TARGET 1

/* A day's stored count saturates here -- it is a uint8_t and nobody checks off
 * 255 habits. Clamping rather than wrapping keeps a corrupt or absurd .sav from
 * turning into a negative average. */
#define GU_DAY_MAX 200

/* ---- durable app state: /sdcard/guru.sav ----------------------------------
 * The ring is indexed by local day number, so day d lives at d % GU_WIN and no
 * shifting is needed when the day rolls -- advancing just zeroes the slots that
 * were passed. `seen_days` saturates at GU_WIN; it exists only to answer "how
 * many of these slots are real days rather than an unused array?". */
typedef struct {
    uint32_t magic;
    int32_t  last_day;          /* the local day the counters below describe     */
    uint8_t  day_n[GU_WIN];     /* checks per local day, ring keyed by day % WIN */
    uint8_t  seen_days;         /* distinct local days ever rolled through, <=WIN */
    uint8_t  pad;
    int32_t  last_active;       /* last local day that got at least one check    */
    uint16_t streak, best_streak;
    uint32_t total_n;           /* checks ever, for the lifetime read-out        */
} GuruState;

/* ---- lifecycle ---- */
/* reset a state block to first-run defaults (no history, no streak). */
void guru_state_init(GuruState *s);

/* Advance the state to the local day containing `now`, zeroing the days passed
 * over. Returns the number of days advanced (0 if it was already current).
 *
 * Every read and write below rolls first, so callers never have to. A clock that
 * has moved BACKWARDS is ignored rather than obeyed: the CYD has no RTC and
 * restores its epoch from an NVS checkpoint at boot, so a backwards jump is much
 * more likely to be a bad clock than a real day, and rewinding the ring would
 * erase a real week of history. */
int guru_roll(GuruState *s, uint32_t now, int tz_off_min);

/* ---- checking things off ---- */
/* Record (or take back) one check on the local day containing `now`. Both return
 * the resulting count for today. Un-checking never goes below zero. */
int guru_note_check(GuruState *s, uint32_t now, int tz_off_min);
int guru_note_uncheck(GuruState *s, uint32_t now, int tz_off_min);

/* ---- read-outs (all pure, all integer) ---- */
/* checks completed on the local day containing `now` (0 once the day rolls). */
int guru_today_n(const GuruState *s, uint32_t now, int tz_off_min);

/* The day's target: the mean of the up-to-GU_WIN-1 completed days before today,
 * rounded half UP (a tie nudges upward -- an average of 4.5 asks for 5), with a
 * floor of GU_MIN_TARGET. A first-ever day has nothing to average and gets the
 * floor. */
int guru_target(const GuruState *s, uint32_t now, int tz_off_min);

/* how many completed days the target above actually drew on, 0..GU_WIN-1. The
 * week screen says "your average over N days" rather than implying a full week
 * of data on day two. */
int guru_window_days(const GuruState *s, uint32_t now, int tz_off_min);

/* 1 once today's count has reached today's target. */
int guru_met_today(const GuruState *s, uint32_t now, int tz_off_min);

/* The streak as it stands AT `now`, in consecutive local days with at least one
 * check. Deliberately NOT "days that met target": the target is the user's own
 * rising average, so a target-based streak would break precisely when someone
 * improved enough to raise their own bar -- it would punish the good week that
 * caused it. The streak measures showing up; the target measures the day. */
int guru_streak_now(const GuruState *s, uint32_t now, int tz_off_min);

/* total checks over the stored window, and the mean per day across it (the same
 * number guru_target() rounds, but including today) -- for the week screen. */
int guru_window_total(const GuruState *s, uint32_t now, int tz_off_min);

#endif
