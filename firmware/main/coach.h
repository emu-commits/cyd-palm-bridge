/* coach.h -- pure Coach logic (no LVGL, no ESP-IDF, no stdio), for the Coach app.
 *
 * Coach is a ritual-based focus timer: three taps pick energy / domain / intention,
 * one Graffiti stroke becomes the session's "sigil", and the sigil inks in from the
 * bottom as the session elapses. Afterwards you tap a result (and a blocker, if it
 * went badly), and the session is appended to an on-SD log.
 *
 * Everything here is pure and clock-injected on purpose -- every call that needs the
 * wall clock or the local zone takes `epoch`/`tz_off_min` rather than calling time()
 * or reading TZ, so the whole engine is deterministic and host-testable
 * (sim/tests/coach_test.c). ui.c owns all file I/O and all user-facing copy; this
 * file owns the numbers. That is the same split minesweeper.c / sudoku.c use.
 *
 * See docs/COACH_DESIGN.md for the product design this implements.
 */
#ifndef COACH_H
#define COACH_H
#include <stdint.h>

/* ---- the ritual's fixed vocabularies (indices are persisted: never reorder) ---- */
enum { CO_ENERGY_LOW, CO_ENERGY_MED, CO_ENERGY_HIGH, CO_NENERGY };
enum { CO_DOM_CAREER, CO_DOM_HEALTH, CO_DOM_LEARN, CO_DOM_CREATE, CO_NDOM };
enum { CO_INT_FINISH, CO_INT_EXPLORE, CO_INT_MAINTAIN, CO_NINT };
enum { CO_RES_GREAT, CO_RES_OKAY, CO_RES_STRUGGLED, CO_RES_ABANDONED, CO_NRES };
enum { CO_BLK_NONE, CO_BLK_TIRED, CO_BLK_KIDS, CO_BLK_WORK, CO_BLK_DISTRACT, CO_NBLK };
enum { CO_MORNING, CO_AFTERNOON, CO_EVENING, CO_NSLOT };

/* session phase, persisted in CoachState so a reboot can recover mid-session */
enum { CO_PH_IDLE, CO_PH_RUNNING, CO_PH_PAUSED, CO_PH_REFLECT };

/* advice codes returned by coach_advise(). ui.c maps these to wording, so the tone
 * can be retuned without touching logic or invalidating a single test. */
enum { CA_NONE, CA_EARLIER, CA_SHORTER, CA_REDUCE, CA_INCREASE, CA_STEADY };

/* a "bad" session for rate purposes: struggled or abandoned */
#define CO_IS_BAD(res) ((res) == CO_RES_STRUGGLED || (res) == CO_RES_ABANDONED)

/* ---- one finished session: exactly 12 bytes, appended to /sdcard/coach.log ---- */
typedef struct {
    uint32_t start;        /* epoch the session began                        */
    uint16_t planned_min;
    uint16_t actual_min;   /* wall minutes actually run, pauses excluded     */
    uint8_t  domain;       /* CO_DOM_*                                       */
    uint8_t  energy;       /* CO_ENERGY_*                                    */
    uint8_t  intent;       /* CO_INT_*                                       */
    uint8_t  outcome;      /* result | blocker << 3 -- see co_pack()         */
} CoachRec;

static inline uint8_t co_pack(int result, int blocker){
    return (uint8_t)(((unsigned)result & 7u) | (((unsigned)blocker & 7u) << 3));
}
static inline int co_result(const CoachRec *r){ return r->outcome & 7; }
static inline int co_blocker(const CoachRec *r){ return (r->outcome >> 3) & 7; }

/* ---- the sigil: the session's mark, normalised to a 0..100 box ---- */
#define CO_SIG_MAXPT 39
typedef struct {
    uint8_t n;                      /* points used, <= CO_SIG_MAXPT          */
    int8_t  xy[2 * CO_SIG_MAXPT];   /* x,y pairs, each 0..100                */
    uint8_t pad;                    /* keeps the struct a tidy 80 bytes      */
} CoachSigil;

/* ---- durable app state: /sdcard/coach.sav ---- */
typedef struct {
    uint32_t  magic;
    /* --- the live session (phase == CO_PH_IDLE means none) --- */
    uint32_t  start;       /* epoch the live session began                   */
    uint16_t  planned_min;
    uint8_t   domain, energy, intent;
    uint8_t   phase;       /* CO_PH_*                                        */
    /* --- durable counters --- */
    int32_t   last_day;    /* local day index of the last completed session  */
    uint16_t  streak, best_streak;
    uint16_t  today_n, day_goal;
    uint16_t  total_n;
    uint16_t  pref_min;    /* configured session length, minutes             */
} CoachState;

