/* monster_test.c -- the monster game's reader and rules on the host
 * (make -C sim monster, under ASan and UBSan; smoke32 runs it 32-bit).
 *
 *   monster_test dump FILE   the canonical dump: must equal tools/mkgame.py
 *                            --dump, byte for byte (the round trip)
 *   monster_test             the checks, on tests/data/game/game.bin (the
 *                            fixture: tests/data/game/source.py, all ours):
 *     1. the file opens and verifies;
 *     2. each formula against numbers worked by hand (GAME_FORMAT.md 2);
 *     3. walking: water, a ledge, a fence, a wall, a one-way gate, a tree,
 *        warps both ways (and the facing they need), a sign;
 *     4. encounters: the chance per step, the table's creatures and levels;
 *     5. 40 seeded battles against the recorded transcript
 *        (tests/data/game/battle.txt; MONSTER_RECORD=1 rewrites it), with
 *        the invariants checked after every turn;
 *     6. the save: a round trip, every truncation and every flipped bit
 *        refused, an unknown creature refused, a newer version refused;
 *     7. Auto-play (monster_bot.c) on the game that ships
 *        (courses/critters/game.bin): from a new game with each of the
 *        three first critters, the bot alone beats every keeper in the
 *        region, within a step limit, from a fixed seed;
 *     8. the mirror match: a level 6 Vamporm against one, 200 seeds, every
 *        battle ends within 400 turns (the healing move's recharge).
 * Run from sim/. */
#include "monster.h"
#include "monster_bot.h"
#include "monster_play.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME   "../tests/data/game/game.bin"
#define BATTLE "../tests/data/game/battle.txt"
#define REGION "../courses/critters/game.bin"
#define BOT_LIMIT 400000L           /* presses and battle turns, all told (GAME_LENGTH_PLAN.md) */
#define BOT_MAIN_MIN 36             /* the first critter's level when every keeper is beaten, at least (the owner's ask) */
static const char *SAVE = "build/monster_save.bin";   /* MONSTER_SAVE overrides */

static int fails;
#define CHECK(c, ...) do { if(!(c)){ fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static const char *RANGES[] = { "melee", "touch", "ranged", "reach", "reliable", "special" };

static uint32_t rgb888(uint16_t v){
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}

static void text(MonGame *g, MgStr s, char *buf){ mg_text(g, s, buf, 512); }

/* ---- the dump: tools/mkgame.py's dump(), line for line ---- */

static int dump(const char *path){
    MonGame g;
    char t[512];
    if(mg_open(&g, path)){ fprintf(stderr, "%s: %s\n", path, g.err); return 1; }
    uint8_t *scratch = malloc(MG_HEAD_MAX + MG_SPRITE_MAX);
    if(mg_verify(&g, scratch, MG_HEAD_MAX + MG_SPRITE_MAX)){ fprintf(stderr, "%s: %s\n", path, g.err); return 1; }
    for(uint16_t want = 1; want <= 2; want++){
        for(uint32_t i = 0; ; i++){
            uint16_t k, n;
            uint8_t v[512];
            if(mg_meta_entry(&g, i, &k, v, sizeof v - 1, &n)) break;
            if(k != want) continue;
            v[n] = 0;
            printf("%s %s\n", want == 1 ? "title" : "source", (char *)v);
        }
    }
    printf("start area %u at %u,%u facing %u\n", g.start_area, g.start_x, g.start_y, g.start_facing);
    printf("looks %u colours %u crc %08x\n", g.looks, g.look_colours, (unsigned)g.s[MG_LOOK].crc);
    if(g.n_starters){
        printf("starters");
        for(int i = 0; i < g.n_starters; i++) printf(" %u", g.starters[i]);
        printf("\n");
    }
    printf("rules");   for(int i = 0; i < 8; i++) printf(" %u", (unsigned)g.rules[i]);   printf("\n");
    printf("capture"); for(int i = 0; i < 7; i++) printf(" %u", (unsigned)g.capture[i]); printf("\n");
    printf("growth");  for(int i = 0; i < 4; i++) printf(" %u", (unsigned)g.growth[i]);  printf("\n");
    printf("keeper bonus %u\n", (unsigned)g.keeper_bonus);
    printf("money %u %u\n", (unsigned)g.money_start, (unsigned)g.money_per_level);
    for(uint32_t i = 0; i < g.n_items; i++){
        MgItem it;
        mg_item(&g, i, &it);
        text(&g, it.name, t);
        printf("item %u %s effects %u amount %u price %u\n", (unsigned)it.id, t, it.effects, it.amount, (unsigned)it.price);
    }
    if(g.n_steps){
        printf("steps");
        for(int i = 0; i < g.n_steps; i++) printf(" %u:%u>%u", g.steps[i].flag, g.steps[i].table, g.steps[i].to);
        printf("\n");
    }
    for(uint32_t i = 0; i < g.n_types; i++){
        MgStr s;
        mg_type_name(&g, i, &s);
        text(&g, s, t);
        printf("type %s", t);
        for(uint32_t j = 0; j < g.n_types; j++) printf(" %u", g.types[i][j]);
        printf("\n");
    }
    for(uint32_t i = 0; i < g.n_shapes; i++){
        printf("shape %u", (unsigned)i);
        for(int k = 0; k < MG_NSTATS; k++) printf(" %u", g.shapes[i][k]);
        printf("\n");
    }
    for(uint32_t i = 0; i < g.n_statuses; i++){
        MgStatus *st = &g.statuses[i];
        text(&g, st->name, t);
        printf("status %u %s kind %u divisor %u flags %u immune 0x%04x", (unsigned)st->id, t, st->kind,
               st->divisor, st->flags, st->immune);
        if(st->chance || st->duration || st->ranges) printf(" chance %u duration %u ranges %u", st->chance, st->duration, st->ranges);
        if(g.has_sicons){
            uint16_t pal[4];
            uint8_t px[MG_SICON_BYTES];
            mg_status_icon(&g, i, pal, px);
            printf(" mult %u,%u,%u,%u,%u then %u icon %04x%04x%04x %08x", st->mult[0], st->mult[1], st->mult[2], st->mult[3],
                   st->mult[4], st->then, pal[1], pal[2], pal[3], (unsigned)mg_crc32(0, px, MG_SICON_BYTES));
        }
        printf("\n");
    }
    for(uint32_t i = 0; i < g.n_moves; i++){
        MgMove m;
        mg_move(&g, i, &m);
        text(&g, m.name, t);
        printf("move %u %s types %u,%u range %s speed %u power %u acc %u pot %u recharge %u target %u heal %u effects",
               (unsigned)m.id, t, m.type[0], m.type[1], RANGES[m.range], m.speed, m.power, m.accuracy, m.potency,
               m.recharge, m.targets, m.heal);
        const char *sep = " ";
        for(int e = 0; e < 3; e++){
            if(!m.eff_kind[e]) continue;
            printf("%s%u/%u/%u", sep, m.eff_kind[e], m.eff_status[e], m.eff_target[e]);
            if(m.eff_arg[e]) printf("/%d", m.eff_arg[e]);
            sep = " ";
        }
        int any = m.eff_kind[0] || m.eff_kind[1] || m.eff_kind[2];
        printf(any ? "\n" : " \n");
    }
    for(uint32_t i = 0; i < g.n_mons; i++){
        MgMon m;
        mg_mon(&g, i, &m);
        text(&g, m.name, t);
        printf("creature %u %s txmn %u types %u,%u shape %u stage %u catch %u group %u resist %u-%u evolve %u->%u learns",
               (unsigned)m.id, t, m.txmn, m.type[0], m.type[1], m.shape, m.stage, m.catch_rate, m.group,
               m.resist_lo, m.resist_hi, m.evolve_at, m.evolve_to);
        for(uint32_t j = 0; j < m.n_learn; j++){
            uint8_t lv;
            uint16_t mv;
            mg_learn(&g, &m, j, &lv, &mv);
            printf(" %u:%u", lv, mv);
        }
        printf("\n");
    }
    for(uint32_t i = 0; i < g.n_tables; i++){
        uint16_t n;
        mg_enc_rows(&g, i, &n);
        printf("encounter %u", (unsigned)i);
        for(uint32_t j = 0; j < n; j++){
            MgEncRow r;
            mg_enc_row(&g, i, j, &r);
            printf(" %u:%u-%u@%u", r.mon, r.lo, r.hi, r.rate);
        }
        printf("\n");
    }
    uint8_t *head = scratch, chunk[MG_CHUNK_BYTES];
    for(uint32_t i = 0; i < g.n_areas; i++){
        MgArea a;
        mg_area_open(&g, i, &a, head, MG_HEAD_MAX);
        text(&g, a.dir.name, t);
        printf("area %u %s %ux%u bpp %u tiles %u palette", (unsigned)a.dir.id, t, a.w, a.h, a.bpp, a.n_tiles);
        for(uint32_t k = 1; k <= a.colours; k++) printf(" %06x", (unsigned)rgb888(mg_palette(&a, k)));
        printf("\n");
        for(uint32_t k = 0; k < a.n_warps; k++){
            MgWarp w;
            mg_warp(&a, k, &w);
            printf("  warp %u,%u %ux%u -> area %u %u,%u facing %u\n", w.x, w.y, w.w, w.h, w.to, w.tx, w.ty, w.facing);
        }
        for(uint32_t k = 0; k < a.n_zones; k++){
            MgZone z;
            mg_zone(&a, k, &z);
            printf("  zone %u,%u %ux%u encounter %u chance %u\n", z.x, z.y, z.w, z.h, z.table, z.chance);
        }
        for(uint32_t k = 0; k < a.n_signs; k++){
            MgSign s;
            mg_sign(&a, k, &s);
            text(&g, s.text, t);
            printf("  talk %u,%u %ux%u facing %u: %s\n", s.x, s.y, s.w, s.h, s.facing, t);
        }
        for(uint32_t k = 0; k < a.n_people; k++){
            MgPerson p;
            mg_person(&a, k, &p);
            printf("  person %u,%u facing %u sheet %u kind %u sight %u flags %u/%u/%u team", p.x, p.y, p.facing,
                   p.sheet, p.kind, p.sight, p.flag, p.hide_if, p.show_if);
            if(p.mon[0] == MG_NONE16) printf(" -");
            for(int j = 0; j < MG_TEAM && p.mon[j] != MG_NONE16; j++) printf(" %u:%u", p.mon[j], p.level[j]);
            text(&g, p.name, t);
            printf(": %s | ", t);
            text(&g, p.text, t);
            printf("%s | ", t);
            text(&g, p.after, t);
            printf("%s\n", t);
        }
        /* the cells (u16 each) and movement bytes, row by row across the chunks */
        uint32_t cc = 0, mc = 0;
        for(uint32_t y = 0; y < a.h; y++)
            for(uint32_t x = 0; x < a.w; x++){
                mg_chunk(&g, &a, (y / MG_CHUNK) * a.ccols + x / MG_CHUNK, chunk);
                uint32_t j = (y % MG_CHUNK) * MG_CHUNK + x % MG_CHUNK;
                cc = mg_crc32(cc, chunk + 2 * j, 2);
                mc = mg_crc32(mc, chunk + 2 * MG_CHUNK * MG_CHUNK + j, 1);
            }
        printf("  cells crc %08x move crc %08x overlays %u\n", (unsigned)cc, (unsigned)mc, a.n_overlays);
    }
    printf("credits %08x\n", (unsigned)g.s[MG_CRED].crc);
    for(uint32_t i = 0; g.has_dex && i < g.n_mons; i++){
        MgDex d;
        char cat[64];
        mg_dex(&g, i, &d);
        text(&g, d.category, cat);
        text(&g, d.description, t);
        printf("dex %u no %u height %u weight %u %s | %s\n", (unsigned)i, d.number, d.height_cm, d.weight_tenths, cat, t);
    }
    if(g.has_icons) printf("icons %u %08x\n", (unsigned)g.n_mons, (unsigned)g.s[MG_ICON].crc);
    free(scratch);
    mg_close(&g);
    return 0;
}

