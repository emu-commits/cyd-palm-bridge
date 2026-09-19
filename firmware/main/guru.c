/* guru.c -- pure Guru logic. See guru.h for the contract and the reasoning behind
 * the rolling target.
 *
 * No LVGL, no ESP-IDF, no stdio: this file is the numbers, ui.c is the pixels and
 * the file I/O. Every wall-clock input is injected, so sim/tests/guru_test.c pins
 * every branch on the host in any locale. */
#include "guru.h"
#include "daycal.h"

/* ------------------------------------------------------------------ the pool
 * The habits themselves live in firmware/main/guru_pool.txt, not here. That file
 * is compiled into GU_POOL_BUILTIN[] by tools/gen_guru_pool.py, and the same file
 * can be dropped on the card as /sdcard/guru.txt to replace the list without a
 * reflash -- gurupool.c parses it and calls guru_pool_set() below.
 *
 * This file stays pure, so the override is a pointer somebody else hands us. It
 * is never copied and never freed here: whoever installs a pool owns its memory
 * for as long as it is installed. That keeps the ownership rule to one sentence
 * and keeps malloc out of the logic layer.
 *
 * Read the NOT MEDICAL ADVICE note in guru.h before editing any of the copy.
 * IDs are assigned once and never reused -- the log stores them. */
static const GuruTask *g_pool;      /* 0 means "use the built-in table" */
static int             g_pool_n;

static const GuruTask *gu_pool(void){
    return g_pool ? g_pool : GU_POOL_BUILTIN;
}
static int gu_pool_n(void){
    return g_pool ? g_pool_n : GU_POOL_BUILTIN_N;
}

int guru_pool_set(const GuruTask *tasks, int n){
    /* An empty or absurd pool is refused rather than installed: a card holding a
     * truncated guru.txt should leave the user with the built-in list, not an
     * app with nothing in it. The caller's own validation is the first line of
     * defence; this is the one that has to hold when the card is bad. */
    if(!tasks || n <= 0 || n > GU_TASK_MAX) return -1;
    for(int i = 0; i < n; i++){
        const GuruTask *t = &tasks[i];
        if(t->id < 1 || t->id > GU_TASK_MAX) return -1;
        if(t->cat >= GU_NCAT)                return -1;
        if(!t->name || !t->name[0])          return -1;
        if(!t->why  || !t->why[0])           return -1;
        for(int j = 0; j < i; j++) if(tasks[j].id == t->id) return -1;
    }
    g_pool   = tasks;
    g_pool_n = n;
    return 0;
}

void guru_pool_reset(void){ g_pool = 0; g_pool_n = 0; }

int guru_pool_is_custom(void){ return g_pool != 0; }

int guru_ntasks(void){ return gu_pool_n(); }

const GuruTask *guru_task(int i){
    return (i >= 0 && i < gu_pool_n()) ? &gu_pool()[i] : 0;
}

const GuruTask *guru_task_by_id(int id){
    const GuruTask *p = gu_pool();
    int n = gu_pool_n();
    for(int i = 0; i < n; i++) if(p[i].id == (uint16_t)id) return &p[i];
    return 0;
}

/* Category headings as they appear on screen. "Movement" and "Strength" rather
 * than the internal GU_CAT_METAB / GU_CAT_STRUCT: the enum names are for the log
 * and must never move, the words are for the user and can be retuned freely. */
const char *guru_cat_name(int cat){
    switch(cat){
        case GU_CAT_GUT:    return "Gut";
        case GU_CAT_METAB:  return "Movement";
        case GU_CAT_COGN:   return "Mind";
        case GU_CAT_STRUCT: return "Strength";
        case GU_CAT_RECOV:  return "Recovery";
    }
    return "";
}

/* The machine-readable half of the same thing: the word used in guru_pool.txt.
 * Deliberately separate from guru_cat_name() -- the display heading is copy and
 * may be reworded at any time, but this key is written in the user's own file
 * and renaming it would silently invalidate their edits. */
