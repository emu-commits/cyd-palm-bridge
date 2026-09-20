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

/* ---- the habit pool -------------------------------------------------------
 * A table of specific, individually checkable things. "Eat healthy" is not a
 * task; "one brazil nut" is.
 *
 * THE HABITS THEMSELVES ARE NOT IN THE SOURCE. They live in
 * firmware/main/guru_pool.txt, which tools/gen_guru_pool.py compiles into the
 * built-in table below, and which can also be dropped on the card as
 * /sdcard/guru.txt to replace the list without a reflash. Edit the .txt.
 *
 * The built-in table is const flash rodata, so it costs no RAM and none of the
 * LVGL object pool. A pool loaded off the card costs its own text, once.
 *
 * NOT MEDICAL ADVICE, and the copy must never drift into it. These are widely
 * discussed consumer wellness practices, not treatment for anything. A `why`
 * line MAY say what a practice is understood to do and why people do it -- a
 * habit with no stated point is one nobody keeps -- but it must never name a
 * disease, promise to prevent or cure anything, or give a dose. "Builds aerobic
 * base" is fine; "prevents heart disease" and "take 5 g" are not. Guru's
 * Menu > About carries the one-line disclaimer that ships with the list.
 *
 * Categories exist to give the week analysis something to say beyond a total.
 * They are named for the domain of the habit, not for a disease.
 * INDICES ARE PERSISTED IN THE LOG: never reorder, only append. */
enum { GU_CAT_GUT, GU_CAT_METAB, GU_CAT_COGN, GU_CAT_STRUCT, GU_CAT_RECOV, GU_NCAT };

/* One task. `id` is a STABLE NUMBER, not a position: the log stores it, the pool
 * gets a Menu editor in a later phase, and a log written today has to still mean
 * the same thing after the user adds or hides one. IDs are assigned once and
 * never reused; the table may grow and may not be reordered. */
typedef struct {
    uint16_t    id;
    uint8_t     cat;        /* GU_CAT_* */
    const char *name;       /* the row text -- short enough for the table column */
    const char *why;        /* one line of context, shown when the row is tapped */
} GuruTask;

/* The bitmap of what is checked off today is a fixed 64 bits in the saved state,
 * so this is the ceiling on the pool until that format is revised. Asserted
 * against the real table in guru_test.c rather than left as a comment. */
#define GU_TASK_MAX  64
#define GU_BITS      ((GU_TASK_MAX + 7) / 8)

/* the table generated from guru_pool.txt; the fallback when the card has none. */
extern const GuruTask GU_POOL_BUILTIN[];
extern const int      GU_POOL_BUILTIN_N;

int              guru_ntasks(void);            /* how many tasks the pool holds  */
const GuruTask  *guru_task(int i);             /* by POSITION, 0..ntasks-1       */
const GuruTask  *guru_task_by_id(int id);      /* by stable id; NULL if unknown  */
const char      *guru_cat_name(int cat);       /* "Gut", "Metabolic", ... (shown) */
const char      *guru_cat_key(int cat);        /* "gut", "metabolic", ... (file)  */
int              guru_cat_from_key(const char *key);  /* -1 if not a category    */

/* ---- replacing the pool at runtime ----------------------------------------
 * Install a pool parsed from /sdcard/guru.txt in place of the built-in one.
 * gurupool.c does this at startup; this file stays free of file I/O.
 *
 * THE CALLER KEEPS OWNERSHIP. The array and every string in it are borrowed,
 * not copied, and must outlive the installation -- gurupool.c holds them in a
 * single arena for the life of the process. Nothing here ever frees them.
 *
 * Returns 0 if installed, -1 if the pool was refused, in which case the
 * previous pool stays in place. A card holding a truncated or malformed
 * guru.txt must leave the user with the built-in list, never an empty app, so
 * the checks here repeat the loader's rather than trusting them. */
int  guru_pool_set(const GuruTask *tasks, int n);
void guru_pool_reset(void);                    /* back to the built-in table     */
int  guru_pool_is_custom(void);                /* 1 if a card pool is installed  */