/* ---- helpers ---- */

static MonGame G;
static uint8_t HEAD[MG_HEAD_MAX];

static uint16_t mon_named(const char *name){
    char t[64];
    for(uint32_t i = 0; i < G.n_mons; i++){
        MgMon m;
        mg_mon(&G, i, &m);
        mg_text(&G, m.name, t, sizeof t);
        if(!strcmp(t, name)) return (uint16_t)i;
    }
    printf("no creature %s\n", name);
    exit(2);
}

static uint16_t move_named(const char *name){
    char t[64];
    for(uint32_t i = 0; i < G.n_moves; i++){
        MgMove m;
        mg_move(&G, i, &m);
        mg_text(&G, m.name, t, sizeof t);
        if(!strcmp(t, name)) return (uint16_t)i;
    }
    printf("no move %s\n", name);
    exit(2);
}

static MgMove move_rec(const char *name){
    MgMove m;
    mg_move(&G, move_named(name), &m);
    return m;
}

/* a creature with every iv 0, for numbers worked by hand */
static MpCreature plain(const char *name, uint8_t level){
    MpCreature c;
    uint32_t rng = 1;
    mp_make(&G, &rng, mon_named(name), level, &c);
    memset(c.iv, 0, sizeof c.iv);
    mp_stats(&G, &c);
    c.hp = c.stat[MG_HP];
    return c;
}

/* put the player at (x, y) facing dir, and press dir once: the event */
static MpEvent press_from(MpWorld *w, int x, int y, int dir){
    MpLog log;
    MpBattle b;
    w->x = (uint8_t)x; w->y = (uint8_t)y; w->facing = (uint8_t)dir;
    mp_step(w, dir, &b, &log);
    MpEvent none = { 0 };
    return log.n ? log.ev[0] : none;
}

/* ---- 2. formulas ---- */

static void formulas(void){
    MpCreature ek = plain("Emberkit", 5), rm = plain("Rustmole", 5), pd = plain("Puddlop", 5), th = plain("Thornet", 5);
    /* Emberkit is a sprite (hp 5, armour 4, dodge 8, melee 5, ranged 7, speed 8): shape x (5 + 7) */
    uint16_t want[MG_NSTATS] = { 60, 48, 96, 60, 84, 96 };
    for(int k = 0; k < MG_NSTATS; k++) CHECK(ek.stat[k] == want[k], "Emberkit stat %d is %u, not %u", k, ek.stat[k], want[k]);
    /* experience: fast 0.8 x 10^3, slow 1.25 x 10^3, default 3^3 */
    CHECK(mp_exp_for(&G, mon_named("Emberkit"), 10) == 800, "fast group at 10");
    CHECK(mp_exp_for(&G, mon_named("Puddlop"), 10) == 1250, "slow group at 10");
    CHECK(mp_exp_for(&G, mon_named("Thornet"), 3) == 27, "default group at 3");
    /* multipliers: fire, venom vs wood, venom = 2 x 2 x 2 x 1 = 8, clamped to 4 */
    MgMove tf = move_rec("Toxic Flare"), sc = move_rec("Scratch"), fb = move_rec("Focus Beam"), em = move_rec("Ember");
    MgMove rest = move_rec("Rest"), qj = move_rec("Quick Jab"), bash = move_rec("Bash");
    CHECK(mp_multiplier(&G, &tf, &th, NULL) == 400, "the clamp: %u", (unsigned)mp_multiplier(&G, &tf, &th, NULL));
    CHECK(mp_multiplier(&G, &em, &pd, NULL) == 50, "fire on water");
    CHECK(mp_multiplier(&G, &sc, &rm, NULL) == 50, "normal on metal");
    CHECK(mp_multiplier(&G, &rest, &rm, NULL) == 100, "no type");
    /* Scratch (melee, 1.00) from Emberkit at 5 on Rustmole (armour 8 x 12 = 96):
     * 60 x 12 x 1.00 x 0.5 / 96 = 3.75 -> 3 */
    uint32_t mult;
    CHECK(mp_damage(&G, &ek, &sc, &rm, &mult) == 3 && mult == 50, "Scratch on Rustmole: %u", (unsigned)mp_damage(&G, &ek, &sc, &rm, &mult));
    /* Focus Beam is reliable: (7 + 5) x 1.00 x 1 / 1 = 12 */
    CHECK(mp_damage(&G, &ek, &fb, &pd, NULL) == 12, "Focus Beam: %u", (unsigned)mp_damage(&G, &ek, &fb, &pd, NULL));
    /* Ember (ranged, 1.25) on Puddlop (dodge 4 x 12 = 48): 84 x 12 x 1.25 x 0.5 / 48 = 13.125 -> 13 */
    CHECK(mp_damage(&G, &ek, &em, &pd, NULL) == 13, "Ember on Puddlop: %u", (unsigned)mp_damage(&G, &ek, &em, &pd, NULL));
    CHECK(mp_damage(&G, &ek, &rest, &pd, NULL) == 0, "a special move does no damage");
    /* speed: 96 x (1 + 3 x 0.25) + 0.96 -> 168; 96 x (1 - 0.75) + 0.96 -> 24 */
    CHECK(mp_speed_score(&ek, &qj) == 168, "Quick Jab's speed %u", (unsigned)mp_speed_score(&ek, &qj));
    CHECK(mp_speed_score(&ek, &bash) == 24, "Bash's speed %u", (unsigned)mp_speed_score(&ek, &bash));
    /* catching Puddlop (rate 90) at full HP: catch_check 30; float 48,505; ours 48,500 */
    MgMon pm;
    mg_mon(&G, pd.mon, &pm);
    uint32_t s = mp_shake_check(&G, &pd, &pm, 100);
    double f = 524325.0 / (8.0 * pow(100.0 / 30.0, 0.25));
    CHECK(s == 48500 && fabs(s - f) / f < 0.001, "shake check %u (float %.1f)", (unsigned)s, f);
    /* at one HP it's easier, and a rate of 0 can't be caught */
    pd.hp = 1;
    CHECK(mp_shake_check(&G, &pd, &pm, 100) > s, "a weak creature is easier to catch");
    MgMon rmm;
    mg_mon(&G, rm.mon, &rmm);
    CHECK(mp_shake_check(&G, &rm, &rmm, 100) == 0, "Rustmole can't be caught");
    /* a wild Emberkit at 9 knows the last four it learnt: Quick Jab, Soothe, Toxic Flare, Bash */
    MpCreature e9 = plain("Emberkit", 9);
    const char *knows[4] = { "Quick Jab", "Soothe", "Toxic Flare", "Bash" };
    for(int i = 0; i < 4; i++) CHECK(e9.moves[i] == move_named(knows[i]), "Emberkit at 9, move %d", i);
    MpCreature e1 = plain("Emberkit", 1);
    CHECK(e1.moves[0] == move_named("Ember") && e1.moves[1] == move_named("Scratch") && e1.moves[2] == MP_NO_MOVE,
          "Emberkit at 1 knows Scratch and Ember");
    CHECK(mp_learn(&G, &e1, move_named("Scratch"), 2) != 0, "a move it knows can't be learnt twice");
    CHECK(mp_learn(&G, &e1, move_named("Bash"), 2) == 0 && e1.moves[2] == move_named("Bash"), "learning into a slot");
    /* ivs stay in range */
    uint32_t rng = 7;
    for(int i = 0; i < 200; i++){
        MpCreature c;
        mp_make(&G, &rng, (uint16_t)(i % G.n_mons), (uint8_t)(1 + i % 30), &c);
        for(int k = 0; k < MG_NSTATS; k++) CHECK(c.iv[k] <= G.growth[0], "an iv over the maximum");
    }
}

/* ---- 3. walking ---- */