const char *guru_cat_key(int cat){
    switch(cat){
        case GU_CAT_GUT:    return "gut";
        case GU_CAT_METAB:  return "metabolic";
        case GU_CAT_COGN:   return "mind";
        case GU_CAT_STRUCT: return "strength";
        case GU_CAT_RECOV:  return "recovery";
    }
    return "";
}

int guru_cat_from_key(const char *key){
    if(!key) return -1;
    for(int c = 0; c < GU_NCAT; c++){
        const char *k = guru_cat_key(c);
        int i = 0;
        while(k[i] && key[i] && k[i] == key[i]) i++;
        if(!k[i] && !key[i]) return c;
    }
    return -1;
}

/* ------------------------------------------------------------------ internals */
/* How many of the GU_WIN-1 days before `today` are real days -- days the user has
 * actually lived through with the app -- rather than an unused slot of the ring.
 * Days that elapsed since the state was last written count: they happened, and
 * they were empty, which is exactly the data the average wants. */
static int gu_prior_days(const GuruState *s, int32_t today){
    if(s->seen_days == 0) return 0;
    int32_t adv = today - s->last_day;
    if(adv < 0) adv = 0;                       /* a backwards clock adds no days */
    long p = (long)s->seen_days - 1 + adv;
    if(p > GU_WIN - 1) p = GU_WIN - 1;
    return (int)p;
}

/* The count stored for one local day, read WITHOUT rolling the state -- so it has
 * to reason about staleness itself. Three ways a day holds nothing: the state has
 * never been used, the day is after the one the state describes (time passed with
 * nothing checked off), or the day has fallen out of the ring. */
static int gu_count_on(const GuruState *s, int32_t day, int32_t today){
    if(s->seen_days == 0)         return 0;
    if(day > s->last_day)         return 0;
    if(day <= today - GU_WIN)     return 0;
    return s->day_n[cal_mod(day, GU_WIN)];
}

static int32_t gu_today(uint32_t now, int tz_off_min){
    return cal_day_index(now, tz_off_min);
}

/* Today's tick marks. A task id maps to bit id-1, so id 1 is bit 0 and the
 * ceiling is GU_TASK_MAX ids -- guru_test.c asserts the real pool stays under it
 * rather than trusting this comment. An id outside the range is not clamped into
 * somebody else's bit; it is simply refused. */
static int gu_bit_ok(int id){ return id >= 1 && id <= GU_TASK_MAX; }
static int gu_bit_get(const GuruState *s, int id){
    return (s->today_bits[(id - 1) >> 3] >> ((id - 1) & 7)) & 1;
}
static void gu_bit_set(GuruState *s, int id){
    s->today_bits[(id - 1) >> 3] |= (uint8_t)(1u << ((id - 1) & 7));
}
static void gu_bit_clr(GuruState *s, int id){
    s->today_bits[(id - 1) >> 3] &= (uint8_t)~(1u << ((id - 1) & 7));
}
static void gu_bits_clear(GuruState *s){
    for(int i = 0; i < GU_BITS; i++) s->today_bits[i] = 0;
}

/* ------------------------------------------------------------------ lifecycle */
void guru_state_init(GuruState *s){
    if(!s) return;
    unsigned char *p = (unsigned char *)s;
    for(unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    /* seen_days == 0 is the "never used" sentinel; the first roll seeds the rest. */
}

int guru_roll(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);

    if(s->seen_days == 0){                     /* first ever use: start here */
        for(int i = 0; i < GU_WIN; i++) s->day_n[i] = 0;
        gu_bits_clear(s);
        s->last_day    = today;
        s->last_active = today - 1;            /* no active day yet; see streak */
        s->seen_days   = 1;
        return 0;
    }

    int32_t adv = today - s->last_day;
    if(adv <= 0) return 0;                     /* current, or a backwards clock */

    /* zero the days passed over. Beyond a full window every slot is stale, so
     * clear the lot rather than walking a gap that could be years long. */
    if(adv >= GU_WIN){
        for(int i = 0; i < GU_WIN; i++) s->day_n[i] = 0;
    } else {
        for(int32_t d = s->last_day + 1; d <= today; d++) s->day_n[cal_mod(d, GU_WIN)] = 0;
    }

    /* a new day starts with an empty list -- the ticks belonged to the old one */
    gu_bits_clear(s);

    s->last_day = today;
    long seen = (long)s->seen_days + adv;
    s->seen_days = (uint8_t)(seen > GU_WIN ? GU_WIN : seen);
    return (int)(adv > GU_WIN ? GU_WIN : adv);
}

