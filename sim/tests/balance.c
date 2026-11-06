/* balance.c -- the balance run (docs/EFFECTS_PLAN.md section 3).
 *
 * Every critter against every critter at levels 10, 25 and 40, each with the
 * moves it would know then, BALANCE_SEEDS seeds a pair (20 unless the
 * environment says), both sides choosing their moves as Auto-play does
 * (mb_pick). It prints one table, writes build/balance.tsv, and compares the
 * run with the baseline the last accepted step left (tests/data/balance.tsv):
 *
 *   nothing stalls        no battle unended in 400 turns
 *   length                the mean battle within 25 % of the baseline's
 *   no critter runs away  no critter's win rate moves more than 10 points
 *   no move takes over    no move chosen in more than 40 % of the turns of the
 *                         critters that know it, unless it was before
 *
 * BALANCE_RECORD=1 writes the run as the new baseline instead (a step that
 * passes, accepted). The numbers each step leaves go in BUILD_PROGRESS.md.
 *
 *   ./build/balance_test [game.bin]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monster.h"
#include "monster_play.h"
#include "monster_bot.h"

#define TURNS 400
#define MAXM  256
#define MAXMV 512

static const char *GAME = "../courses/critters/game.bin";
static const char *BASE = "../tests/data/balance.tsv";
static const char *OUT = "build/balance.tsv";
static const int LEVELS[] = { 10, 25, 40 };

static long wins[MAXM], fights[MAXM];
static long mv_used[MAXMV], mv_known[MAXMV], mv_won[MAXMV], mv_sides[MAXMV];
static char mon_name[MAXM][32], move_name[MAXMV][32];
static long turns_all, battles, unended, longest;

static MbBot BOT;

static void named(MonGame *g){
    for(uint32_t i = 0; i < g->n_mons && i < MAXM; i++){
        MgMon m;
        if(mg_mon(g, i, &m) || mg_text(g, m.name, mon_name[i], 32) < 0) snprintf(mon_name[i], 32, "?");
    }
    for(uint32_t i = 0; i < g->n_moves && i < MAXMV; i++){
        MgMove m;
        if(mg_move(g, i, &m) || mg_text(g, m.name, move_name[i], 32) < 0) snprintf(move_name[i], 32, "?");
    }
}

/* one battle: 1 if a wins, 0 if b, -1 unended; its turns */
static int fight(MpWorld *w, MonGame *g, uint16_t a, uint16_t b, int lv, uint32_t seed, int *turns){
    MpBattle bt;
    MpLog log;
    uint32_t r1 = seed * 2654435761u + 1, r2 = seed * 40503u + 7;
    MpCreature ca, cb;
    if(mp_make(g, &r1, a, (uint8_t)lv, &ca) || mp_make(g, &r2, b, (uint8_t)lv, &cb)) return -2;
    w->party[0] = ca;
    w->n_party = 1;
    w->rng = seed * 747796405u + 2891336453u;
    if(!w->rng) w->rng = 1;
    if(mp_battle_start(w, &bt, &cb)) return -2;
    uint8_t usedA[MP_MOVES] = { 0 }, usedB[MP_MOVES] = { 0 };
    int t = 0;
    for(; !bt.over && t < TURNS; t++){
        MpCreature *me = &w->party[0];
        int pa = mb_pick(&BOT, &bt, 0, me, &bt.wild, bt.recharge[0]);
        int pb = mb_pick(&BOT, &bt, 1, &bt.wild, me, bt.recharge[1]);
        for(int s = 0; s < MP_MOVES; s++){
            if(me->moves[s] != MP_NO_MOVE && me->moves[s] < MAXMV) mv_known[me->moves[s]]++;
            if(bt.wild.moves[s] != MP_NO_MOVE && bt.wild.moves[s] < MAXMV) mv_known[bt.wild.moves[s]]++;
        }
        if(pa >= 0 && me->moves[pa] < MAXMV){ mv_used[me->moves[pa]]++; usedA[pa] = 1; }
        if(pb >= 0 && bt.wild.moves[pb] < MAXMV){ mv_used[bt.wild.moves[pb]]++; usedB[pb] = 1; }
        bt.foe_set = pb >= 0;
        bt.foe_slot = (uint8_t)(pb < 0 ? 0 : pb);
        if(mp_turn(w, &bt, MP_ACT_MOVE, pa, &log)){ fprintf(stderr, "a turn refused: %s\n", w->err); return -2; }
        if(bt.need_swap) break;
    }
    *turns = t;
    int res = bt.over ? (bt.wild.hp == 0) : bt.need_swap ? 0 : -1;
    for(int s = 0; s < MP_MOVES; s++){
        if(usedA[s] && ca.moves[s] < MAXMV){ mv_sides[ca.moves[s]]++; mv_won[ca.moves[s]] += res == 1; }
        if(usedB[s] && cb.moves[s] < MAXMV){ mv_sides[cb.moves[s]]++; mv_won[cb.moves[s]] += res == 0; }
    }
    return res;
}

/* the baseline's line for kind and name, its first number, or -1 */
static double base_val(const char *buf, const char *kind, const char *name, int col){
    char key[96];
    snprintf(key, sizeof key, "%s\t%s\t", kind, name);
    const char *p = buf;
    while(p && *p){
        if(!strncmp(p, key, strlen(key))){
            p += strlen(key);
            for(int c = 0; c < col; c++){ p = strchr(p, '\t'); if(!p) return -1; p++; }
            return atof(p);
        }
        p = strchr(p, '\n');
        if(p) p++;
    }
    return -1;
}