static void walking(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    CHECK(mp_new(w, &G, HEAD, 1, mon_named("Emberkit")) == 0, "a new game: %s", w->err);
    CHECK(w->area.index == 0 && w->x == 3 && w->y == 4 && w->facing == MG_S, "the start");
    /* a press in a new direction only turns */
    mp_step(w, MG_E, &b, &log);
    CHECK(log.n == 1 && log.ev[0].kind == MP_EV_TURN && w->x == 3, "turning");
    mp_step(w, MG_E, &b, &log);
    CHECK(log.ev[0].kind == MP_EV_STEP && w->x == 4, "stepping");
    CHECK(press_from(w, 13, 4, MG_E).kind == MP_EV_BUMP, "water is solid");
    MpEvent e = press_from(w, 7, 7, MG_S);
    CHECK(e.kind == MP_EV_HOP && w->x == 7 && w->y == 9, "jumping down the ledge");
    CHECK(press_from(w, 7, 9, MG_N).kind == MP_EV_BUMP, "the ledge can't be climbed");
    CHECK(press_from(w, 3, 16, MG_S).kind == MP_EV_BUMP, "the fence (a line along y 17)");
    CHECK(press_from(w, 3, 17, MG_N).kind == MP_EV_BUMP, "the fence, from the other side");
    CHECK(press_from(w, 12, 11, MG_E).kind == MP_EV_BUMP, "the wall (a line along x 13)");
    CHECK(press_from(w, 12, 3, MG_N).kind == MP_EV_STEP, "the gate, from below");
    CHECK(press_from(w, 11, 2, MG_E).kind == MP_EV_BUMP, "the gate, from the side");
    CHECK(press_from(w, 12, 2, MG_W).kind == MP_EV_STEP, "leaving the gate");
    CHECK(press_from(w, 20, 13, MG_N).kind == MP_EV_BUMP, "a tree trunk");
    CHECK(press_from(w, 0, 0, MG_N).kind == MP_EV_BUMP, "the edge");
    /* the burrow, and back */
    w->x = 22; w->y = 9; w->facing = MG_E;
    mp_step(w, MG_E, &b, &log);
    CHECK(log.n == 2 && log.ev[1].kind == MP_EV_WARP && w->area.index == 1 && w->x == 1 && w->y == 8,
          "into the burrow");
    CHECK(press_from(w, 1, 1, MG_N).kind == MP_EV_BUMP, "the burrow's walls");
    w->x = 1; w->y = 8; w->facing = MG_W;
    mp_step(w, MG_W, &b, &log);
    CHECK(log.n == 2 && log.ev[1].kind == MP_EV_WARP && w->area.index == 0 && w->x == 22 && w->y == 9,
          "back to the meadow");
    /* the mouth needs facing east: stepping onto it from above doesn't warp */
    CHECK(press_from(w, 23, 8, MG_S).kind == MP_EV_STEP && w->area.index == 0, "a warp's facing");
    /* the sign: facing it from below */
    char t[128];
    w->x = 5; w->y = 3; w->facing = MG_N;
    CHECK(mp_interact(w, NULL, &log) == 1 && log.ev[0].kind == MP_EV_SIGN, "reading the sign");
    MgStr s = { log.ev[0].n, log.ev[0].b };
    mg_text(&G, s, t, sizeof t);
    CHECK(!strcmp(t, "Meadow. The burrow is east, past the trees."), "the sign says %s", t);
    w->facing = MG_S;
    CHECK(mp_interact(w, NULL, &log) == 0, "a sign behind the player");
    /* head_cap: the screens size the head to the file's largest; a smaller
     * one is refused, never overrun */
    uint32_t most = 0;
    for(uint32_t i = 0; i < G.n_areas; i++){
        MgAreaDir d;
        CHECK(mg_area_dir(&G, i, &d) == 0, "area %u's directory", (unsigned)i);
        if(d.head_len > most) most = d.head_len;
    }
    w->head_cap = most;
    CHECK(mp_new(w, &G, HEAD, 1, mon_named("Emberkit")) == 0 && w->head_cap == most,
          "a head the size of the largest: %s", w->err);
    w->x = 22; w->y = 9; w->facing = MG_E;
    mp_step(w, MG_E, &b, &log);
    CHECK(w->area.index == 1, "into the burrow with a head the size of the largest");
    MgAreaDir d0;
    mg_area_dir(&G, 0, &d0);
    w->head_cap = d0.head_len - 1;
    CHECK(mp_new(w, &G, HEAD, 1, mon_named("Emberkit")) != 0, "a head one byte short is refused");
    free(w);
}

/* ---- 3b. people (minor version 1): talking, flags, trainers, healers ---- */

static int has_ev(const MpLog *log, int kind){
    for(int i = 0; i < log->n; i++) if(log->ev[i].kind == kind) return 1;
    return 0;
}

static void said(const MpLog *log, const char *want, const char *what){
    char t[128] = "";
    for(int i = 0; i < log->n; i++)
        if(log->ev[i].kind == MP_EV_TALK){
            MgStr s = { log->ev[i].n, log->ev[i].b };
            mg_text(&G, s, t, sizeof t);
        }
    CHECK(!strcmp(t, want), "%s: said \"%s\", not \"%s\"", what, t, want);
}

static void people(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    MgPerson fen, fen2;
    CHECK(mp_new(w, &G, HEAD, 3, mon_named("Emberkit")) == 0, "a new game: %s", w->err);
    CHECK(G.looks == 2 && w->area.n_people == 3, "two looks, and three people in the meadow");
    mg_person(&w->area, 0, &fen);
    mg_person(&w->area, 2, &fen2);
    /* her rematch shares her cell: it comes when she goes */
    CHECK(fen2.x == fen.x && fen2.y == fen.y && fen2.kind == MG_P_TRAINER && fen.hide_if && fen2.show_if == fen.hide_if &&
          fen2.level[0] == 8 && mp_present(w, &fen) && !mp_present(w, &fen2), "Keeper Fen's rematch, not there yet");
    CHECK(fen.kind == MG_P_TRAINER && fen.sight == 2 && fen.mon[1] == mon_named("Thornet") && fen.level[1] == 4 &&
          fen.mon[2] == MG_NONE16, "Keeper Fen's record");
    /* the guard is in the way */
    CHECK(press_from(w, 3, 15, MG_S).kind == MP_EV_BUMP, "the guard blocks the path");
    w->x = 3; w->y = 15; w->facing = MG_S;
    CHECK(mp_interact(w, &b, &log) == 1 && b.over, "talking to the guard");
    said(&log, "Beat Fen first.", "the guard");
    CHECK(w->pface[1] == MG_N, "the guard turns to the player");
    /* Fen sees the player step into her line, two cells off */
    w->x = 3; w->y = 13; w->facing = MG_S;
    mp_step(w, MG_S, &b, &log);
    CHECK(log.n == 5 && log.ev[0].kind == MP_EV_STEP && log.ev[1].kind == MP_EV_SPOTTED && log.ev[2].kind == MP_EV_TALK &&
          log.ev[3].kind == MP_EV_TRAINER && log.ev[4].kind == MP_EV_SENDS && log.ev[4].a == mon_named("Puddlop"),
          "spotted: step, spotted, talk, battle, sends out");
    CHECK(!b.over && b.trainer == 1 && b.n_team == 2 && w->facing == MG_E, "the trainer's battle");
    CHECK(mp_turn(w, &b, MP_ACT_CATCH, 0, &log) == MG_ERANGE, "no catching a keeper's critter");
    CHECK(mp_turn(w, &b, MP_ACT_RUN, 0, &log) == MG_ERANGE, "no running from a keeper");
    /* a strong Emberkit wins it: Puddlop, then Thornet, then the win and the flag */
    uint32_t rng = 9;
    mp_make(&G, &rng, mon_named("Blazecat"), 40, &w->party[0]);
    b.active = 0;
    int sends = 0, turns = 0, nexp = 0;
    uint32_t exp[4] = { 0 };
    while(!b.over && turns++ < 40){
        int slot = 0;
        while(slot < MP_MOVES && (w->party[0].moves[slot] == MP_NO_MOVE || b.recharge[0][slot])) slot++;
        mp_turn(w, &b, MP_ACT_MOVE, slot < MP_MOVES ? slot : -1, &log);
        if(has_ev(&log, MP_EV_SENDS)) sends++;
        for(int i = 0; i < log.n; i++) if(log.ev[i].kind == MP_EV_EXP && nexp < 4) exp[nexp++] = log.ev[i].n;
    }
    CHECK(b.over && sends == 1 && has_ev(&log, MP_EV_WIN) && has_ev(&log, MP_EV_BEATEN), "beating Fen (%d turns)", turns);
    /* the keeper bonus, 150 %: Puddlop 3 (slow) is 3^3 x 1.25 / 3 = 11, so 16; Thornet 4 is 4^3 / 4 = 16, so 24 */
    CHECK(G.keeper_bonus == 150 && nexp == 2 && exp[0] == 16 && exp[1] == 24, "a keeper's experience: %u %u",
          (unsigned)exp[0], (unsigned)exp[1]);
    said(&log, "Fen is beaten.", "Fen after the battle");
    CHECK(mp_flag(w, fen.flag), "Fen's flag is set");
    /* her prize: 10 coins a level of her critters, 3 + 4 */
    CHECK(has_ev(&log, MP_EV_PRIZE) && w->money == G.money_start + 70, "Fen's prize: %u coins", (unsigned)w->money);
    /* beaten: she says her piece and doesn't battle again */
    w->x = 4; w->y = 14; w->facing = MG_E;
    CHECK(mp_interact(w, &b, &log) == 1 && b.over && !has_ev(&log, MP_EV_TRAINER), "Fen won't battle twice");
    said(&log, "Fen is beaten.", "Fen, beaten");
    w->x = 3; w->y = 13; w->facing = MG_S;
    mp_step(w, MG_S, &b, &log);
    CHECK(b.over && !has_ev(&log, MP_EV_SPOTTED), "a beaten trainer doesn't see the player");
    /* the guard has gone */
    CHECK(press_from(w, 3, 15, MG_S).kind == MP_EV_STEP && w->y == 16, "the guard steps aside once Fen is beaten");
    /* the village: the elder's flag, the herald the flag brought, the nurse */
    w->x = 3; w->y = 1; w->facing = MG_N;
    mp_step(w, MG_N, &b, &log);
    CHECK(w->area.index == 2 && w->x == 6 && w->y == 8, "into the village");
    MgZone vz;
    mg_zone(&w->area, 0, &vz);
    CHECK(G.n_steps == 1 && G.steps[0].table == vz.table && mp_table(w, vz.table) == vz.table,
          "the village grass, before the elder");
    w->x = 5; w->y = 3; w->facing = MG_N;
    mp_interact(w, &b, &log);
    said(&log, "Hello, keeper.", "the elder, first");
    /* the elder's flag: the grass steps up a table, and Fen gives way to her rematch */
    CHECK(mp_table(w, vz.table) == G.steps[0].to, "the village grass, after the elder");
    CHECK(!mp_present(w, &fen) && mp_present(w, &fen2), "Fen gone, her rematch there");
    mp_interact(w, &b, &log);
    said(&log, "Again?", "the elder, again");
    w->x = 9; w->y = 3; w->facing = MG_N;
    CHECK(mp_interact(w, &b, &log) == 1, "the herald is there once Fen is beaten");
    said(&log, "Fen is beaten!", "the herald");
    /* the clerk: their words, then the shop */
    w->x = 7; w->y = 3; w->facing = MG_N;
    CHECK(mp_interact(w, &b, &log) == 1 && log.n == 2 && log.ev[0].kind == MP_EV_TALK && log.ev[1].kind == MP_EV_SHOP,
          "the clerk opens the shop");
    w->party[0].hp = 1;
    w->x = 2; w->y = 3; w->facing = MG_N;
    CHECK(mp_interact(w, &b, &log) == 1 && has_ev(&log, MP_EV_HEALED) && w->party[0].hp == w->party[0].stat[MG_HP],
          "the nurse rests the party");
    CHECK(w->has_respawn && w->respawn_area == 2 && w->respawn_x == 2 && w->respawn_y == 3, "and a loss comes back here");
    /* lose a battle: back to the nurse, rested */
    w->x = 6; w->y = 8; w->facing = MG_S;
    mp_place(w, 0, 10, 12, MG_S);
    MpCreature big;
    mp_make(&G, &rng, mon_named("Blazecat"), 60, &big);
    mp_make(&G, &rng, mon_named("Puddlop"), 2, &w->party[0]);
    w->n_party = 1;
    mp_battle_start(w, &b, &big);
    turns = 0;
    while(!b.over && turns++ < 40) mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
    CHECK(has_ev(&log, MP_EV_LOSE) && w->area.index == 2 && w->x == 2 && w->y == 3 && w->facing == MG_N &&
          w->party[0].hp == w->party[0].stat[MG_HP], "losing goes back to the nurse");
    /* a fresh game: Fen battles when spoken to, as well */
    CHECK(mp_new(w, &G, HEAD, 3, mon_named("Emberkit")) == 0, "a new game");
    w->x = 4; w->y = 14; w->facing = MG_E;
    CHECK(mp_interact(w, &b, &log) == 1 && !b.over && b.trainer == 1 && has_ev(&log, MP_EV_TRAINER), "Fen, spoken to");
    free(w);
}