/* ----------------------------------------------------------- checking things off */
int guru_note_check(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    guru_roll(s, now, tz_off_min);
    int slot = cal_mod(s->last_day, GU_WIN);
    int was  = s->day_n[slot];

    if(was < GU_DAY_MAX) s->day_n[slot] = (uint8_t)(was + 1);
    if(s->total_n < 0xFFFFFFFFu) s->total_n++;

    if(was == 0){                              /* today becomes an active day */
        if(s->streak > 0 && s->last_active == s->last_day - 1){
            if(s->streak < 0xFFFF) s->streak++;
        } else {
            s->streak = 1;
        }
        s->last_active = s->last_day;
        if(s->streak > s->best_streak) s->best_streak = s->streak;
    }
    return s->day_n[slot];
}

int guru_note_uncheck(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    guru_roll(s, now, tz_off_min);
    int slot = cal_mod(s->last_day, GU_WIN);
    if(s->day_n[slot] == 0) return 0;
    s->day_n[slot]--;
    if(s->total_n) s->total_n--;

    /* Taking back the last check un-makes the active day, so the streak has to
     * give back the day it was just granted -- otherwise a mis-tap and its undo
     * leave a day credited that has nothing on it. Yesterday is still in the ring,
     * so we can tell whether the streak survives at length-1 or ends entirely.
     * best_streak deliberately stands: it is a high-water mark, not a live count. */
    if(s->day_n[slot] == 0 && s->last_active == s->last_day){
        int32_t yday = s->last_day - 1;
        if(s->streak > 1 && gu_count_on(s, yday, s->last_day) > 0){
            s->streak--;
            s->last_active = yday;
        } else {
            s->streak      = 0;
            s->last_active = s->last_day - 1;
        }
    }
    return s->day_n[slot];
}

/* Which tasks are ticked today. Kept in step with the day's count rather than
 * derived from it: the count is the history the target averages, the bitmap is
 * only ever about today, and the two have different lifetimes. */
int guru_is_checked(const GuruState *s, int id, uint32_t now, int tz_off_min){
    if(!s || !gu_bit_ok(id) || s->seen_days == 0) return 0;
    /* a state nobody has rolled since yesterday still has yesterday's ticks in
     * it -- report the fresh day the caller is actually asking about. */
    if(gu_today(now, tz_off_min) != s->last_day) return 0;
    return gu_bit_get(s, id);
}

int guru_toggle(GuruState *s, int id, uint32_t now, int tz_off_min){
    if(!s || !gu_bit_ok(id)) return 0;
    guru_roll(s, now, tz_off_min);
    if(gu_bit_get(s, id)){
        gu_bit_clr(s, id);
        guru_note_uncheck(s, now, tz_off_min);
        return 0;
    }
    gu_bit_set(s, id);
    guru_note_check(s, now, tz_off_min);
    return 1;
}

/* -------------------------------------------------------------------- read-outs */
int guru_today_n(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    return gu_count_on(s, today, today);
}

int guru_window_days(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    return gu_prior_days(s, gu_today(now, tz_off_min));
}

int guru_target(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return GU_MIN_TARGET;
    int32_t today = gu_today(now, tz_off_min);
    int p = gu_prior_days(s, today);
    if(p <= 0) return GU_MIN_TARGET;           /* nothing to average yet */

    long sum = 0;
    for(int d = 1; d <= p; d++) sum += gu_count_on(s, today - d, today);

    /* round half up: an average of 4.5 asks for 5, because the tie should lean
     * toward the direction the app is nudging. Integer throughout, so the host
     * test and the device agree exactly. */
    long t = (sum + p / 2) / p;
    return t < GU_MIN_TARGET ? GU_MIN_TARGET : (int)t;
}

