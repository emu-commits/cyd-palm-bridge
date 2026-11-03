/* dex_test.c -- what the Dex reads, on the host (make -C sim dex, under ASan
 * and UBSan; docs/DEX_PLAN.md section 7). The game's own rules set the marks,
 * so this plays them on the fixture (tests/data/game):
 *   - each event of DEX_PLAN.md 4.1 sets the marks it should, and no others;
 *   - evolving, or letting a creature go, doesn't take a mark back;
 *   - a version 4 save keeps them, and mp_read_marks() reads what mp_load() does;
 *   - a version 3 save loads with the marks made from its party and box;
 *   - mp_read_marks() refuses what mp_load() refuses;
 *   - the speaker's choice of line, for each state.
 * And for tests/dex_check.py, which works the same answers out from mkgame.py's
 * reading of the file and checks every line fits the lower screen:
 *   dex_test where FILE    every critter's Where answer, act by act
 *   dex_test lines FILE    every line he can say
 * Run from sim/. */
#include "dex.h"
#include "monster.h"
#include "monster_play.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME "../tests/data/game/game.bin"
#define SAVE "build/dex_save.bin"

static int fails;
#define CHECK(c, ...) do { if(!(c)){ fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static MonGame G;
static uint8_t HEAD[MG_HEAD_MAX];

static uint16_t mon_named(const char *name){
    for(uint32_t i = 0; i < G.n_mons; i++){
        MgMon m;
        char t[32];
        if(!mg_mon(&G, i, &m) && mg_text(&G, m.name, t, sizeof t) >= 0 && !strcmp(t, name)) return (uint16_t)i;
    }
    CHECK(0, "no creature %s", name);
    return 0;
}
static uint32_t id_of(uint16_t mon){
    MgMon m;
    return mg_mon(&G, mon, &m) ? 0 : m.id;
}
static int count(const uint8_t *set){
    int n = 0;
    for(int i = 0; i < MP_MARK_BYTES; i++) for(int b = 0; b < 8; b++) n += set[i] >> b & 1;
    return n;
}