/* ---- items, money and the shop (2.1) ---- */

static void items(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    CHECK(mp_new(w, &G, HEAD, 3, mon_named("Emberkit")) == 0, "a new game: %s", w->err);
    CHECK(G.n_items == 2 && w->money == 100 && !w->bag[0] && !w->bag[1], "a new game's money and bag");
    MgItem tonic, revival;
    mg_item(&G, 0, &tonic);
    mg_item(&G, 1, &revival);
    CHECK(tonic.effects == MG_I_HEAL && tonic.amount == 20 && tonic.price == 50 &&
          revival.effects == (MG_I_HEAL_PCT | MG_I_CURE | MG_I_REVIVE) && revival.amount == 50, "the items' records");
    /* buying */
    CHECK(!mp_buy(w, 0) && !mp_buy(w, 0) && w->money == 0 && w->bag[0] == 2, "two tonics for 100");
    CHECK(mp_buy(w, 0) == MG_ERANGE && mp_buy(w, 1) == MG_ERANGE && mp_buy(w, 9) == MG_ERANGE, "not without the money");
    /* using: a tonic on a full creature does nothing, on a hurt one heals 20 */
    MpCreature *c = &w->party[0];
    uint32_t gain;
    CHECK(!mp_item_helps(w, 0, 0, NULL) && mp_use(w, 0, 0, &log) == MG_ERANGE, "no tonic on a creature at full HP");
    c->hp = 5;
    CHECK(mp_item_helps(w, 0, 0, &gain) && gain == (c->stat[MG_HP] - 5u < 20 ? c->stat[MG_HP] - 5u : 20u), "a tonic would heal %u", (unsigned)gain);
    CHECK(!mp_use(w, 0, 0, &log) && c->hp == 5 + gain && w->bag[0] == 1 && has_ev(&log, MP_EV_ITEM), "a tonic used");
    /* a revival: only on a fainted creature, to half its HP, its poison gone */
    w->bag[1] = 1;
    CHECK(!mp_item_helps(w, 1, 0, NULL), "no revival for a creature still standing");
    c->hp = 0; c->status = 1;
    CHECK(!mp_item_helps(w, 0, 0, NULL), "no tonic for a fainted creature");
    CHECK(!mp_use(w, 1, 0, &log) && c->hp == c->stat[MG_HP] / 2 && !c->status && !w->bag[1], "a revival used");
    /* in a battle: an item is the turn, then the other side acts */
    MpCreature wild;
    uint32_t rng = 4;
    mp_make(&G, &rng, mon_named("Puddlop"), 2, &wild);
    CHECK(!mp_battle_start(w, &b, &wild), "a battle");
    c->hp = 3;
    CHECK(!mp_turn(w, &b, MP_ACT_ITEM, 0 | 0 << 8, &log) && log.ev[0].kind == MP_EV_ITEM && !w->bag[0] &&
          (has_ev(&log, MP_EV_USE) || has_ev(&log, MP_EV_RECHARGE)), "a tonic in a battle, then the wild one's move");
    CHECK(mp_turn(w, &b, MP_ACT_ITEM, 0, &log) == MG_ERANGE, "no tonic left");
    free(w);
}

/* ---- 4. encounters ---- */

static void encounters(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    mp_new(w, &G, HEAD, 99, mon_named("Emberkit"));
    w->x = 7; w->y = 12; w->facing = MG_E;
    uint32_t steps = 0, met = 0, by[8] = { 0 };
    for(int i = 0; i < 20000; i++){
        int dir = w->x == 7 ? MG_E : MG_W;
        w->facing = (uint8_t)dir;
        mp_step(w, dir, &b, &log);
        steps++;
        if(log.n == 2 && log.ev[1].kind == MP_EV_ENCOUNTER){
            met++;
            MpEvent *e = &log.ev[1];
            CHECK(!b.over && b.wild.mon == e->a && b.wild.level == e->b, "a battle starts");
            const char *n[] = { "Emberkit", "Puddlop", "Thornet" };
            const uint8_t lo[] = { 2, 2, 3 }, hi[] = { 4, 3, 5 };
            int k;
            for(k = 0; k < 3 && mon_named(n[k]) != e->a; k++){}
            CHECK(k < 3, "a creature from another table");
            if(k < 3){ by[k]++; CHECK(e->b >= lo[k] && e->b <= hi[k], "a level outside the table's"); }
        }
    }
    /* 11 % a step; 40 / 35 / 25 */
    double rate = (double)met / steps;
    CHECK(rate > 0.10 && rate < 0.12, "%u encounters in %u steps", (unsigned)met, (unsigned)steps);
    CHECK(fabs((double)by[0] / met - 0.40) < 0.04 && fabs((double)by[1] / met - 0.35) < 0.04 &&
          fabs((double)by[2] / met - 0.25) < 0.04, "the table's weights: %u %u %u", (unsigned)by[0], (unsigned)by[1], (unsigned)by[2]);
    /* off the grass, never */
    w->x = 3; w->y = 5; met = 0;
    for(int i = 0; i < 2000; i++){
        int dir = w->y == 5 ? MG_S : MG_N;
        w->facing = (uint8_t)dir;
        mp_step(w, dir, &b, &log);
        if(log.n == 2) met++;
    }
    CHECK(met == 0, "an encounter on the path");
    free(w);
}

/* ---- 5. battles ---- */

static const char *EV[] = {
    "none", "turn", "step", "bump", "hop", "warp", "sign", "encounter", "use", "miss", "damage", "heal",
    "status", "immune", "status-hurt", "status-heal", "status-end", "recharge", "faint", "swap", "need-swap",
    "throw", "shake", "break-free", "caught", "run-fail", "run", "exp", "level", "learn", "learn-full",
    "evolve", "win", "lose",
};

static char *out;
static size_t out_len, out_cap;
static void emit(const char *fmt, ...){
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if(out_len + (size_t)n + 1 > out_cap){ out_cap = (out_cap + (size_t)n + 1) * 2; out = realloc(out, out_cap); }
    memcpy(out + out_len, line, (size_t)n + 1);
    out_len += (size_t)n;
}

static void name_of_mon(uint16_t mon, char *t){ MgMon m; mg_mon(&G, mon, &m); mg_text(&G, m.name, t, 32); }
static void name_of_move(uint16_t mv, char *t){ MgMove m; mg_move(&G, mv, &m); mg_text(&G, m.name, t, 32); }

static void log_events(const MpLog *log){
    char t[32];
    CHECK(!log->overflow, "an action said more than MP_EVENTS things");
    for(int i = 0; i < log->n; i++){
        const MpEvent *e = &log->ev[i];
        t[0] = 0;
        if(e->kind == MP_EV_USE || e->kind == MP_EV_LEARN || e->kind == MP_EV_LEARN_FULL)
            name_of_move(e->kind == MP_EV_USE ? e->a : e->b, t);
        if(e->kind == MP_EV_ENCOUNTER) name_of_mon(e->a, t);
        if(e->kind == MP_EV_EVOLVE) name_of_mon(e->b, t);
        emit("  %s %u %u %u %u %s\n", EV[e->kind], e->side, e->a, e->b, (unsigned)e->n, t);
    }
}

static void invariants(MpWorld *w, MpBattle *b){
    for(int i = 0; i < w->n_party; i++){
        MpCreature *c = &w->party[i];
        CHECK(c->hp <= c->stat[MG_HP] && c->status <= G.n_statuses && c->level >= 1 && c->level <= G.rules[5],
              "party member %d is out of range", i);
        CHECK(c->level == G.rules[5] || c->exp < mp_exp_for(&G, c->mon, c->level + 1u), "experience past the level");
    }
    CHECK(b->wild.hp <= b->wild.stat[MG_HP], "the wild creature's HP");
    CHECK(w->n_party >= 1 && w->n_party <= MP_PARTY && w->n_box <= MP_BOX, "the party's size");
}