/* ---- folded stats over a range of records: the ONLY thing held while reading ---- */
typedef struct {
    uint16_t n;
    uint32_t focus_min;
    uint16_t dom[CO_NDOM];
    uint16_t res[CO_NRES];
    uint16_t blk[CO_NBLK];
    uint16_t slot_n[CO_NSLOT];      /* sessions per time-of-day bucket       */
    uint16_t slot_bad[CO_NSLOT];    /* of those, struggled or abandoned      */
    uint16_t en_n[CO_NENERGY];
    uint16_t en_great[CO_NENERGY];
    uint16_t en_bad[CO_NENERGY];
    uint32_t planned_sum, actual_sum;
    uint16_t planned_lo, planned_hi;/* range of session lengths seen         */
} CoachAgg;

/* ---- time helpers (injected zone offset, minutes east of UTC) ---- */
/* local day number since the epoch -- the unit the streak counts in. */
int32_t coach_day_index(uint32_t epoch, int tz_off_min);
/* local hour 0..23. */
int coach_local_hour(uint32_t epoch, int tz_off_min);
/* CO_MORNING (<12) / CO_AFTERNOON (<17) / CO_EVENING. */
int coach_slot(uint32_t epoch, int tz_off_min);
/* short label for a slot ("mornings"), a domain ("Career"), a blocker, a result. */
const char *coach_slot_name(int slot);
const char *coach_domain_name(int dom);
const char *coach_energy_name(int en);
const char *coach_intent_name(int in);
const char *coach_blocker_name(int blk);
const char *coach_result_name(int res);

/* ---- aggregation ---- */
void coach_agg_reset(CoachAgg *a);
void coach_agg_add(CoachAgg *a, const CoachRec *r, int tz_off_min);

/* ---- derived read-outs for the report (all pure, all integer) ---- */
/* slot with the best success rate among those with >= CO_MIN_SLOT sessions; -1 if
 * no slot qualifies. coach_slot_ok_pct() is its success rate, 0..100 (-1 if none). */
int coach_best_slot(const CoachAgg *a);
int coach_worst_slot(const CoachAgg *a);
int coach_slot_ok_pct(const CoachAgg *a, int slot);
/* most-cited blocker (CO_BLK_NONE if nothing was blocked); its count. */
int coach_top_blocker(const CoachAgg *a);
/* domain with the most sessions; -1 if there are none. */
int coach_top_domain(const CoachAgg *a);
/* percentage of sessions at `energy` that came out Great, 0..100 (-1 if no data). */
int coach_energy_great_pct(const CoachAgg *a, int energy);
/* percentage of all sessions that struggled or were abandoned, 0..100 (0 if empty). */
int coach_bad_pct(const CoachAgg *a);

/* ---- the rule engine ---- */
/* Deterministic, fixed priority, first match wins. See docs/COACH_DESIGN.md §5. */
int coach_advise(const CoachAgg *a);

/* ---- streak bookkeeping ---- */
/* fold a just-finished session into the durable counters (streak, today_n, total). */
void coach_note_completion(CoachState *s, uint32_t when, int tz_off_min);
/* the streak as it stands AT `now` -- a stored streak older than yesterday is dead. */
int coach_streak_now(const CoachState *s, uint32_t now, int tz_off_min);
/* sessions completed on the local day containing `now` (0 once the day rolls over). */
int coach_today_now(const CoachState *s, uint32_t now, int tz_off_min);
/* reset a state block to first-run defaults (25-minute sessions, goal of 6). */
void coach_state_init(CoachState *s);

/* ---- sigil ---- */
/* normalise a raw captured stroke (x,y pairs in any coordinate space) into `sg`,
 * scaled to a 0..100 box with aspect preserved and centred on the short axis.
 * Strokes longer than CO_SIG_MAXPT are decimated evenly. Returns the point count
 * stored (0 if the stroke was empty or degenerate, which callers treat as "no
 * sigil" and fall back to a plain filled square). */
int coach_sigil_from_raw(CoachSigil *sg, const int16_t *xy, int n);
/* 1 if the sigil holds a drawable stroke. */
int coach_sigil_ok(const CoachSigil *sg);

#endif