static uint8_t *slurp(const char *p, long *n){
    FILE *f = fopen(p, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)*n);
    if(fread(b, 1, (size_t)*n, f) != (size_t)*n){ free(b); b = NULL; }
    fclose(f);
    return b;
}
static void spit(const char *p, const uint8_t *b, long n){
    FILE *f = fopen(p, "wb");
    if(f){ fwrite(b, 1, (size_t)n, f); fclose(f); }
}
static void put32(uint8_t *p, uint32_t v){ for(int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* ---- 1. the file's own part ---- */
static void file_part(void){
    CHECK(G.has_dex && G.has_icons, "the fixture has DEXE and ICON");
    uint8_t used[MG_MAX_DEX] = { 0 };
    for(uint32_t i = 0; i < G.n_mons; i++){
        MgDex d;
        char cat[32], desc[160];
        uint8_t ic[MG_ICON_BYTES];
        CHECK(!mg_dex(&G, i, &d), "creature %u's record", (unsigned)i);
        CHECK(d.number >= 1 && d.number <= G.n_mons && !used[d.number - 1], "creature %u's number %u", (unsigned)i, d.number);
        used[d.number - 1] = 1;
        CHECK(mg_text(&G, d.category, cat, sizeof cat) > 0 && mg_text(&G, d.description, desc, sizeof desc) > 0, "its text");
        CHECK(d.height_cm && d.weight_tenths, "its size");
        CHECK(!mg_icon(&G, i, ic, sizeof ic), "creature %u's icon", (unsigned)i);
        CHECK(mg_icon(&G, i, ic, sizeof ic - 1) == MG_ERANGE, "an icon needs the whole record");
    }
    MgDex d;
    uint8_t ic[MG_ICON_BYTES];
    CHECK(mg_dex(&G, G.n_mons, &d) == MG_ERANGE && mg_icon(&G, G.n_mons, ic, sizeof ic) == MG_ERANGE, "past the last creature");
}

/* ---- 2. the marks, event by event (DEX_PLAN.md 4.1) ---- */
static void events(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpBattle b;
    MpLog log;
    uint16_t ember = mon_named("Emberkit"), blaze = mon_named("Blazecat");
    /* receiving the first critter: seen and caught */
    CHECK(!mp_new(w, &G, HEAD, 7, ember), "a new game: %s", w->err);
    CHECK(mp_seen(w, id_of(ember)) && mp_caught(w, id_of(ember)), "the first critter is caught");
    CHECK(count(w->seen) == 1 && count(w->caught) == 1, "and nothing else");
    CHECK(w->starter_id == id_of(ember), "the save knows the first critter");
    /* a wild encounter: seen only */
    w->x = 7; w->y = 12; w->facing = MG_E;
    int met = 0;
    uint16_t wild = 0;
    for(int i = 0; i < 2000 && !met; i++){
        mp_step(w, w->x == 7 ? MG_E : MG_W, &b, &log);
        for(int k = 0; k < log.n; k++)      /* one the player doesn't already own */
            if(log.ev[k].kind == MP_EV_ENCOUNTER && log.ev[k].a != ember){ met = 1; wild = log.ev[k].a; }
    }
    CHECK(met, "no encounter in 2000 steps");
    CHECK(mp_seen(w, id_of(wild)) && !mp_caught(w, id_of(wild)), "an encounter marks seen, not caught");
    /* catching it: caught. Worn down and thrown at until it stays */
    int caught = 0;
    for(int tries = 0; tries < 400 && !caught; tries++){
        b.wild.hp = 1;
        w->party[b.active].hp = w->party[b.active].stat[MG_HP];
        if(mp_turn(w, &b, MP_ACT_CATCH, 0, &log)) break;
        for(int k = 0; k < log.n; k++) caught |= log.ev[k].kind == MP_EV_CAUGHT;
        if(b.over && !caught) break;
    }
    CHECK(caught, "the catch never held");
    CHECK(mp_caught(w, id_of(wild)), "a catch marks caught");
    /* evolving: its new form is caught, the old form stays */
    w->party[0].level = 5;
    MpCreature *c = &w->party[0];
    uint32_t rng = 5;
    mp_make(&G, &rng, ember, 5, c);
    CHECK(!mp_caught(w, id_of(blaze)), "not yet");
    /* a win with enough experience to reach its evolution */
    MpCreature foe;
    mp_make(&G, &rng, mon_named("Puddlop"), 3, &foe);
    CHECK(!mp_battle_start(w, &b, &foe), "a battle");
    c->exp = mp_exp_for(&G, ember, 6) - 1;
    b.wild.hp = 1;
    int evolved = 0;
    for(int t = 0; t < 40 && !b.over; t++){
        b.wild.hp = 1;
        c->hp = c->stat[MG_HP];
        int slot = -1;
        for(int s = 0; s < MP_MOVES; s++) if(c->moves[s] != MP_NO_MOVE && !b.recharge[0][s]){ slot = s; break; }
        mp_turn(w, &b, MP_ACT_MOVE, slot, &log);
        for(int k = 0; k < log.n; k++) evolved |= log.ev[k].kind == MP_EV_EVOLVE;
    }
    CHECK(evolved, "it never evolved");
    CHECK(mp_seen(w, id_of(blaze)) && mp_caught(w, id_of(blaze)), "the evolved form is caught");
    CHECK(mp_caught(w, id_of(ember)), "the old form stays caught");
    /* a keeper's critters: seen, not caught */
    MpWorld *k = calloc(1, sizeof *k);
    CHECK(!mp_new(k, &G, HEAD, 3, ember), "a new game");
    k->x = 3; k->y = 13; k->facing = MG_S;              /* in Fen's sight (monster_test.c's people()) */
    mp_step(k, MG_S, &b, &log);
    uint16_t puddlop = mon_named("Puddlop"), thornet = mon_named("Thornet");
    CHECK(mp_seen(k, id_of(puddlop)) && !mp_caught(k, id_of(puddlop)), "a keeper's first critter is seen");
    CHECK(!mp_seen(k, id_of(thornet)), "their next isn't, until it comes out");
    mp_make(&G, &rng, blaze, 40, &k->party[0]);
    b.active = 0;
    int sent = 0;
    for(int t = 0; t < 80 && !sent && !b.over; t++){
        b.wild.hp = 1;
        int slot = -1;
        for(int s = 0; s < MP_MOVES; s++) if(k->party[0].moves[s] != MP_NO_MOVE && !b.recharge[0][s]){ slot = s; break; }
        mp_turn(k, &b, MP_ACT_MOVE, slot, &log);
        for(int e = 0; e < log.n; e++) sent |= log.ev[e].kind == MP_EV_SENDS;
    }
    CHECK(sent && mp_seen(k, id_of(thornet)) && !mp_caught(k, id_of(thornet)), "the next is seen when it comes out");
    free(k);
    /* the save keeps them, and letting one go doesn't lose them */
    uint8_t seen[MP_MARK_BYTES], got[MP_MARK_BYTES];
    memcpy(seen, w->seen, sizeof seen);
    memcpy(got, w->caught, sizeof got);
    if(w->n_party > 1) w->n_party = 1;
    CHECK(!mp_save(w, SAVE), "saving: %s", w->err);
    MpWorld *v = calloc(1, sizeof *v);
    CHECK(!mp_load(v, &G, HEAD, SAVE), "loading: %s", v->err);
    CHECK(!memcmp(v->seen, seen, sizeof seen) && !memcmp(v->caught, got, sizeof got), "the marks after a round trip");
    CHECK(v->starter_id == id_of(ember), "the first critter after a round trip");
    MpMarks mk;
    CHECK(!mp_read_marks(SAVE, &mk), "reading the marks");
    CHECK(mk.version == MP_SAVE_VERSION && !memcmp(mk.seen, seen, sizeof seen) && !memcmp(mk.caught, got, sizeof got) &&
          mk.starter == id_of(ember) && !memcmp(mk.flags, w->flags, sizeof mk.flags), "mp_read_marks() reads what mp_load() does");

    /* every truncation and flipped bit: refused by the Dex's reading too */
    long n;
    uint8_t *good = slurp(SAVE, &n), *bad = malloc((size_t)n);
    int took = 0;
    for(long i = 0; i < n; i++){ spit(SAVE, good, i); took += !mp_read_marks(SAVE, &mk); }
    CHECK(!took, "%d truncations read", took);
    took = 0;
    for(long i = 0; i < n * 8; i += 3){
        memcpy(bad, good, (size_t)n);
        bad[i / 8] ^= (uint8_t)(1u << (i % 8));
        spit(SAVE, bad, n);
        took += !mp_read_marks(SAVE, &mk);
    }
    CHECK(!took, "%d flipped bits read", took);
    remove(SAVE);
    CHECK(mp_read_marks(SAVE, &mk) == MG_EIO, "no save");

    /* a version 3 save: the marks come from its party and box */
    const long HDR = MP_SAVE_HEADER, REC = MP_SAVE_REC, BAG = 80, MARKS = 72;
    long n3 = n - MARKS;
    memcpy(bad, good, (size_t)(HDR + REC + BAG));
    memcpy(bad + HDR + REC + BAG, good + HDR + REC + BAG + MARKS, (size_t)(n3 - HDR - REC - BAG));
    bad[4] = 3; bad[5] = 0;
    put32(bad + 24, mg_crc32(0, bad + HDR, (uint32_t)(n3 - HDR)));
    put32(bad + 28, mg_crc32(0, bad, 28));
    spit(SAVE, bad, n3);
    CHECK(!mp_load(v, &G, HEAD, SAVE), "a version 3 save: %s", v->err);
    int owned = v->n_party + v->n_box;
    CHECK(owned > 0 && !v->starter_id, "it doesn't know the first critter");
    for(int i = 0; i < owned; i++){
        uint32_t id = id_of(i < v->n_party ? v->party[i].mon : v->box[i - v->n_party].mon);
        CHECK(mp_seen(v, id) && mp_caught(v, id), "a version 3 save's creature %d is marked", i);
    }
    CHECK(count(v->caught) <= owned && count(v->seen) == count(v->caught), "and only those");
    CHECK(!mp_read_marks(SAVE, &mk) && mk.version == 3 && !memcmp(mk.seen, v->seen, sizeof mk.seen) &&
          !memcmp(mk.caught, v->caught, sizeof mk.caught), "the Dex reads a version 3 save the same");
    CHECK(!mp_save(v, SAVE) && !mp_read_marks(SAVE, &mk) && mk.version == MP_SAVE_VERSION &&
          !memcmp(mk.caught, v->caught, sizeof mk.caught), "and saving it makes it version 4");
    remove(SAVE);
    free(good); free(bad); free(v); free(w);
}

/* ---- 3. what he says ---- */

/* a Dex with these marks and no save file behind it */
static void marks_of(Dex *d, const char *names){
    memset(d->marks.seen, 0, sizeof d->marks.seen);
    memset(d->marks.caught, 0, sizeof d->marks.caught);
    d->has_save = 1;
    d->n_seen = d->n_caught = 0;
    for(int p = 0; p < d->n; p++){
        MgMon m;
        char t[32], key[40];
        mg_mon(&d->g, d->mon[p], &m);
        mg_text(&d->g, m.name, t, sizeof t);
        snprintf(key, sizeof key, "+%s+", t);
        int c = strstr(names, key) != NULL;
        snprintf(key, sizeof key, "?%s?", t);
        int e = strstr(names, key) != NULL;
        d->state[p] = c ? DX_CAUGHT : e ? DX_SEEN : DX_NEVER;
        if(d->state[p]) d->n_seen++;
        if(c){ d->marks.caught[m.id / 8] |= (uint8_t)(1u << (m.id % 8)); d->caught[d->n_caught++] = (uint16_t)p; }
        if(c || e) d->marks.seen[m.id / 8] |= (uint8_t)(1u << (m.id % 8));
    }
}

static void lines(void){
    Dex d;
    char t[DX_LINE], n[24];
    uint8_t *head;
    remove(SAVE);
    CHECK(!dx_open(&d, GAME, SAVE), "dx_open: %s", d.g.err);
    head = malloc(d.head_cap);
    CHECK(!d.has_save && !d.n_seen, "no save, nothing marked");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "licence"), "no save: %s", t);
    dx_number(6, n, sizeof n); CHECK(!strcmp(n, "Six"), "%s", n);
    dx_number(33, n, sizeof n); CHECK(!strcmp(n, "Thirty-three"), "%s", n);
    dx_number(40, n, sizeof n); CHECK(!strcmp(n, "Forty"), "%s", n);
    dx_number(19, n, sizeof n); CHECK(!strcmp(n, "Nineteen"), "%s", n);
    /* the fixture: Puddlop, Emberkit > Blazecat, Thornet, Rustmole */
    marks_of(&d, "");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "Not one yet"), "none yet: %s", t);
    marks_of(&d, "+Emberkit+");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "Your Emberkit's line has one left"), "a line one short, on an odd count: %s", t);
    marks_of(&d, "+Puddlop+ +Thornet+");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "Two of five") && strstr(t, "Something"), "a hint: %s", t);
    CHECK(!strstr(t, "Emberkit") && !strstr(t, "Blazecat"), "a hint never names what's unseen: %s", t);
    marks_of(&d, "+Puddlop+ ?Emberkit? ?Blazecat? ?Thornet? ?Rustmole?");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "met them all"), "every one seen: %s", t);
    marks_of(&d, "+Puddlop+ +Emberkit+ +Blazecat+ +Thornet+ +Rustmole+");
    dx_greeting(&d, t, sizeof t, head);
    CHECK(strstr(t, "Five of five") && strstr(t, "nothing left"), "complete: %s", t);
    /* Talk on an entry */
    int pud = 0;
    for(int p = 0; p < d.n; p++) if(d.mon[p] == mon_named("Puddlop")) pud = p;
    marks_of(&d, "?Puddlop?");
    dx_talk(&d, pud, t, sizeof t, head);
    CHECK(!strcmp(t, "Nothing on this one yet."), "seen, not caught: %s", t);
    marks_of(&d, "+Puddlop+");
    dx_talk(&d, pud, t, sizeof t, head);
    CHECK(strstr(t, "levels 2 to 4") || strstr(t, "levels"), "caught: %s", t);
    d.marks.starter = id_of(d.mon[pud]);
    dx_talk(&d, pud, t, sizeof t, head);
    CHECK(strstr(t, "Yours came from"), "the first critter: %s", t);
    dx_talk(&d, -1, t, sizeof t, head);
    CHECK(!strcmp(t, "Nothing on this one yet."), "no entry");
    /* no memory for the search: he still speaks */
    d.marks.starter = 0;
    dx_talk(&d, pud, t, sizeof t, NULL);
    CHECK(strstr(t, "smudged"), "no head: %s", t);
    marks_of(&d, "+Puddlop+ +Thornet+");
    dx_greeting(&d, t, sizeof t, NULL);
    CHECK(strstr(t, "Two of five"), "no head, no hint: %s", t);
    free(head);
    dx_close(&d);
}