static int has_event(const MpLog *log, int kind){
    for(int i = 0; i < log->n; i++) if(log->ev[i].kind == kind) return i + 1;
    return 0;
}

/* the outcomes the recorded battles may not reach */
static void outcomes(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    /* losing: healed, and back at the start */
    mp_new(w, &G, HEAD, 3, mon_named("Emberkit"));
    w->x = 10; w->y = 10;
    w->party[0].hp = 1;
    MpCreature big = plain("Blazecat", 30);
    mp_battle_start(w, &b, &big);
    int lost = 0;
    for(int t = 0; t < 20 && !b.over; t++){
        int s = 0;
        while(s < MP_MOVES && (w->party[0].moves[s] == MP_NO_MOVE || b.recharge[0][s])) s++;
        mp_turn(w, &b, MP_ACT_MOVE, s < MP_MOVES ? s : -1, &log);
        lost |= has_event(&log, MP_EV_LOSE) != 0;
    }
    CHECK(lost && b.over && w->party[0].hp == w->party[0].stat[MG_HP] && w->x == G.start_x && w->y == G.start_y,
          "losing heals the party and goes back to the start");
    /* winning at 8 with one point to go: level 9, Bash with four known, and the evolution */
    w->party[0] = plain("Emberkit", 8);
    w->party[0].exp = mp_exp_for(&G, w->party[0].mon, 9) - 1;
    MpCreature weak = plain("Rustmole", 5);
    weak.hp = 1;
    mp_battle_start(w, &b, &weak);
    int qj = 0;
    while(w->party[0].moves[qj] != move_named("Quick Jab")) qj++;
    CHECK(mp_turn(w, &b, MP_ACT_MOVE, qj, &log) == 0, "the winning move");
    int lf = has_event(&log, MP_EV_LEARN_FULL), ev = has_event(&log, MP_EV_EVOLVE);
    CHECK(has_event(&log, MP_EV_WIN) && has_event(&log, MP_EV_LEVEL) && w->party[0].level == 9, "a level from a win");
    CHECK(lf && log.ev[lf - 1].b == move_named("Bash"), "Bash, with four moves known");
    CHECK(ev && w->party[0].mon == mon_named("Blazecat"), "the evolution at 6 or over");
    /* a poisoned creature stays poisoned after the battle; recover ends */
    CHECK(G.statuses[1].flags & 2 && !(G.statuses[2].flags & 2), "which statuses outlast a battle");
    free(w);
}

static MpWorld *battles(void){
    MpWorld *w = calloc(1, sizeof *w);
    MpLog log;
    MpBattle b;
    out_len = 0;
    mp_new(w, &G, HEAD, 12345, mon_named("Emberkit"));
    w->x = 7; w->y = 12;
    int fought = 0, guard = 0;
    while(fought < 40 && guard++ < 100000){
        int dir = w->x == 7 ? MG_E : MG_W;
        w->facing = (uint8_t)dir;
        mp_step(w, dir, &b, &log);
        if(log.n < 2 || log.ev[1].kind != MP_EV_ENCOUNTER) continue;
        fought++;
        emit("battle %d at %u,%u\n", fought, w->x, w->y);
        log_events(&log);
        for(int turn = 0; !b.over && turn < 200; turn++){
            MpCreature *me = &w->party[b.active];
            int act = MP_ACT_MOVE, arg = -1;
            if(b.need_swap){
                act = MP_ACT_SWAP;
                for(int i = 0; i < w->n_party; i++) if(w->party[i].hp && i != b.active){ arg = i; break; }
            }
            else if(b.wild.hp * 3 < b.wild.stat[MG_HP] && turn % 2 == 0) act = MP_ACT_CATCH;
            else if(me->hp * 5 < me->stat[MG_HP] && turn % 3 == 2) act = MP_ACT_RUN;
            else
                for(int s = 0; s < MP_MOVES; s++)
                    if(me->moves[s] != MP_NO_MOVE && !b.recharge[0][s]){ arg = s; if(mp_rand(&w->rng) & 1) break; }
            int r = mp_turn(w, &b, act, arg, &log);
            CHECK(r == 0, "battle %d turn %d: action %d %d refused", fought, turn, act, arg);
            if(r) break;
            emit(" turn %d: %d %d\n", turn, act, arg);
            log_events(&log);
            invariants(w, &b);
            for(int i = 0; i < log.n; i++)
                if(log.ev[i].kind == MP_EV_LEARN_FULL) mp_learn(&G, &w->party[log.ev[i].a], log.ev[i].b, 0);
        }
        CHECK(b.over, "battle %d never ended", fought);
        emit("party:");
        for(int i = 0; i < w->n_party; i++){
            char t[32];
            name_of_mon(w->party[i].mon, t);
            emit(" %s/%u/%u/%u", t, w->party[i].level, w->party[i].hp, (unsigned)w->party[i].exp);
        }
        emit(" box %u\n", w->n_box);
        if(!w->party[0].hp || (w->party[0].hp * 2 < w->party[0].stat[MG_HP])) mp_heal_party(w);
        w->x = 7; w->y = 12;
    }
    CHECK(fought == 40, "only %d battles", fought);
    /* the refusals */
    MpCreature wild = plain("Puddlop", 3);
    mp_battle_start(w, &b, &wild);
    for(int s = 0; s < MP_MOVES; s++) if(w->party[b.active].moves[s] == MP_NO_MOVE)
        CHECK(mp_turn(w, &b, MP_ACT_MOVE, s, &log) != 0, "an empty slot");
    CHECK(mp_turn(w, &b, MP_ACT_SWAP, b.active, &log) != 0, "swapping to the one already out");
    CHECK(mp_turn(w, &b, MP_ACT_MOVE, -1, &log) != 0, "resting with a move ready");
    b.over = 1;
    CHECK(mp_turn(w, &b, MP_ACT_RUN, 0, &log) != 0, "a battle that's over");

    if(getenv("MONSTER_RECORD")){
        FILE *f = fopen(BATTLE, "w");
        fwrite(out, 1, out_len, f);
        fclose(f);
        printf("  recorded %s (%zu bytes)\n", BATTLE, out_len);
    }
    else{
        FILE *f = fopen(BATTLE, "rb");
        char *want = NULL;
        long n = 0;
        if(f){
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            want = malloc((size_t)n + 1);
            n = (long)fread(want, 1, (size_t)n, f);
            fclose(f);
        }
        int same = want && (size_t)n == out_len && !memcmp(want, out, out_len);
        if(!same){
            size_t i = 0;
            while(want && i < out_len && i < (size_t)n && want[i] == out[i]) i++;
            size_t l = i;
            while(l && out[l - 1] != '\n') l--;
            printf("FAIL: the battles differ from %s at byte %zu:\n  got:  %.80s\n", BATTLE, i, out + l);
            fails++;
        }
        free(want);
    }
    return w;
}

/* ---- 6. the save ---- */

