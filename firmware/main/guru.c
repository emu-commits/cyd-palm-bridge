/* guru.c -- pure Guru logic. See guru.h for the contract and the reasoning behind
 * the rolling target.
 *
 * No LVGL, no ESP-IDF, no stdio: this file is the numbers, ui.c is the pixels and
 * the file I/O. Every wall-clock input is injected, so sim/tests/guru_test.c pins
 * every branch on the host in any locale. */
#include "guru.h"
#include "daycal.h"

/* ------------------------------------------------------------------ the pool
 * Specific enough to check off without interpreting. The test for every line was
 * "could two people disagree about whether I did this today?" -- if yes, it is a
 * tip, not a task, and it does not belong here.
 *
 * Read the NOT MEDICAL ADVICE note in guru.h before editing any of this copy.
 * Habits, not outcomes; no dosages; no disease named anywhere in a `name` or a
 * `why`. IDs are assigned once and never reused -- append, never reorder. */
static const GuruTask GU_POOL[] = {
 /* --- Gut ------------------------------------------------------------------ */
 {  1, GU_CAT_GUT,    "One brazil nut",
    "A single nut is the whole ritual. More is not better here." },
 {  2, GU_CAT_GUT,    "Black garlic",
    "Garlic aged until it is sweet and soft. Eaten as-is, a clove at a time." },
 {  3, GU_CAT_GUT,    "Broccoli or sprouts",
    "The sprouts are the concentrated version of the same plant." },
 {  4, GU_CAT_GUT,    "Natto or another ferment",
    "Fermented soybeans. Kimchi, sauerkraut or miso count for this one." },
 {  5, GU_CAT_GUT,    "Kefir or live yoghurt",
    "Live cultures rather than the sweetened, pasteurised sort." },
 {  6, GU_CAT_GUT,    "A plant you have not had",
    "Variety is the point -- people who track this aim at thirty a week." },
 {  7, GU_CAT_GUT,    "Twelve hours between meals",
    "An overnight gap. Dinner early or breakfast late, whichever you prefer." },
 /* --- Movement ------------------------------------------------------------- */
 {  8, GU_CAT_METAB,  "Zone 2, with a 140 burst",
    "Steady enough to hold a conversation, with one hard push in it." },
 {  9, GU_CAT_METAB,  "Twelve-second sprints",
    "All-out and very short, with a long walk back between each one." },
 { 10, GU_CAT_METAB,  "Walk after the big meal",
    "Ten minutes on your feet rather than sitting straight down." },
 { 11, GU_CAT_METAB,  "Sardines or mackerel",
    "Small oily fish. Tinned counts, and is what most people actually eat." },
 { 12, GU_CAT_METAB,  "Mineral water, glass bottle",
    "Volcanic if you can get it; glass because plastic is the part people mind." },
 { 13, GU_CAT_METAB,  "Protein before bed",
    "A small savoury something rather than a sweet one." },
 { 14, GU_CAT_METAB,  "Creatine",
    "The most boring and most studied supplement on the shelf." },
 /* --- Mind ----------------------------------------------------------------- */
 { 15, GU_CAT_COGN,   "Morning sunlight, outdoors",
    "Outside, without glasses or a window in the way. Early is the point." },
 { 16, GU_CAT_COGN,   "A slow breath cycle",
    "Longer out than in, until the hurry goes out of it." },
 { 17, GU_CAT_COGN,   "Ten minutes of stillness",
    "Sitting, doing nothing, not listening to anything. Harder than it sounds." },
 { 18, GU_CAT_COGN,   "Learn something by heart",
    "A few lines, a phone number, a route. Recall is the exercise." },
 { 19, GU_CAT_COGN,   "Read on paper",
    "Long-form and un-scrollable, for as long as it holds you." },
 { 20, GU_CAT_COGN,   "A real conversation",
    "Voice or face, not typing. Length matters less than attention." },
 { 21, GU_CAT_COGN,   "Screens off before bed",
    "An hour of dimmer, duller things first." },
 /* --- Strength ------------------------------------------------------------- */
 { 22, GU_CAT_STRUCT, "One set to real failure",
    "A single movement taken until the next rep will not happen." },
 { 23, GU_CAT_STRUCT, "Hang from a bar",
    "Dead weight, shoulders loose, for as long as your grip lasts." },
 { 24, GU_CAT_STRUCT, "Sit in a deep squat",
    "Heels down, all the way at the bottom. Rest there." },
 { 25, GU_CAT_STRUCT, "Calf raises to burning",
    "Slow, off a step, until they complain." },
 { 26, GU_CAT_STRUCT, "Grip work",
    "Carry something heavy until you have to put it down." },
 { 27, GU_CAT_STRUCT, "Hip and hamstring stretch",
    "The two that shorten from sitting. Held, not bounced." },
 { 28, GU_CAT_STRUCT, "Stand on one leg, eyes shut",
    "Balance is the one that quietly goes. Both legs, near a wall." },
 /* --- Recovery ------------------------------------------------------------- */
 { 29, GU_CAT_RECOV,  "Woke at your usual time",
    "The same hour as yesterday, weekend included." },
 { 30, GU_CAT_RECOV,  "Cold finish to the shower",
    "The last stretch on cold, long enough to change your breathing." },
 { 31, GU_CAT_RECOV,  "Sauna or a long hot bath",
    "Heat, until you have properly had enough of it." },
 { 32, GU_CAT_RECOV,  "No caffeine after noon",
    "It is still working at bedtime whether you feel it or not." },
 { 33, GU_CAT_RECOV,  "Bedroom cold and dark",
    "Colder than feels reasonable, and dark enough to lose your hand." },
 { 34, GU_CAT_RECOV,  "A day without alcohol",
    "Simply a day that did not have any in it." },
 { 35, GU_CAT_RECOV,  "Nose-breathe overnight",
    "Mouth shut. People who chase this tape it; you do not have to." },
};
#define GU_NPOOL ((int)(sizeof(GU_POOL) / sizeof(GU_POOL[0])))

int guru_ntasks(void){ return GU_NPOOL; }

const GuruTask *guru_task(int i){
    return (i >= 0 && i < GU_NPOOL) ? &GU_POOL[i] : 0;
}

const GuruTask *guru_task_by_id(int id){
    for(int i = 0; i < GU_NPOOL; i++) if(GU_POOL[i].id == (uint16_t)id) return &GU_POOL[i];
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