int guru_met_today(const GuruState *s, uint32_t now, int tz_off_min){
    return guru_today_n(s, now, tz_off_min) >= guru_target(s, now, tz_off_min);
}

int guru_streak_now(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s || s->streak == 0) return 0;
    int32_t today = gu_today(now, tz_off_min);
    /* today or yesterday keeps it alive -- the day isn't over until it's over. */
    if(today == s->last_active || today == s->last_active + 1) return s->streak;
    return 0;
}

int guru_window_total(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    int sum = 0;
    for(int d = 0; d < GU_WIN; d++) sum += gu_count_on(s, today - d, today);
    return sum;
}

int guru_window_days_active(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    int days = 0;
    for(int d = 0; d < GU_WIN; d++) if(gu_count_on(s, today - d, today) > 0) days++;
    return days;
}

int guru_window_best(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    int best = 0;
    for(int d = 0; d < GU_WIN; d++){
        int c = gu_count_on(s, today - d, today);
        if(c > best) best = c;
    }
    return best;
}

/* ------------------------------------------------------------- the week's fold */
void guru_agg_reset(GuruAgg *a){
    if(!a) return;
    unsigned char *p = (unsigned char *)a;
    for(unsigned i = 0; i < sizeof *a; i++) p[i] = 0;
}

void guru_agg_add(GuruAgg *a, const GuruRec *r){
    if(!a || !r || r->cat >= GU_NCAT) return;
    if(r->flags & GU_F_UNDO){
        /* an undo cancels a check that is already in the fold. Clamped at zero
         * rather than allowed to wrap: a log truncated mid-file, or one whose
         * matching check fell outside the range being folded, would otherwise
         * turn a uint16 into 65535 and make the week look extraordinary. */
        if(a->n) a->n--;
        if(a->cat[r->cat]) a->cat[r->cat]--;
        return;
    }
    if(a->n < 0xFFFF)          a->n++;
    if(a->cat[r->cat] < 0xFFFF) a->cat[r->cat]++;
}

int guru_top_cat(const GuruAgg *a){
    if(!a || a->n == 0) return -1;
    int best = 0;
    for(int c = 1; c < GU_NCAT; c++) if(a->cat[c] > a->cat[best]) best = c;
    return a->cat[best] > 0 ? best : -1;
}

int guru_weakest_cat(const GuruAgg *a){
    if(!a || a->n == 0) return -1;
    int worst = 0;
    for(int c = 1; c < GU_NCAT; c++) if(a->cat[c] < a->cat[worst]) worst = c;
    return worst;
}

/* ---------------------------------------------------------------- rule engine */
/* The rule engine stays quiet until it has seen this much of a week. Advice off
 * two checks is not analysis, it is pattern-matching on noise. */
#define GU_MIN_ADVISE 5
/* ...and it will not call a category neglected until there is enough of a week
 * for the gap to mean something rather than just be early. */
#define GU_MIN_NEGLECT 8

int guru_advise(const GuruAgg *a, int days_active, int window_days){
    if(!a || a->n < GU_MIN_ADVISE) return GA_NONE;                 /* R0 */

    /* R1 -- a whole category got nothing. Ahead of R2 on purpose: naming the
     * empty one is more actionable than naming the crowded one, and a lopsided
     * week is usually both at once. */
    if(a->n >= GU_MIN_NEGLECT){
        int w = guru_weakest_cat(a);
        if(w >= 0 && a->cat[w] == 0) return GA_NEGLECTED;
    }

    /* R2 -- most of the week went into one category. */
    {
        int t = guru_top_cat(a);
        if(t >= 0 && a->cat[t] * 100 >= a->n * 60) return GA_NARROW;
    }

    /* R3 -- big days with nothing between them. Real volume, but landing on at
     * most half the week: the habit is the streak, not the total. */
    if(window_days > 0 && days_active > 0 && days_active * 2 <= window_days)
        return GA_SPOTTY;

    /* R4 -- showed up nearly every day. The thing the app is actually for. */
    if(window_days > 0 && days_active >= window_days - 1) return GA_STEADY;

    return GA_KEEPGOING;                                           /* R5 */
}