static uint8_t *slurp(const char *p, long *n){
    FILE *f = fopen(p, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if(fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = 0;
    fclose(f);
    return b;
}

static void spit(const char *p, const uint8_t *b, long n){
    FILE *f = fopen(p, "wb");
    fwrite(b, 1, (size_t)n, f);
    fclose(f);
}

static void saves(MpWorld *w){
    MpWorld *v = calloc(1, sizeof *v);
    uint8_t head2[MG_HEAD_MAX];
    /* put something in the box, then a round trip */
    uint32_t rng = 5;
    mp_make(&G, &rng, mon_named("Thornet"), 4, &w->box[w->n_box++]);
    w->party[0].status = 1;
    mp_set_flag(w, 2);
    mp_set_flag(w, 255);
    w->look = 1;
    w->has_respawn = 1; w->respawn_area = 2; w->respawn_x = 4; w->respawn_y = 5; w->respawn_facing = MG_W;
    w->money = 1234; w->bag[1] = 3;
    CHECK(mp_save(w, SAVE) == 0, "saving: %s", w->err);
    CHECK(mp_load(v, &G, head2, SAVE) == 0, "loading: %s", v->err);
    CHECK(v->n_party == w->n_party && v->n_box == w->n_box && v->x == w->x && v->y == w->y &&
          v->facing == w->facing && v->rng == w->rng && v->area.index == w->area.index, "the world after a round trip");
    CHECK(!memcmp(v->flags, w->flags, sizeof w->flags) && v->look == 1 && v->has_respawn && v->respawn_area == 2 &&
          v->respawn_x == 4 && v->respawn_y == 5 && v->respawn_facing == MG_W, "the story after a round trip");
    CHECK(v->money == 1234 && !v->bag[0] && v->bag[1] == 3, "the money and the bag after a round trip");
    for(int i = 0; i < w->n_party; i++)
        CHECK(!memcmp(&v->party[i], &w->party[i], sizeof w->party[i]), "party member %d after a round trip", i);
    for(int i = 0; i < w->n_box; i++)
        CHECK(!memcmp(&v->box[i], &w->box[i], sizeof w->box[i]), "box %d after a round trip", i);
    long n;
    uint8_t *good = slurp(SAVE, &n);
    const long BAG = 80, MARKS = 72;
    CHECK(n == MP_SAVE_HEADER + MP_SAVE_REC * (1 + w->n_party + w->n_box) + BAG + MARKS, "the save is %ld bytes", n);
    /* every truncation, and every flipped bit, refused */
    int took = 0;
    for(long k = 0; k < n; k++){
        spit(SAVE, good, k);
        if(!mp_load(v, &G, head2, SAVE)) took++;
    }
    CHECK(!took, "%d truncations loaded", took);
    took = 0;
    uint8_t *bad = malloc((size_t)n);
    for(long k = 0; k < n * 8; k++){
        memcpy(bad, good, (size_t)n);
        bad[k / 8] ^= (uint8_t)(1u << (k % 8));
        spit(SAVE, bad, n);
        if(!mp_load(v, &G, head2, SAVE)) took++;
    }
    CHECK(!took, "%d bit flips loaded", took);
    printf("  save: %ld bytes; %ld truncations and %ld flipped bits refused\n", n, n, n * 8);
    /* a creature this file doesn't have (the CRCs fixed, so the id is what's checked) */
    memcpy(bad, good, (size_t)n);
    bad[MP_SAVE_HEADER + MP_SAVE_REC + BAG + MARKS] = 77;
    uint32_t crc = mg_crc32(0, bad + MP_SAVE_HEADER, (uint32_t)(n - MP_SAVE_HEADER));
    for(int i = 0; i < 4; i++) bad[24 + i] = (uint8_t)(crc >> (8 * i));
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n);
    CHECK(mp_load(v, &G, head2, SAVE) == MG_EBAD && strstr(v->err, "creature"), "an unknown creature: %s", v->err);
    /* a newer version */
    memcpy(bad, good, (size_t)n);
    bad[4] = MP_SAVE_VERSION + 1;
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n);
    CHECK(mp_load(v, &G, head2, SAVE) == MG_EVERSION, "a newer save");
    /* a look this file doesn't have */
    memcpy(bad, good, (size_t)n);
    bad[MP_SAVE_HEADER + 39] = G.looks;
    crc = mg_crc32(0, bad + MP_SAVE_HEADER, (uint32_t)(n - MP_SAVE_HEADER));
    for(int i = 0; i < 4; i++) bad[24 + i] = (uint8_t)(crc >> (8 * i));
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n);
    CHECK(mp_load(v, &G, head2, SAVE) == MG_EBAD, "a look past the file's");
    /* an item this file doesn't have */
    memcpy(bad, good, (size_t)n);
    bad[MP_SAVE_HEADER + MP_SAVE_REC + 8] = 99;
    crc = mg_crc32(0, bad + MP_SAVE_HEADER, (uint32_t)(n - MP_SAVE_HEADER));
    for(int i = 0; i < 4; i++) bad[24 + i] = (uint8_t)(crc >> (8 * i));
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n);
    CHECK(mp_load(v, &G, head2, SAVE) == MG_EBAD && strstr(v->err, "item"), "an unknown item: %s", v->err);
    /* a version 2 save (before the bag) loads with the starting money and nothing in the bag */
    long n2 = n - BAG - MARKS;
    memcpy(bad, good, MP_SAVE_HEADER + MP_SAVE_REC);
    memcpy(bad + MP_SAVE_HEADER + MP_SAVE_REC, good + MP_SAVE_HEADER + MP_SAVE_REC + BAG + MARKS,
           (size_t)(n2 - MP_SAVE_HEADER - MP_SAVE_REC));
    bad[4] = 2; bad[5] = 0;
    crc = mg_crc32(0, bad + MP_SAVE_HEADER, (uint32_t)(n2 - MP_SAVE_HEADER));
    for(int i = 0; i < 4; i++) bad[24 + i] = (uint8_t)(crc >> (8 * i));
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n2);
    CHECK(mp_load(v, &G, head2, SAVE) == 0 && v->money == G.money_start && !v->bag[1] && mp_flag(v, 2),
          "a version 2 save: %s", v->err);
    /* a version 1 save (before the story's record) still loads, with no flags */
    long n1 = n - MP_SAVE_REC - BAG - MARKS;
    memcpy(bad, good, MP_SAVE_HEADER);
    memcpy(bad + MP_SAVE_HEADER, good + MP_SAVE_HEADER + MP_SAVE_REC + BAG + MARKS, (size_t)(n1 - MP_SAVE_HEADER));
    bad[4] = 1; bad[5] = 0;
    crc = mg_crc32(0, bad + MP_SAVE_HEADER, (uint32_t)(n1 - MP_SAVE_HEADER));
    for(int i = 0; i < 4; i++) bad[24 + i] = (uint8_t)(crc >> (8 * i));
    crc = mg_crc32(0, bad, 28);
    for(int i = 0; i < 4; i++) bad[28 + i] = (uint8_t)(crc >> (8 * i));
    spit(SAVE, bad, n1);
    CHECK(mp_load(v, &G, head2, SAVE) == 0 && !mp_flag(v, 2) && !v->has_respawn && !v->look &&
          v->n_party == w->n_party, "a version 1 save: %s", v->err);
    /* a crash after the .tmp was written whole, before the swap: it's promoted */
    remove(SAVE);
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", SAVE);
    spit(tmp, good, n);
    CHECK(mp_load(v, &G, head2, SAVE) == 0, "a finished .tmp promoted: %s", v->err);
    remove(SAVE);
    free(bad);
    free(good);
    free(v);
}

/* ---- 7. Auto-play ---------------------------------------------------------- */

/* The bot plays a new game with first critter `pick` from `seed`, feeding its
 * choices straight to the rules, as the screens do with presses. 1 if it
 * beat every keeper within BOT_LIMIT actions. */
static long all_lost, all_lost_k;
static int bot_plays(MonGame *g, int pick, uint32_t seed){
    uint32_t cap = 0;
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgAreaDir d;
        if(!mg_area_dir(g, i, &d) && d.head_len > cap) cap = d.head_len;
    }
    uint8_t *head = malloc(cap), *scratch = malloc(cap);
    MpWorld *w = calloc(1, sizeof *w);
    MbBot *b = malloc(sizeof *b);
    MpBattle bt;
    MpLog log;
    long n, tidied = 0, battles = 0, keepers = 0, lost = 0, lost_k = 0, caught = 0, stuck = 0, learnt = 0, bought = 0, used = 0;
    /* the grind (BACKLOG.md, "progress without grinding"): wild battles fought
     * only to train, between one keeper beaten and the next, and the longest */
    long trained = 0, streak = 0, worst_streak = 0, train_all = 0;
    char worst_at[48] = "";
    int training = 0;
    int ok = 0;
    uint8_t met[32] = { 0 };        /* the creatures it has met, by position */
    char who[32] = "?";
    w->head_cap = cap;
    uint16_t starter = g->n_starters ? g->starters[pick % g->n_starters] : 0;
    CHECK(!mp_new(w, g, head, seed, starter), "bot: a new game: %s", w->err);
    CHECK(!mb_init(b, g, seed, scratch, cap), "bot: reading the region: %s", g->err);
    { MgMon m; if(!mg_mon(g, starter, &m)) mg_text(g, m.name, who, sizeof who); }
    bt.over = 1;
    int bad = fails;
    for(n = 0; n < BOT_LIMIT && fails == bad; n++){
        int r;
        if(!bt.over){
            int act, arg;
            mb_battle(b, w, &bt, &act, &arg);
            r = mp_turn(w, &bt, act, arg, &log);
            CHECK(r == 0, "bot: a battle action (%d, %d) the rules refuse: %d %s", act, arg, r, w->err);
        }
        else{
            int d, k = mb_world(b, w, &d);
            training = !strncmp(b->why, "training", 8);
            if(getenv("BOT_VERBOSE") && n > BOT_LIMIT - 12)
                printf("    %ld: area %u at %u,%u facing %u: %d dir %d (%s)\n", n, w->area.index, w->x, w->y, w->facing, k, d, b->why);
            if(k == MB_DONE){ ok = 1; break; }
            if(k == MB_TIDY){ tidied++; continue; }
            CHECK(k >= 0, "bot: the map: %d", k);
            if(k == MB_STUCK) stuck++;
            r = k == MB_TALK ? mp_interact(w, &bt, &log) : mp_step(w, d, &bt, &log);
            CHECK(r >= 0, "bot: %s: %d %s", k == MB_TALK ? "talking" : "a step", r, w->err);
        }
        int fought = b->fighting;
        mb_events(b, &bt, &log);
        for(int i = 0; i < log.n; i++){
            const MpEvent *e = &log.ev[i];
            if(e->kind == MP_EV_ENCOUNTER || e->kind == MP_EV_TRAINER) battles++;
            if(e->kind == MP_EV_ENCOUNTER && training && !mb_done(b, w)){ trained++; train_all++; }
            if(e->kind == MP_EV_BEATEN){
                if(trained > worst_streak){
                    MgAreaDir ad;
                    MgPerson pp;
                    char an[24] = "?", pn[24] = "?";
                    if(!mg_area_dir(g, w->area.index, &ad)) mg_text(g, ad.name, an, sizeof an);
                    mg_person(&w->area, e->a, &pp);
                    mg_text(g, pp.name, pn, sizeof pn);
                    worst_streak = trained;
                    snprintf(worst_at, sizeof worst_at, "%s, %s", pn, an);
                }
                trained = 0;
            }
            if((e->kind == MP_EV_ENCOUNTER || e->kind == MP_EV_SENDS) && e->a < 256) met[e->a / 8] |= (uint8_t)(1u << (e->a % 8));
            if(e->kind == MP_EV_BEATEN) keepers++;
            if(e->kind == MP_EV_LOSE){ lost++; lost_k += bt.trainer != 0; }
            if(getenv("BOT_VERBOSE") && (e->kind == MP_EV_LOSE || e->kind == MP_EV_BEATEN)){
                printf("    %6ld %s keeper %d (top %d margin %d):", n, e->kind == MP_EV_LOSE ? "lost to" : "beat", fought,
                       fought ? b->people[fought - 1].top : -1, fought ? b->people[fought - 1].margin : -1);
                for(int j = 0; j < w->n_party; j++) printf(" %u/%u", w->party[j].level, w->party[j].hp);
                printf("  (%s)\n", b->why);
            }
            if(e->kind == MP_EV_CAUGHT) caught++;
            if(e->kind == MP_EV_ITEM) used++;
            if(e->kind == MP_EV_SHOP){         /* as the screens do: buy what it asks for, then leave */
                int it;
                while((it = mb_shop(b, w)) >= 0){
                    CHECK(!mp_buy(w, (uint16_t)it), "bot: a purchase the rules refuse");
                    bought++;
                    if(bought > 10000) break;
                }
            }
            if(e->kind == MP_EV_LEARN_FULL){
                int slot = mb_learn(b, w, e->a, e->b);
                if(slot >= 0){ mp_learn(g, &w->party[e->a], e->b, slot); learnt++; }
            }
        }
    }
    char party[160] = "";
    for(int i = 0; i < w->n_party; i++){
        char t[32];
        MgMon m;
        if(mg_mon(g, w->party[i].mon, &m) || mg_text(g, m.name, t, sizeof t) < 0) snprintf(t, sizeof t, "?");
        size_t l = strlen(party);
        snprintf(party + l, sizeof party - l, "%s%s %u", i ? ", " : "", t, w->party[i].level);
    }
    /* the Dex's marks after a whole game: all it met seen, all it owns caught */
    for(uint32_t i = 0; i < g->n_mons && i < 256; i++){
        MgMon m;
        if(mg_mon(g, i, &m)) continue;
        if(met[i / 8] >> (i % 8) & 1) CHECK(mp_seen(w, m.id), "bot: met creature %u, not marked seen", (unsigned)i);
    }
    for(int i = 0; i < w->n_party + w->n_box; i++){
        MgMon m;
        if(!mg_mon(g, i < w->n_party ? w->party[i].mon : w->box[i - w->n_party].mon, &m))
            CHECK(mp_caught(w, m.id) && mp_seen(w, m.id), "bot: owns creature %u, not marked caught", (unsigned)m.id);
    }
    int main_lv = w->party[0].level;
    for(int i = 1; i < w->n_party; i++) if(w->party[i].level > main_lv) main_lv = w->party[i].level;
    if(ok){       /* the end-game: all the keepers, then every critter caught (the fixture has nowhere to hunt) */
        CHECK(mb_done(b, w), "bot: stopped with a keeper left");
        CHECK(!mb_dex_left(b, w), "bot: stopped with %d not caught: %s", mb_dex_left(b, w), b->why);
    }
    CHECK(!ok || main_lv >= BOT_MAIN_MIN, "bot: the best critter ends at level %d, under %d", main_lv, BOT_MAIN_MIN);
    all_lost += lost; all_lost_k += lost_k;
    (void)streak;
    printf("  grind with %s, seed %u: %ld wild battles to train before keepers, at most %ld before one (%s)\n",
           who, (unsigned)seed, train_all, worst_streak, worst_at);
    printf("  bot with %s, seed %u: %s after %ld actions; %ld battles, %ld keepers, %ld lost, "
           "(%ld to keepers), %ld caught, %ld moves relearnt, %ld stuck, %ld bought, %ld used, %ld moved, box %u, %u coins; party %s\n",
           who, (unsigned)seed, ok ? "every keeper beaten, the Dex full" : "NOT finished", n, battles, keepers, lost, lost_k, caught,
           learnt, stuck, bought, used, tidied, (unsigned)w->n_box, (unsigned)w->money, party);
    if(!ok) printf("  bot: last doing: %s, in area %u at %u,%u\n", b->why, w->area.index, w->x, w->y);
    mb_free(b);
    free(b); free(w); free(head); free(scratch);
    return ok;
}