/* ---- for tests/dex_check.py ---- */

/* the acts: no flags, then the steps' flags one more at a time, in their order */
static int acts_of(const MonGame *g, uint16_t *flags){
    int n = 0;
    for(uint32_t i = 0; i < g->n_steps; i++){
        int seen = 0;
        for(int k = 0; k < n; k++) seen |= flags[k] == g->steps[i].flag;
        if(!seen && n < MG_MAX_STEPS) flags[n++] = g->steps[i].flag;
    }
    return n;
}

static int dump_where(const char *path){
    Dex d;
    if(dx_open(&d, path, "/nonexistent")){ printf("FAIL: %s\n", d.g.err); return 1; }
    uint8_t *head = malloc(d.head_cap);
    uint16_t flags[MG_MAX_STEPS];
    int acts = acts_of(&d.g, flags);
    for(int act = 0; act <= acts; act++){
        if(act) d.marks.flags[flags[act - 1] / 8] |= (uint8_t)(1u << (flags[act - 1] % 8));
        for(uint32_t mon = 0; mon < d.n; mon++){
            DxWhere w;
            if(dx_where(&d, (uint16_t)mon, &w, head)){ printf("FAIL: where: %s\n", d.g.err); return 1; }
            printf("%d %u %u %u %u", act, (unsigned)mon, w.lo == 255 ? 0 : w.lo, w.hi, w.n_areas + w.more_areas);
            for(int i = 0; i < w.n_areas; i++) printf("|%s", w.area[i]);
            printf("\n");
        }
    }
    free(head);
    dx_close(&d);
    return 0;
}