/* ---- one check, appended to /sdcard/guru.log ------------------------------
 * Exactly 8 bytes, and frozen: the week analysis reads these back.
 *
 * `cat` is denormalised out of the task table on purpose. The pool gets an editor
 * later; if a task is ever recategorised, the week it was logged under should
 * still analyse the way it was lived, and a log that has to be joined against a
 * mutable table to mean anything is a log that rots. `flags` bit 0 marks an undo,
 * which is appended rather than erased -- the file stays append-only, and the
 * analysis nets them. */
typedef struct {
    uint32_t when;          /* epoch the check landed          */
    uint16_t task;          /* stable task id                  */
    uint8_t  cat;           /* GU_CAT_* as it was at the time  */
    uint8_t  flags;         /* bit 0: this undoes an earlier check */
} GuruRec;

#define GU_F_UNDO 0x01

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
    uint8_t  today_bits[GU_BITS];   /* WHICH task ids are ticked today           */
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
/* Record (or take back) one anonymous check on the local day containing `now`.
 * Both return the resulting count for today; un-checking never goes below zero.
 * These are the counting layer -- guru_toggle() below is what the UI calls. */
int guru_note_check(GuruState *s, uint32_t now, int tz_off_min);
int guru_note_uncheck(GuruState *s, uint32_t now, int tz_off_min);

/* Is task `id` ticked on the local day containing `now`? A stale state (the day
 * has rolled since it was written) reads as all-clear without needing a roll. */
int guru_is_checked(const GuruState *s, int id, uint32_t now, int tz_off_min);

/* Tick or un-tick task `id` for today, keeping the bitmap and the day's count in
 * step. Returns the NEW checked state (1 ticked, 0 clear), so the caller knows
 * which way it went and what to append to the log. An unknown or out-of-range id
 * changes nothing and returns 0. */
int guru_toggle(GuruState *s, int id, uint32_t now, int tz_off_min);

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

/* ---- the week, read out of the ring ---------------------------------------
 * These come from the saved state rather than the log because the ring already
 * holds exactly one week of per-day counts -- streaming the log to recompute
 * what is sitting in a seven-byte array would be a second implementation of the
 * same thing, and the two would eventually disagree. The log's job is the
 * per-category breakdown below, which the ring does NOT hold. */
/* local days in the window that got at least one check, 0..GU_WIN. */
int guru_window_days_active(const GuruState *s, uint32_t now, int tz_off_min);
/* the most checks landed on any single day of the window. */
int guru_window_best(const GuruState *s, uint32_t now, int tz_off_min);

/* ---- folded stats over a range of log records -----------------------------
 * Deliberately tiny: the whole history is never resident, records stream in one
 * at a time and only this fold is held. An undo record decrements what its
 * matching check added, which is what makes an append-only log net out. */
typedef struct {
    uint16_t n;                 /* net checks folded                          */
    uint16_t cat[GU_NCAT];      /* net checks per category                    */
} GuruAgg;

void guru_agg_reset(GuruAgg *a);
void guru_agg_add(GuruAgg *a, const GuruRec *r);

/* the category with the most checks, and the one with the fewest. Ties go to the
 * LOWEST index both times, so the order of GU_CAT_* decides ties and the answer
 * is stable across runs. -1 if there is nothing to rank. */
int guru_top_cat(const GuruAgg *a);
int guru_weakest_cat(const GuruAgg *a);

/* ---- the rule engine ------------------------------------------------------
 * Advice codes, mapped to wording in ui.c (the same split coach.h uses), so her
 * tone can be retuned without touching logic or invalidating a single test. */
enum { GA_NONE, GA_NEGLECTED, GA_NARROW, GA_SPOTTY, GA_STEADY, GA_KEEPGOING };

/* Deterministic, fixed priority, first match wins. `days_active` and
 * `window_days` come from the two ring read-outs above; passing them in rather
 * than reaching for a GuruState keeps this pure and trivially testable.
 *
 * GA_NEGLECTED means one category got nothing at all -- the caller pairs it with
 * guru_weakest_cat() to name which. It is deliberately ahead of GA_NARROW: "you
 * did nothing from Recovery" is more actionable than "most of it was Movement",
 * and a week can be both. */
int guru_advise(const GuruAgg *a, int days_active, int window_days);

#endif