/* Release and the box: nobody is ever left without a critter that can fight,
 * and the Dex's marks stay. */
static void release_rules(MonGame *g){
    uint32_t cap = 0;
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgAreaDir d;
        if(!mg_area_dir(g, i, &d) && d.head_len > cap) cap = d.head_len;
    }
    uint8_t *head = malloc(cap);
    MpWorld *w = calloc(1, sizeof *w);
    w->head_cap = cap;
    CHECK(!mp_new(w, g, head, 7, g->n_starters ? g->starters[0] : 0), "release: a new game: %s", w->err);
    MgMon m0;
    CHECK(!mg_mon(g, w->party[0].mon, &m0), "release: the starter");
    CHECK(mp_release(w, 0, 0) == MG_ERANGE && w->n_party == 1, "release: the only critter stays");
    CHECK(mp_to_box(w, 0) == MG_ERANGE, "release: nor can it go to the box");
    w->party[1] = w->party[0];
    w->n_party = 2;
    w->party[1].hp = 0;
    CHECK(mp_release(w, 0, 0) == MG_ERANGE, "release: not the only one that can fight");
    CHECK(!mp_release(w, 0, 1) && w->n_party == 1, "release: a fainted one can go, if another can fight");
    CHECK(mp_caught(w, m0.id), "release: the Dex's mark stays");
    w->party[1] = w->party[0];
    w->party[1].level = 9;
    w->n_party = 2;
    CHECK(!mp_to_box(w, 1) && w->n_party == 1 && w->n_box == 1 && w->box[0].level == 9, "release: to the box");
    CHECK(mp_release(w, 1, 1) == MG_ERANGE, "release: the box has one");
    CHECK(!mp_from_box(w, 0) && w->n_party == 2 && !w->n_box, "release: back from the box");
    CHECK(mp_from_box(w, 0) == MG_ERANGE, "release: an empty box");
    for(int i = 0; i < MP_BOX; i++) w->box[i] = w->party[0];
    w->n_box = MP_BOX;
    CHECK(mp_to_box(w, 1) == MG_ERANGE && w->n_party == 2, "release: a full box takes no more");
    w->box[3].level = 77;
    CHECK(!mp_release(w, 1, 2) && w->n_box == MP_BOX - 1 && w->box[2].level == 77, "release: from the box, the rest close up");
    while(w->n_party < MP_PARTY) w->party[w->n_party++] = w->party[0];
    CHECK(mp_from_box(w, 0) == MG_ERANGE, "release: a full party takes no more");
    free(w); free(head);
}

static void autoplay(void){
    MonGame g;
    const char *region = getenv("MONSTER_REGION") ? getenv("MONSTER_REGION") : REGION;   /* another game file to measure */
    if(mg_open(&g, region)){ CHECK(0, "%s: %s", region, g.err); return; }
    release_rules(&g);
    /* BOT_SEEDS=n plays each starter on seeds 1..n (a measure, not the gate: the
     * gate is seed 1); the losses' total is printed at the end */
    int seeds = getenv("BOT_SEEDS") ? atoi(getenv("BOT_SEEDS")) : 1;
    for(int sd = 1; sd <= (seeds > 0 ? seeds : 1); sd++)
        for(int pick = 0; pick < (g.n_starters ? g.n_starters : 3); pick++)
            if(!getenv("BOT_ONLY") || atoi(getenv("BOT_ONLY")) == pick) CHECK(bot_plays(&g, pick, (uint32_t)sd), "bot: didn't finish the region with first critter %d, seed %d", pick, sd);
    printf("  bot: %ld losses in all, %ld of them to keepers\n", all_lost, all_lost_k);
    mg_close(&g);
}

/* 8. the mirror match (2026-11-05): a level 6 Vamporm against a level 6
 * Vamporm, on the game that ships, with a random move each turn for both
 * sides, must end within MIRROR_TURNS on every seed. Before the refresh edit
 * (tools/game_region.json move_edits) a sixth of these never ended: healing
 * undid every hit. */
#define MIRROR_SEEDS 200
#define MIRROR_TURNS 400
static uint16_t mon_in(MonGame *g, const char *name){
    for(uint32_t i = 0; i < g->n_mons; i++){ MgMon m; mg_mon(g, (uint16_t)i, &m); char t[32]; mg_text(g, m.name, t, 32); if(!strcmp(t, name)) return (uint16_t)i; }
    return 0xFFFF;
}
static void mirror(void){
    MonGame g;
    if(mg_open(&g, REGION)){ CHECK(0, "%s: %s", REGION, g.err); return; }
    uint16_t vam = mon_in(&g, "Vamporm");
    CHECK(vam != 0xFFFF, "the mirror match: no Vamporm in the game");
    static uint8_t head[MG_HEAD_MAX];
    int unended = 0, longest = 0;
    for(uint32_t seed = 1; seed <= MIRROR_SEEDS && vam != 0xFFFF; seed++){
        MpWorld *w = calloc(1, sizeof *w);
        MpBattle b;
        MpLog log;
        CHECK(!mp_new(w, &g, head, seed, vam), "mirror: a new game: %s", w->err);
        uint32_t r1 = seed * 7 + 3, r2 = seed * 13 + 5;
        CHECK(!mp_make(&g, &r1, vam, 6, &w->party[0]) && !mp_stats(&g, &w->party[0]), "mirror: our critter");
        w->party[0].hp = w->party[0].stat[MG_HP];
        w->n_party = 1;
        MpCreature wild;
        CHECK(!mp_make(&g, &r2, vam, 6, &wild) && !mp_stats(&g, &wild), "mirror: the wild critter");
        wild.hp = wild.stat[MG_HP];
        CHECK(!mp_battle_start(w, &b, &wild), "mirror: a battle: %s", w->err);
        int turn = 0;
        for(; !b.over && turn < MIRROR_TURNS; turn++){
            int arg = -1;
            for(int k = 0; k < MP_MOVES; k++)
                if(w->party[b.active].moves[k] != MP_NO_MOVE && !b.recharge[0][k]){ arg = k; if(mp_rand(&w->rng) & 1) break; }
            CHECK(mp_turn(w, &b, MP_ACT_MOVE, arg, &log) == 0, "mirror: seed %u turn %d refused", (unsigned)seed, turn);
            if(b.need_swap) break;
        }
        if(!b.over) unended++;
        if(turn > longest) longest = turn;
        free(w);
    }
    CHECK(unended == 0, "mirror: %d of %d level 6 Vamporm battles never ended in %d turns", unended, MIRROR_SEEDS, MIRROR_TURNS);
    printf("  mirror: %d level 6 Vamporm battles, %d unended, the longest %d turns\n", MIRROR_SEEDS, unended, longest);
    mg_close(&g);
}

/* 9. the move effects (2.3, EFFECTS_PLAN.md), on the game that ships: each rule
 * in a battle made for it, a move of our choosing against one of the foe's. */
static MonGame EG;
static int st_of(int kind, const char *name){         /* a status of this kind (and name, if given), or -1 */
    for(uint32_t i = 0; i < EG.n_statuses; i++){
        char t[32];
        mg_text(&EG, EG.statuses[i].name, t, 32);
        if(EG.statuses[i].kind == kind && (!name || !strcmp(t, name))) return (int)i;
    }
    return -1;
}
static int mv_of(const char *name){
    for(uint32_t i = 0; i < EG.n_moves; i++){ MgMove m; char t[32]; mg_move(&EG, i, &m); mg_text(&EG, m.name, t, 32); if(!strcmp(t, name)) return (int)i; }
    return -1;
}
static int ev_count(const MpLog *log, int kind){ int n = 0; for(int i = 0; i < log->n; i++) n += log->ev[i].kind == kind; return n; }
static int ev_side(const MpLog *log, int kind, int side){ int n = 0; for(int i = 0; i < log->n; i++) n += log->ev[i].kind == kind && log->ev[i].side == side; return n; }