static int dump_lines(const char *path){
    Dex d;
    char t[DX_LINE];
    if(dx_open(&d, path, "/nonexistent")){ printf("FAIL: %s\n", d.g.err); return 1; }
    uint8_t *head = malloc(d.head_cap);
    uint16_t flags[MG_MAX_STEPS];
    int acts = acts_of(&d.g, flags);
    dx_greeting(&d, t, sizeof t, head);
    printf("%s\n", t);
    d.has_save = 1;
    for(int act = 0; act <= acts; act++){
        if(act) d.marks.flags[flags[act - 1] / 8] |= (uint8_t)(1u << (flags[act - 1] % 8));
        for(int yours = 0; yours < 2; yours++)
            for(int p = 0; p < d.n; p++){
                MgMon m;
                mg_mon(&d.g, d.mon[p], &m);
                memset(d.marks.seen, 0, sizeof d.marks.seen);
                memset(d.marks.caught, 0, sizeof d.marks.caught);
                d.n_seen = d.n_caught = 0;
                for(int q = 0; q < d.n; q++){    /* every count of caught, with this one the last caught */
                    MgMon o;
                    mg_mon(&d.g, d.mon[q], &o);
                    d.state[q] = q <= p ? DX_CAUGHT : DX_NEVER;
                    if(q > p) continue;
                    d.marks.seen[o.id / 8] |= (uint8_t)(1u << (o.id % 8));
                    d.marks.caught[o.id / 8] |= (uint8_t)(1u << (o.id % 8));
                    d.n_seen++;
                    d.caught[d.n_caught++] = (uint16_t)q;
                }
                d.marks.starter = yours ? m.id : 0;
                dx_greeting(&d, t, sizeof t, head);
                printf("%s\n", t);
                dx_talk(&d, p, t, sizeof t, head);
                printf("%s\n", t);
                dx_greeting(&d, t, sizeof t, NULL);
                printf("%s\n", t);
            }
    }
    d.marks.starter = 0;
    dx_talk(&d, 0, t, sizeof t, NULL);
    printf("%s\n", t);
    for(int p = 0; p < d.n; p++) d.state[p] = DX_SEEN;
    d.n_seen = d.n; d.n_caught = 1; d.state[0] = DX_CAUGHT; d.caught[0] = 0;
    dx_greeting(&d, t, sizeof t, head);
    printf("%s\n", t);
    free(head);
    dx_close(&d);
    return 0;
}

int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "where")) return dump_where(argv[2]);
    if(argc == 3 && !strcmp(argv[1], "lines")) return dump_lines(argv[2]);
    if(mg_open(&G, GAME)){ printf("FAIL: %s: %s\n", GAME, G.err); return 1; }
    file_part();
    events();
    lines();
    mg_close(&G);
    if(fails){ printf("dex: FAILED (%d failures)\n", fails); return 1; }
    printf("dex: OK\n");
    return 0;
}