int main(int argc, char **argv){
    MonGame g;
    const char *path = argc > 1 ? argv[1] : GAME;
    if(mg_open(&g, path)){ printf("FAIL: %s: %s\n", path, g.err); return 1; }
    int seeds = getenv("BALANCE_SEEDS") ? atoi(getenv("BALANCE_SEEDS")) : 20;
    named(&g);
    memset(&BOT, 0, sizeof BOT);
    BOT.g = &g;
    uint32_t cap = 0;
    for(uint32_t i = 0; i < g.n_areas; i++){
        MgAreaDir d;
        if(!mg_area_dir(&g, i, &d) && d.head_len > cap) cap = d.head_len;
    }
    uint8_t *head = malloc(cap);
    MpWorld *w = calloc(1, sizeof *w);
    w->head_cap = cap;
    if(mp_new(w, &g, head, 1, 0)){ printf("FAIL: a world: %s\n", w->err); return 1; }
    uint32_t n = g.n_mons < MAXM ? g.n_mons : MAXM;
    for(uint32_t a = 0; a < n; a++)
        for(uint32_t b = 0; b < n; b++)
            for(int li = 0; li < 3; li++)
                for(int k = 0; k < seeds; k++){
                    int t = 0;
                    uint32_t seed = ((a * 977u + b) * 3u + (uint32_t)li) * 101u + (uint32_t)k + 1;
                    int r = fight(w, &g, (uint16_t)a, (uint16_t)b, LEVELS[li], seed, &t);
                    if(r == -2) continue;
                    battles++;
                    turns_all += t;
                    if(t > longest) longest = t;
                    if(r < 0){ unended++; continue; }
                    fights[a]++; fights[b]++;
                    wins[r ? a : b]++;
                }
    double mean = battles ? (double)turns_all / battles : 0;
    FILE *f = fopen(OUT, "w");
    if(!f){ printf("FAIL: can't write %s\n", OUT); return 1; }
    fprintf(f, "battles\t%ld\t%ld\t%.2f\t%ld\n", battles, unended, mean, longest);
    for(uint32_t i = 0; i < n; i++)
        fprintf(f, "critter\t%s\t%.1f\n", mon_name[i], fights[i] ? 100.0 * wins[i] / fights[i] : 0);
    for(uint32_t i = 0; i < g.n_moves && i < MAXMV; i++)
        if(mv_known[i]) fprintf(f, "move\t%s\t%.1f\t%.1f\n", move_name[i], 100.0 * mv_used[i] / mv_known[i],
                                mv_sides[i] ? 100.0 * mv_won[i] / mv_sides[i] : 0);
    fclose(f);
    printf("balance: %ld battles (%d seeds a pair, levels 10/25/40), %ld unended, mean %.2f turns, longest %ld\n",
           battles, seeds, unended, mean, longest);

    int fails = 0;
    if(unended){ printf("FAIL: %ld battles never ended in %d turns\n", unended, TURNS); fails++; }
    if(getenv("BALANCE_RECORD")){
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cp %s %s", OUT, BASE);
        if(system(cmd)) { printf("FAIL: couldn't record %s\n", BASE); return 1; }
        printf("  recorded %s as the baseline\n", BASE);
        return fails ? 1 : 0;
    }
    FILE *bf = fopen(BASE, "rb");
    char *buf = NULL;
    if(bf){
        fseek(bf, 0, SEEK_END); long len = ftell(bf); fseek(bf, 0, SEEK_SET);
        buf = calloc(1, (size_t)len + 1);
        if(fread(buf, 1, (size_t)len, bf) != (size_t)len) len = 0;
        fclose(bf);
    }
    if(!buf){ printf("FAIL: no baseline %s (BALANCE_RECORD=1 makes one)\n", BASE); return 1; }
    double bmean = base_val(buf, "battles", "", 0);
    {   /* the battles line has no name column: read it directly */
        long bb, bu; double bm; long bl;
        if(sscanf(buf, "battles\t%ld\t%ld\t%lf\t%ld", &bb, &bu, &bm, &bl) == 4) bmean = bm;
    }
    if(bmean > 0 && (mean > bmean * 1.25 || mean < bmean * 0.75)){
        printf("FAIL: the mean battle is %.2f turns, the baseline's %.2f (more than 25 %% apart)\n", mean, bmean);
        fails++;
    }
    int moved = 0, top = 0;
    double worst = 0;
    for(uint32_t i = 0; i < n; i++){
        double now = fights[i] ? 100.0 * wins[i] / fights[i] : 0, was = base_val(buf, "critter", mon_name[i], 0);
        if(was < 0) continue;
        double dlt = now > was ? now - was : was - now;
        if(dlt > worst){ worst = dlt; top = (int)i; }
        if(dlt > 10){ printf("FAIL: %s wins %.1f %%, the baseline %.1f %%\n", mon_name[i], now, was); fails++; moved++; }
    }
    printf("  the critter that moved most: %s, %.1f points\n", mon_name[top], worst);
    double most = 0;
    int mi = -1;
    for(uint32_t i = 0; i < g.n_moves && i < MAXMV; i++){
        if(!mv_known[i]) continue;
        double use = 100.0 * mv_used[i] / mv_known[i], was = base_val(buf, "move", move_name[i], 0);
        if(use > most){ most = use; mi = (int)i; }
        if(use > 40 && was <= 40){ printf("FAIL: %s is chosen in %.1f %% of the turns of the critters that know it\n", move_name[i], use); fails++; }
    }
    if(mi >= 0) printf("  the most chosen move: %s, %.1f %% of its knowers' turns\n", move_name[mi], most);
    printf("balance: %s\n", fails ? "FAILED" : "OK");
    free(buf); free(w); free(head);
    mg_close(&g);
    return fails ? 1 : 0;
}