/* a battle: our level-lv creature mon knowing only move ours, the foe knowing only theirs */
static void eff_battle(MpWorld *w, MpBattle *b, uint8_t *head, uint32_t seed, int mon, int ours, int theirs, int lv){
    CHECK(!mp_new(w, &EG, head, seed, (uint16_t)mon), "effects: a new game: %s", w->err);
    uint32_t r = seed;
    mp_make(&EG, &r, (uint16_t)mon, (uint8_t)lv, &w->party[0]);
    MpCreature foe;
    mp_make(&EG, &r, (uint16_t)mon, (uint8_t)lv, &foe);
    for(int s = 0; s < MP_MOVES; s++){ w->party[0].moves[s] = MP_NO_MOVE; foe.moves[s] = MP_NO_MOVE; }
    w->party[0].moves[0] = (uint16_t)ours;
    foe.moves[0] = (uint16_t)theirs;
    w->n_party = 1;
    mp_battle_start(w, b, &foe);
}

static void effects(void){
    if(mg_open(&EG, REGION)){ CHECK(0, "%s: %s", REGION, EG.err); return; }
    static uint8_t head[MG_HEAD_MAX];
    MpWorld *w = calloc(1, sizeof *w);
    MpBattle b;
    MpLog log;
    int mon = EG.n_starters ? EG.starters[0] : 0, n = 0;
    int ex = st_of(MG_S_STATS, "Exhausted"), cu = st_of(MG_S_STATS, "Charged Up"), sl = st_of(MG_S_ASLEEP, NULL);
    int stk = st_of(MG_S_WEAKEN, "Stuck"), fe = st_of(MG_S_FESTER, NULL), le = st_of(MG_S_LEECH, NULL);
    int dh = st_of(MG_S_DIEHARD, NULL), sh = st_of(MG_S_THORNS, NULL), rv = st_of(MG_S_REVENGE, NULL);
    int tackle = mv_of("Struggle");
    CHECK(ex >= 0 && cu >= 0 && sl >= 0 && stk >= 0 && fe >= 0 && le >= 0 && dh >= 0 && sh >= 0 && rv >= 0 && tackle >= 0,
          "effects: the shipped game lacks a status or Struggle (%d %d %d %d %d %d %d %d %d %d)", ex, cu, sl, stk, fe, le, dh, sh, rv, tackle);
    if(ex < 0 || cu < 0 || sl < 0 || stk < 0 || fe < 0 || le < 0 || dh < 0 || sh < 0 || rv < 0 || tackle < 0){ free(w); mg_close(&EG); return; }

    /* stats: exhausted halves melee and ranged; charged up doubles all five, then becomes exhausted on use */
    eff_battle(w, &b, head, 3, mon, tackle, tackle, 20);
    MpCreature v, *me = &w->party[0];
    me->status = (uint8_t)(ex + 1);
    mp_view(&EG, &b, 0, me, &v);
    CHECK(v.stat[MG_MELEE_ST] == me->stat[MG_MELEE_ST] / 2 && v.stat[MG_RANGED_ST] == me->stat[MG_RANGED_ST] / 2 &&
          v.stat[MG_ARMOUR] == me->stat[MG_ARMOUR], "effects: exhausted's view");
    me->status = (uint8_t)(cu + 1);
    mp_view(&EG, &b, 0, me, &v);
    CHECK(v.stat[MG_SPEED] == me->stat[MG_SPEED] * 2 && v.stat[MG_ARMOUR] == me->stat[MG_ARMOUR] * 2, "effects: charged up's view");
    mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
    CHECK(me->status == ex + 1, "effects: charged up becomes exhausted once used (now %d)", me->status);
    mp_turn(w, &b, MP_ACT_MOVE, -1, &log);       /* resting, Struggle recharging: exhausted stays */
    b.recharge[0][0] = 0;
    mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
    CHECK(!me->status || me->hp == 0, "effects: exhausted ends with the next move (now %d)", me->status);

    /* asleep: no move while it lasts, never more than its duration, and it ends */
    int longest = 0;
    for(uint32_t seed = 1; seed <= 200; seed++){
        eff_battle(w, &b, head, seed, mon, tackle, tackle, 50);
        b.wild.status = (uint8_t)(sl + 1);
        int t = 0;
        for(; b.wild.status == sl + 1 && t < 20 && !b.over; t++){
            b.recharge[0][0] = 0;
            w->party[0].hp = w->party[0].stat[MG_HP]; b.wild.hp = b.wild.stat[MG_HP];
            mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
            CHECK(ev_side(&log, MP_EV_ASLEEP, 1) == 1 && !ev_side(&log, MP_EV_USE, 1), "effects: asleep moves, seed %u", (unsigned)seed);
        }
        if(t > longest) longest = t;
    }
    CHECK(longest >= 2 && longest <= EG.statuses[sl].duration, "effects: asleep lasts %d turns at most, its duration %u", longest, EG.statuses[sl].duration);

    /* stuck: its melee and touch moves at half power and potency */
    {
        MgMove m;
        mg_move(&EG, (uint32_t)mv_of("Gnaw"), &m);
        uint16_t p0 = m.power;
        me->status = (uint8_t)(stk + 1);
        mp_view_move(&EG, me, &m);
        CHECK(m.power == p0 / 2, "effects: stuck halves a touch move's power (%u from %u)", m.power, p0);
    }

    /* festering: no healing; lifeleech: a drain each turn that ends when the other side leaves */
    eff_battle(w, &b, head, 5, mon, tackle, tackle, 30);
    me->status = (uint8_t)(fe + 1);
    me->hp = me->stat[MG_HP] / 2;
    for(uint32_t i = 0; i < EG.n_items; i++){
        MgItem it;
        mg_item(&EG, i, &it);
        int helps = mp_item_helps(w, (uint16_t)i, 0, NULL);
        if((it.effects & (MG_I_HEAL | MG_I_HEAL_PCT)) && !(it.effects & (MG_I_CURE | MG_I_REVIVE)))
            CHECK(!helps, "effects: a potion helps a festering critter");
        if((it.effects & MG_I_CURE) && !(it.effects & MG_I_REVIVE)) CHECK(helps, "effects: a cure doesn't help a festering critter");
    }
    eff_battle(w, &b, head, 6, mon, tackle, tackle, 30);
    b.wild.status = (uint8_t)(le + 1);
    w->party[0].hp = (uint16_t)(w->party[0].stat[MG_HP] / 2);
    b.recharge[0][0] = 2;               /* resting: nothing ready */
    mp_turn(w, &b, MP_ACT_MOVE, -1, &log);
    CHECK(ev_count(&log, MP_EV_DRAIN) == 1, "effects: lifeleech drains at the turn's end");

    /* diehard: a fainting blow leaves 1 HP; thorns and revenge strike back */
    for(n = 0; n < 50; n++){
        eff_battle(w, &b, head, 100 + (uint32_t)n, mon, tackle, tackle, 30);
        b.wild.status = (uint8_t)(dh + 1);
        b.wild.hp = 1;
        mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
        if(ev_count(&log, MP_EV_DAMAGE)) break;
    }
    CHECK(ev_count(&log, MP_EV_HOLD_ON) == 1 && b.wild.hp == 1 && !b.wild.status, "effects: diehard holds on with 1 HP");
    for(n = 0; n < 50; n++){
        eff_battle(w, &b, head, 200 + (uint32_t)n, mon, tackle, tackle, 30);
        b.wild.status = (uint8_t)(rv + 1);
        mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
        if(ev_count(&log, MP_EV_HIT_BACK)) break;
    }
    CHECK(ev_count(&log, MP_EV_HIT_BACK) >= 1, "effects: revenge strikes back");

    /* the battle's end: a status that doesn't outlast it goes, and so does a switched type */
    eff_battle(w, &b, head, 9, mon, tackle, tackle, 30);
    me->xtype = 1;
    b.wild.hp = 1;
    me->status = (uint8_t)(stk + 1);
    for(int t = 0; t < 20 && !b.over; t++){ b.recharge[0][0] = 0; mp_turn(w, &b, MP_ACT_MOVE, 0, &log); }
    CHECK(b.over && !me->xtype && !me->sturn && !me->status, "effects: the battle's end clears stuck and the switched type");

    /* a move that disappears: out of reach, then it lands */
    int rd = mv_of("Rift Dash");
    if(rd >= 0){
        int landed = 0;
        for(n = 0; n < 40 && !landed; n++){
            eff_battle(w, &b, head, 300 + (uint32_t)n, mon, rd, tackle, 40);
            mp_turn(w, &b, MP_ACT_MOVE, 0, &log);
            if(!ev_count(&log, MP_EV_VANISH)) continue;
            CHECK(b.gone[0] && b.land[0] == 1, "effects: out of reach after disappearing");
            mp_turn(w, &b, MP_ACT_MOVE, -1, &log);
            landed = ev_count(&log, MP_EV_DAMAGE) >= 1 && !b.gone[0];
        }
        CHECK(landed, "effects: a move that disappears lands the next turn");
    }
    printf("  effects: the rules hold on the shipped game (asleep at most %d turns)\n", longest);
    free(w);
    mg_close(&EG);
}

int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "dump")) return dump(argv[2]);
    if(getenv("MONSTER_SAVE")) SAVE = getenv("MONSTER_SAVE");
    const char *path = argc > 1 ? argv[1] : GAME;
    if(mg_open(&G, path)){ printf("FAIL: %s: %s\n", path, G.err); return 1; }
    uint8_t *scratch = malloc(MG_HEAD_MAX + MG_SPRITE_MAX);
    CHECK(mg_verify(&G, scratch, MG_HEAD_MAX + MG_SPRITE_MAX) == 0, "verify: %s", G.err);
    free(scratch);
    printf("monster: %u creatures, %u moves, %u areas\n", (unsigned)G.n_mons, (unsigned)G.n_moves, (unsigned)G.n_areas);
    formulas();
    walking();
    people();
    items();
    encounters();
    outcomes();
    MpWorld *w = battles();
    saves(w);
    free(w);
    effects();
    autoplay();
    mirror();
    free(out);
    mg_close(&G);
    printf("monster: %s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
