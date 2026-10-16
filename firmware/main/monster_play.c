/* monster_play.c -- the monster game's rules. See monster_play.h, and
 * docs/GAME_FORMAT.md section 2 for where each formula comes from.
 *
 * Integer arithmetic throughout. The bounds monster.c checks on META (and on
 * every record) keep each product below inside 64 bits; the comments say how. */
#include "monster_play.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "safefile.h"

static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };

static int werr(MpWorld *w, int r, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->err, sizeof w->err, fmt, ap);
    va_end(ap);
    return r;
}

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v){ put16(p, v); put16(p + 2, v >> 16); }

static void say(MpLog *log, uint8_t kind, uint8_t side, uint32_t a, uint32_t b, uint32_t n){
    if(!log) return;
    if(log->n >= MP_EVENTS){ log->overflow = 1; return; }
    MpEvent *e = &log->ev[log->n++];
    e->kind = kind; e->side = side; e->a = (uint16_t)a; e->b = (uint16_t)b; e->n = n;
}

/* ---- the generator ------------------------------------------------------- */

uint32_t mp_rand(uint32_t *s){
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

uint32_t mp_below(uint32_t *s, uint32_t n){
    return n ? (uint32_t)(((uint64_t)mp_rand(s) * n) >> 32) : 0;
}

/* ---- creatures ----------------------------------------------------------- */

uint32_t mp_exp_for(MonGame *g, uint16_t mon, uint32_t level){
    MgMon m;
    if(mg_mon(g, mon, &m)) return 0xFFFFFFFFu;
    /* level <= 255 and the exponent <= 4: 255^4 x 1000 fits easily */
    uint64_t v = 1;
    for(uint32_t i = 0; i < g->rules[4]; i++) v *= level;
    v = v * g->growth[1 + m.group] / 100;
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

int mp_stats(MonGame *g, MpCreature *c){
    MgMon m;
    int r = mg_mon(g, c->mon, &m);
    if(r) return r;
    for(int k = 0; k < MG_NSTATS; k++){
        uint32_t v = (uint32_t)g->shapes[m.shape][k] * (c->level + g->rules[1]) + c->iv[k];
        c->stat[k] = (uint16_t)(v > 65535 ? 65535 : v);
    }
    if(!c->stat[MG_HP]) c->stat[MG_HP] = 1;
    return 0;
}

static int knows(const MpCreature *c, uint16_t move){
    for(int i = 0; i < MP_MOVES; i++) if(c->moves[i] == move) return 1;
    return 0;
}

int mp_make(MonGame *g, uint32_t *rng, uint16_t mon, uint8_t level, MpCreature *c){
    MgMon m;
    int r;
    memset(c, 0, sizeof *c);
    if(!level || level > g->rules[5]) return MG_ERANGE;
    if((r = mg_mon(g, mon, &m))) return r;
    c->mon = mon;
    c->level = level;
    for(int k = 0; k < MG_NSTATS; k++) c->iv[k] = (uint8_t)mp_below(rng, g->growth[0] + 1);
    c->exp = mp_exp_for(g, mon, level);
    for(int i = 0; i < MP_MOVES; i++) c->moves[i] = MP_NO_MOVE;
    /* Tuxemon's set_moves: the moves learnt by this level, the last ones kept */
    uint32_t eligible = 0, max = g->rules[6];
    uint8_t lv;
    uint16_t mv;
    for(uint32_t j = 0; j < m.n_learn; j++){
        if((r = mg_learn(g, &m, j, &lv, &mv))) return r;
        if(lv <= level) eligible = j + 1;
    }
    uint32_t from = eligible > max ? eligible - max : 0, slot = 0;
    for(uint32_t j = from; j < eligible; j++){
        if((r = mg_learn(g, &m, j, &lv, &mv))) return r;
        if(!knows(c, mv) && slot < MP_MOVES) c->moves[slot++] = mv;
    }
    if((r = mp_stats(g, c))) return r;
    c->hp = c->stat[MG_HP];
    return 0;
}

int mp_learn(MonGame *g, MpCreature *c, uint16_t move, int slot){
    if(slot < 0) return 0;
    if(slot >= MP_MOVES || slot >= (int)g->rules[6] || move >= g->n_moves || knows(c, move)) return MG_ERANGE;
    c->moves[slot] = move;
    return 0;
}

/* The type multiplier as a fraction num/den, clamped to the rules' range.
 * Unclamped, num/den <= max/100, so num <= 100 x den <= 25,600 (den <= 4^4);
 * clamped, num <= 10,000 over den 100. */
static void mult_frac(MonGame *g, const MgMove *m, const MpCreature *def, uint64_t *num, uint64_t *den){
    MgMon dm;
    uint64_t n = 1, d = 1;
    if(!mg_mon(g, def->mon, &dm)){
        for(int i = 0; i < 2; i++){
            if(m->type[i] == MG_NONE8 || m->type[i] >= g->n_types) continue;
            for(int j = 0; j < 2; j++){
                if(dm.type[j] == MG_NONE8 || dm.type[j] >= g->n_types) continue;
                n *= g->types[m->type[i]][dm.type[j]];
                d *= 4;
            }
        }
    }
    if(n * 100 < (uint64_t)g->rules[2] * d){ n = g->rules[2]; d = 100; }
    else if(n * 100 > (uint64_t)g->rules[3] * d){ n = g->rules[3]; d = 100; }
    *num = n; *den = d;
}

uint32_t mp_multiplier(MonGame *g, const MgMove *m, const MpCreature *def, MgMon *def_mon){
    uint64_t n, d;
    (void)def_mon;
    mult_frac(g, m, def, &n, &d);
    return (uint32_t)(n * 100 / d);
}

uint32_t mp_damage(MonGame *g, const MpCreature *att, const MgMove *m, const MpCreature *def, uint32_t *mult){
    uint64_t n, d, strength, resist;
    uint64_t lv = g->rules[0] + att->level;
    mult_frac(g, m, def, &n, &d);
    if(mult) *mult = (uint32_t)(n * 100 / d);
    switch(m->range){
    case MG_MELEE:    strength = att->stat[MG_MELEE_ST] * lv;  resist = def->stat[MG_ARMOUR]; break;
    case MG_TOUCH:    strength = att->stat[MG_MELEE_ST] * lv;  resist = def->stat[MG_DODGE];  break;
    case MG_RANGED:   strength = att->stat[MG_RANGED_ST] * lv; resist = def->stat[MG_DODGE];  break;
    case MG_REACH:    strength = att->stat[MG_RANGED_ST] * lv; resist = def->stat[MG_ARMOUR]; break;
    case MG_RELIABLE: strength = lv; resist = 1; break;
    default:          return 0;
    }
    if(!resist) resist = 1;
    /* strength <= 65,535 x 355, power <= 65,535, n <= 25,600: under 4e16 */
    uint64_t v = strength * m->power * n / (100 * d * resist);
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

uint32_t mp_speed_score(const MpCreature *c, const MgMove *m){
    /* floor(max(1, speed x (1 + move_speed x 0.25)) + dodge x 0.01), in hundredths */
    int32_t ms = (int32_t)m->speed - 3;
    uint32_t v = c->stat[MG_SPEED] * (uint32_t)(4 + ms) * 25;
    if(v < 100) v = 100;
    v += c->stat[MG_DODGE];
    return v / 100;
}

static uint64_t isqrt64(uint64_t x){
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while(bit > x) bit >>= 2;
    while(bit){
        if(x >= r + bit){ x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

uint32_t mp_shake_check(MonGame *g, const MpCreature *c, const MgMon *m, uint32_t u){
    const uint32_t *C = g->capture;
    uint64_t hp = c->stat[MG_HP], cur = c->hp;
    /* catch_check = (3 HP - 2 cur) x rate / (3 HP); shake_check =
     * constant / (denominator x (100 / catch_check)^(1/4)) x u
     *             = constant x (catch_check / 100)^(1/4) / denominator x u.
     * X is (catch_check / 100) x 10^16, so its fourth root is scaled by 10^4. */
    if(C[4] * hp <= C[5] * cur || !m->catch_rate) return 0;
    uint64_t num = (C[4] * hp - C[5] * cur) * m->catch_rate;      /* <= 100 x 65,535 x 100 */
    uint64_t den = C[6] * hp * 100;
    uint64_t a = num * 100000000u, q = a / den, rem = a % den;
    uint64_t x = q > 100000000000ull ? 0xFFFFFFFFFFFFFFFFull : q * 100000000u + rem * 100000000u / den;
    uint64_t root = isqrt64(isqrt64(x));                           /* <= 65,535 */
    uint64_t v = (uint64_t)C[1] * root * u / ((uint64_t)C[2] * 10000 * 100);
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

/* ---- the world ----------------------------------------------------------- */

static int enter_area(MpWorld *w, uint16_t i, uint8_t x, uint8_t y, uint8_t facing){
    int r = mg_area_open(w->g, i, &w->area, w->head, MG_HEAD_MAX);
    if(r) return werr(w, r, "%s", w->g->err);
    for(int k = 0; k < MP_CHUNKS; k++) w->chunk_ix[k] = -1;
    w->chunk_next = 0;
    if(x >= w->area.w || y >= w->area.h) return werr(w, MG_ERANGE, "a place outside the area");
    w->x = x; w->y = y; w->facing = facing & 3;
    return 0;
}

int mp_cell(MpWorld *w, int x, int y, uint16_t *tile, uint8_t *move){
    *tile = 0; *move = 0x0F;
    if(x < 0 || y < 0 || x >= w->area.w || y >= w->area.h) return 0;
    int32_t ci = (y / MG_CHUNK) * w->area.ccols + x / MG_CHUNK;
    int k;
    for(k = 0; k < MP_CHUNKS; k++) if(w->chunk_ix[k] == ci) break;
    if(k == MP_CHUNKS){
        k = w->chunk_next;
        w->chunk_next = (uint8_t)((w->chunk_next + 1) % MP_CHUNKS);
        w->chunk_ix[k] = -1;
        int r = mg_chunk(w->g, &w->area, (uint32_t)ci, w->chunk[k]);
        if(r) return werr(w, r, "%s", w->g->err);
        w->chunk_ix[k] = ci;
    }
    uint32_t j = (uint32_t)(y % MG_CHUNK) * MG_CHUNK + (uint32_t)(x % MG_CHUNK);
    *tile = le16(w->chunk[k] + 2 * j);
    *move = w->chunk[k][2 * MG_CHUNK * MG_CHUNK + j];
    return 0;
}

/* can one step from (x, y) towards dir? (A's leave bit, B's enter bit for the side facing A) */
static int can_step(MpWorld *w, int x, int y, int dir, int *ok){
    uint16_t t;
    uint8_t a, b;
    int r;
    *ok = 0;
    int nx = x + DX[dir], ny = y + DY[dir];
    if(nx < 0 || ny < 0 || nx >= w->area.w || ny >= w->area.h) return 0;
    if((r = mp_cell(w, x, y, &t, &a)) || (r = mp_cell(w, nx, ny, &t, &b))) return r;
    *ok = !(a & (16 << dir)) && !(b & (1 << ((dir + 2) & 3)));
    return 0;
}

static int in_rect(int x, int y, uint8_t rx, uint8_t ry, uint8_t rw, uint8_t rh){
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int alive(const MpWorld *w){
    for(int i = 0; i < w->n_party; i++) if(w->party[i].hp) return 1;
    return 0;
}

static int encounter(MpWorld *w, uint16_t table, MpBattle *b, MpLog *log){
    MonGame *g = w->g;
    uint16_t n;
    uint32_t total = 0;
    MgEncRow row;
    int r;
    if((r = mg_enc_rows(g, table, &n))) return werr(w, r, "%s", g->err);
    for(uint32_t j = 0; j < n; j++){
        if((r = mg_enc_row(g, table, j, &row))) return werr(w, r, "%s", g->err);
        total += row.rate;
    }
    uint32_t pick = mp_below(&w->rng, total);
    for(uint32_t j = 0; j < n; j++){
        if((r = mg_enc_row(g, table, j, &row))) return werr(w, r, "%s", g->err);
        if(pick < row.rate) break;
        pick -= row.rate;
    }
    uint8_t level = (uint8_t)(row.lo + mp_below(&w->rng, (uint32_t)row.hi - row.lo + 1));
    MpCreature wild;
    if((r = mp_make(g, &w->rng, row.mon, level, &wild))) return werr(w, r, "%s", g->err);
    say(log, MP_EV_ENCOUNTER, 1, row.mon, level, 0);
    return mp_battle_start(w, b, &wild);
}

int mp_step(MpWorld *w, int dir, MpBattle *b, MpLog *log){
    int ok, r;
    if(log){ log->n = 0; log->overflow = 0; }
    if(dir < 0 || dir > 3) return MG_ERANGE;
    if(b) b->over = 1;      /* no battle unless one starts here */
    if(dir != w->facing){
        w->facing = (uint8_t)dir;
        say(log, MP_EV_TURN, 0, (uint32_t)dir, 0, 0);
        return 0;
    }
    if((r = can_step(w, w->x, w->y, dir, &ok))) return r;
    if(!ok){ say(log, MP_EV_BUMP, 0, w->x, w->y, 0); return 0; }
    w->x = (uint8_t)(w->x + DX[dir]);
    w->y = (uint8_t)(w->y + DY[dir]);
    uint16_t t;
    uint8_t mv;
    if((r = mp_cell(w, w->x, w->y, &t, &mv))) return r;
    if(w->area.tflags[t] & 1){
        /* a ledge: jump on over it, if there's somewhere to land */
        if((r = can_step(w, w->x, w->y, dir, &ok))) return r;
        if(ok){
            w->x = (uint8_t)(w->x + DX[dir]);
            w->y = (uint8_t)(w->y + DY[dir]);
            say(log, MP_EV_HOP, 0, w->x, w->y, 0);
        }
        else say(log, MP_EV_STEP, 0, w->x, w->y, 0);
    }
    else say(log, MP_EV_STEP, 0, w->x, w->y, 0);
    for(uint32_t k = 0; k < w->area.n_warps; k++){
        MgWarp wp;
        mg_warp(&w->area, k, &wp);
        if(!in_rect(w->x, w->y, wp.x, wp.y, wp.w, wp.h)) continue;
        if(wp.facing != MG_NONE8 && wp.facing != w->facing) continue;
        if((r = enter_area(w, wp.to, wp.tx, wp.ty, w->facing))) return r;
        say(log, MP_EV_WARP, 0, w->x, w->y, wp.to);
        return 0;
    }
    for(uint32_t k = 0; k < w->area.n_zones; k++){
        MgZone z;
        mg_zone(&w->area, k, &z);
        if(!in_rect(w->x, w->y, z.x, z.y, z.w, z.h)) continue;
        if(alive(w) && b && mp_below(&w->rng, 10000) < z.chance) return encounter(w, z.table, b, log);
        break;
    }
    return 0;
}

int mp_interact(MpWorld *w, MpLog *log){
    if(log){ log->n = 0; log->overflow = 0; }
    int fx = w->x + DX[w->facing], fy = w->y + DY[w->facing];
    for(uint32_t k = 0; k < w->area.n_signs; k++){
        MgSign s;
        mg_sign(&w->area, k, &s);
        if(!in_rect(fx, fy, s.x, s.y, s.w, s.h)) continue;
        if(s.facing != MG_NONE8 && s.facing != w->facing) continue;
        say(log, MP_EV_SIGN, 0, 0, s.text.len, s.text.off);
        return 1;
    }
    return 0;
}

void mp_heal_party(MpWorld *w){
    for(int i = 0; i < w->n_party; i++){
        w->party[i].hp = w->party[i].stat[MG_HP];
        w->party[i].status = 0;
    }
}

int mp_new(MpWorld *w, MonGame *g, uint8_t *head, uint32_t seed, uint16_t starter){
    memset(w, 0, sizeof *w);
    w->g = g;
    w->head = head;
    w->rng = seed ? seed : 0x9E3779B9u;
    int r = enter_area(w, g->start_area, g->start_x, g->start_y, g->start_facing);
    if(r) return r;
    uint8_t lv = (uint8_t)(g->rules[5] < 5 ? g->rules[5] : 5);
    if((r = mp_make(g, &w->rng, starter, lv, &w->party[0]))) return werr(w, r, "no such creature");
    w->n_party = 1;
    return 0;
}

/* ---- battles ------------------------------------------------------------- */

int mp_battle_start(MpWorld *w, MpBattle *b, const MpCreature *wild){
    memset(b, 0, sizeof *b);
    b->wild = *wild;
    b->over = 1;
    for(int i = 0; i < w->n_party; i++){
        if(!w->party[i].hp) continue;
        b->active = (uint8_t)i;
        b->fought = (uint8_t)(1u << i);
        b->over = 0;
        return 0;
    }
    return MG_ERANGE;
}

int mp_wild_choice(MpWorld *w, MpBattle *b){
    int usable[MP_MOVES], n = 0;
    for(int s = 0; s < MP_MOVES; s++)
        if(b->wild.moves[s] != MP_NO_MOVE && !b->recharge[1][s]) usable[n++] = s;
    return n ? usable[mp_below(&w->rng, (uint32_t)n)] : -1;
}

static int has_usable(const MpCreature *c, const uint8_t *rech){
    for(int s = 0; s < MP_MOVES; s++) if(c->moves[s] != MP_NO_MOVE && !rech[s]) return 1;
    return 0;
}

static int immune(MonGame *g, const MpCreature *c, uint8_t st){
    MgMon m;
    if(mg_mon(g, c->mon, &m)) return 1;
    uint16_t mask = g->statuses[st].immune;
    for(int j = 0; j < 2; j++)
        if(m.type[j] != MG_NONE8 && m.type[j] < 16 && (mask >> m.type[j] & 1)) return 1;
    return 0;
}

/* side uses its move in slot s */
static int act(MpWorld *w, MpBattle *b, int side, int s, MpLog *log){
    MonGame *g = w->g;
    MpCreature *me = side ? &b->wild : &w->party[b->active];
    MpCreature *foe = side ? &w->party[b->active] : &b->wild;
    MgMove m;
    if(!me->hp) return 0;
    if(s < 0){ say(log, MP_EV_RECHARGE, (uint8_t)side, 0, 0, 0); return 0; }
    int r = mg_move(g, me->moves[s], &m);
    if(r) return werr(w, r, "%s", g->err);
    say(log, MP_EV_USE, (uint8_t)side, me->moves[s], 0, 0);
    b->recharge[side][s] = (uint8_t)(m.recharge + 1);     /* this turn's end takes one off */
    if(mp_below(&w->rng, 10000) >= m.accuracy * 100u){ say(log, MP_EV_MISS, (uint8_t)side, 0, 0, 0); return 0; }
    for(int e = 0; e < 3; e++){
        if(!foe->hp && m.eff_target[e] == MG_T_ENEMY) break;
        switch(m.eff_kind[e]){
        case MG_E_DAMAGE: {
            if(m.range == MG_SPECIAL) break;
            uint32_t mult, d = mp_damage(g, me, &m, foe, &mult);
            if(d > foe->hp) d = foe->hp;
            foe->hp = (uint16_t)(foe->hp - d);
            say(log, MP_EV_DAMAGE, (uint8_t)!side, mult, 0, d);
            if(!foe->hp) say(log, MP_EV_FAINT, (uint8_t)!side, 0, 0, 0);
            break;
        }
        case MG_E_HEALING: {
            MpCreature *t = m.eff_target[e] == MG_T_OWN ? me : foe;
            int ts = m.eff_target[e] == MG_T_OWN ? side : !side;
            uint32_t h = 7 + (uint32_t)me->level * m.heal / 100;
            if(h > (uint32_t)t->stat[MG_HP] - t->hp) h = (uint32_t)t->stat[MG_HP] - t->hp;
            t->hp = (uint16_t)(t->hp + h);
            say(log, MP_EV_HEAL, (uint8_t)ts, 0, 0, h);
            break;
        }
        case MG_E_GIVE: {
            MpCreature *t = m.eff_target[e] == MG_T_OWN ? me : foe;
            int ts = m.eff_target[e] == MG_T_OWN ? side : !side;
            uint8_t st = m.eff_status[e];
            if(mp_below(&w->rng, 10000) >= m.potency * 100u || !t->hp) break;
            if(immune(g, t, st)){ say(log, MP_EV_IMMUNE, (uint8_t)ts, st, 0, 0); break; }
            t->status = (uint8_t)(st + 1);
            say(log, MP_EV_STATUS, (uint8_t)ts, st, 0, 0);
            break;
        }
        default:
            break;
        }
    }
    return 0;
}

/* a status's effect at the end of a turn */
static void status_tick(MpWorld *w, MpCreature *c, int side, MpLog *log){
    if(!c->hp || !c->status || c->status > w->g->n_statuses) return;
    const MgStatus *st = &w->g->statuses[c->status - 1];
    uint32_t amount = c->stat[MG_HP] / st->divisor;
    if(st->kind == MG_S_RECOVER){
        if(amount > (uint32_t)c->stat[MG_HP] - c->hp) amount = (uint32_t)c->stat[MG_HP] - c->hp;
        c->hp = (uint16_t)(c->hp + amount);
        say(log, MP_EV_STATUS_HEAL, (uint8_t)side, c->status - 1u, 0, amount);
        if(c->hp == c->stat[MG_HP]){
            say(log, MP_EV_STATUS_END, (uint8_t)side, c->status - 1u, 0, 0);
            c->status = 0;
        }
        return;
    }
    if(amount > c->hp) amount = c->hp;
    c->hp = (uint16_t)(c->hp - amount);
    say(log, MP_EV_STATUS_HURT, (uint8_t)side, c->status - 1u, 0, amount);
    if(!c->hp) say(log, MP_EV_FAINT, (uint8_t)side, 0, 0, 0);
}

/* level-ups, new moves and evolution for party member i */
static int grow(MpWorld *w, int i, MpLog *log){
    MonGame *g = w->g;
    MpCreature *c = &w->party[i];
    int r;
    while(c->level < g->rules[5] && c->exp >= mp_exp_for(g, c->mon, c->level + 1u)){
        uint16_t old = c->stat[MG_HP];
        c->level++;
        if((r = mp_stats(g, c))) return r;
        c->hp = (uint16_t)(c->hp + (c->stat[MG_HP] > old ? c->stat[MG_HP] - old : 0));
        if(c->hp > c->stat[MG_HP]) c->hp = c->stat[MG_HP];
        say(log, MP_EV_LEVEL, 0, (uint32_t)i, c->level, 0);
        MgMon m;
        if((r = mg_mon(g, c->mon, &m))) return r;
        for(uint32_t j = 0; j < m.n_learn; j++){
            uint8_t lv;
            uint16_t mv;
            if((r = mg_learn(g, &m, j, &lv, &mv))) return r;
            if(lv != c->level || knows(c, mv)) continue;
            int slot = -1;
            for(int s = 0; s < MP_MOVES && s < (int)g->rules[6]; s++) if(c->moves[s] == MP_NO_MOVE){ slot = s; break; }
            if(slot >= 0){ c->moves[slot] = mv; say(log, MP_EV_LEARN, 0, (uint32_t)i, mv, 0); }
            else say(log, MP_EV_LEARN_FULL, 0, (uint32_t)i, mv, 0);
        }
        if(m.evolve_to != MG_NONE16 && c->level >= m.evolve_at){
            old = c->stat[MG_HP];
            c->mon = m.evolve_to;
            if((r = mp_stats(g, c))) return r;
            c->hp = (uint16_t)(c->hp + (c->stat[MG_HP] > old ? c->stat[MG_HP] - old : 0));
            if(c->hp > c->stat[MG_HP]) c->hp = c->stat[MG_HP];
            say(log, MP_EV_EVOLVE, 0, (uint32_t)i, c->mon, 0);
        }
    }
    return 0;
}

/* the battle is over: statuses that don't outlast it end */
static void battle_end(MpWorld *w, MpBattle *b){
    for(int i = 0; i < w->n_party; i++){
        MpCreature *c = &w->party[i];
        if(c->status && c->status <= w->g->n_statuses && !(w->g->statuses[c->status - 1].flags & 2)) c->status = 0;
    }
    b->over = 1;
}

/* who won, who must swap, and the experience */
static int resolve(MpWorld *w, MpBattle *b, MpLog *log){
    MonGame *g = w->g;
    int r;
    if(!alive(w)){
        mp_heal_party(w);
        if((r = enter_area(w, g->start_area, g->start_x, g->start_y, g->start_facing))) return r;
        say(log, MP_EV_LOSE, 0, 0, 0, 0);
        battle_end(w, b);
        return 0;
    }
    if(!b->wild.hp){
        uint32_t gain = mp_exp_for(g, b->wild.mon, b->wild.level) / b->wild.level, n = 0;
        for(int i = 0; i < w->n_party; i++) if((b->fought >> i & 1) && w->party[i].hp) n++;
        if(n){
            uint32_t each = gain / n;
            for(int i = 0; i < w->n_party; i++){
                MpCreature *c = &w->party[i];
                if(!(b->fought >> i & 1) || !c->hp) continue;
                c->exp = c->exp > 0xFFFFFFFFu - each ? 0xFFFFFFFFu : c->exp + each;
                say(log, MP_EV_EXP, 0, (uint32_t)i, 0, each);
                if((r = grow(w, i, log))) return r;
            }
        }
        say(log, MP_EV_WIN, 0, 0, 0, 0);
        battle_end(w, b);
        return 0;
    }
    if(!w->party[b->active].hp){
        b->need_swap = 1;
        say(log, MP_EV_NEED_SWAP, 0, 0, 0, 0);
    }
    return 0;
}

static int catch_it(MpWorld *w, MpBattle *b, MpLog *log, int *caught){
    MonGame *g = w->g;
    MgMon m;
    int r;
    *caught = 0;
    if((r = mg_mon(g, b->wild.mon, &m))) return werr(w, r, "%s", g->err);
    say(log, MP_EV_THROW, 0, 0, 0, 0);
    if(!m.catch_rate){ say(log, MP_EV_BREAK_FREE, 1, 0, 0, 0); return 0; }
    uint32_t u = m.resist_lo + mp_below(&w->rng, (uint32_t)m.resist_hi - m.resist_lo + 1);
    uint32_t sc = mp_shake_check(g, &b->wild, &m, u);
    for(uint32_t i = 1; i <= g->capture[0]; i++){
        if(mp_below(&w->rng, g->capture[3] + 1) > sc){ say(log, MP_EV_BREAK_FREE, 1, 0, 0, 0); return 0; }
        say(log, MP_EV_SHAKE, 1, i, 0, 0);
    }
    uint32_t room = g->rules[7] < MP_PARTY ? g->rules[7] : MP_PARTY;
    if(w->n_party < room){
        w->party[w->n_party] = b->wild;
        say(log, MP_EV_CAUGHT, 1, w->n_party, 0, 0);
        w->n_party++;
    }
    else{
        w->box[w->n_box] = b->wild;
        say(log, MP_EV_CAUGHT, 1, w->n_box, 1, 0);
        w->n_box++;
    }
    *caught = 1;
    battle_end(w, b);
    return 0;
}

int mp_turn(MpWorld *w, MpBattle *b, int act_kind, int arg, MpLog *log){
    MonGame *g = w->g;
    int r;
    if(log){ log->n = 0; log->overflow = 0; }
    if(b->over) return MG_ERANGE;
    MpCreature *me = &w->party[b->active];
    if(b->need_swap && act_kind != MP_ACT_SWAP) return MG_ERANGE;
    switch(act_kind){
    case MP_ACT_MOVE:
        if(arg < 0){ if(has_usable(me, b->recharge[0])) return MG_ERANGE; }
        else if(arg >= MP_MOVES || me->moves[arg] == MP_NO_MOVE || b->recharge[0][arg]) return MG_ERANGE;
        break;
    case MP_ACT_SWAP:
        if(arg < 0 || arg >= w->n_party || arg == b->active || !w->party[arg].hp) return MG_ERANGE;
        break;
    case MP_ACT_CATCH: {
        uint32_t room = g->rules[7] < MP_PARTY ? g->rules[7] : MP_PARTY;
        if(w->n_party >= room && w->n_box >= MP_BOX) return MG_ERANGE;
        break;
    }
    case MP_ACT_RUN:
        break;
    default:
        return MG_ERANGE;
    }

    if(act_kind == MP_ACT_SWAP){
        b->active = (uint8_t)arg;
        b->fought |= (uint8_t)(1u << arg);
        say(log, MP_EV_SWAP, 0, (uint32_t)arg, 0, 0);
        if(b->need_swap){ b->need_swap = 0; return 0; }      /* a fainted creature replaced: no turn */
    }
    int foe_slot = mp_wild_choice(w, b);
    if(act_kind == MP_ACT_RUN){
        int32_t chance = 40 + 15 * ((int32_t)b->run_tries + me->level - b->wild.level);
        if((int32_t)mp_below(&w->rng, 100) < chance){
            say(log, MP_EV_RUN, 0, 0, 0, 0);
            battle_end(w, b);
            return 0;
        }
        say(log, MP_EV_RUN_FAIL, 0, 0, 0, 0);
        b->run_tries++;
    }
    else if(act_kind == MP_ACT_CATCH){
        int caught;
        if((r = catch_it(w, b, log, &caught))) return r;
        if(caught) return 0;
    }

    if(act_kind == MP_ACT_MOVE){
        /* the faster goes first; a tie is a coin toss (Tuxemon's random sub_priority) */
        MgMove pm, fm;
        int player_first = 1;
        if(arg >= 0 && foe_slot >= 0){
            if((r = mg_move(g, me->moves[arg], &pm)) || (r = mg_move(g, b->wild.moves[foe_slot], &fm)))
                return werr(w, r, "%s", g->err);
            uint32_t sp = mp_speed_score(me, &pm), sf = mp_speed_score(&b->wild, &fm);
            player_first = sp > sf || (sp == sf && !mp_below(&w->rng, 2));
        }
        if(player_first){
            if((r = act(w, b, 0, arg, log)) || (r = act(w, b, 1, foe_slot, log))) return r;
        }
        else if((r = act(w, b, 1, foe_slot, log)) || (r = act(w, b, 0, arg, log))) return r;
    }
    else if((r = act(w, b, 1, foe_slot, log))) return r;

    status_tick(w, &w->party[b->active], 0, log);
    status_tick(w, &b->wild, 1, log);
    for(int s = 0; s < 2; s++)
        for(int k = 0; k < MP_MOVES; k++) if(b->recharge[s][k]) b->recharge[s][k]--;
    return resolve(w, b, log);
}

/* ---- the save -------------------------------------------------------------
 * A 32-byte header: "CYDS", u16 version 1, u16 box count, u32 the game file's
 * fingerprint, u32 the generator, u32 area id, u8 x, y, facing, party count,
 * u32 CRC-32 of the records, u32 CRC-32 of bytes 0-27. Then 40 bytes per
 * creature, the party first: u32 creature id, u32 experience, u16 HP, u8
 * level, u8 0, u32 status id (0 none), u8 iv x 6, u16 0, u32 move id x 4
 * (0 none). Ids, not positions, so a rebuilt game file (game_ids.tsv keeps
 * the numbers) still loads the save. Replaced whole (safefile.h). */

static void put_rec(MonGame *g, const MpCreature *c, uint8_t *p){
    MgMon m;
    MgMove mv;
    memset(p, 0, MP_SAVE_REC);
    put32(p, mg_mon(g, c->mon, &m) ? 0 : m.id);
    put32(p + 4, c->exp);
    put16(p + 8, c->hp);
    p[10] = c->level;
    put32(p + 12, c->status && c->status <= g->n_statuses ? g->statuses[c->status - 1].id : 0);
    memcpy(p + 16, c->iv, MG_NSTATS);
    for(int i = 0; i < MP_MOVES; i++)
        put32(p + 24 + 4 * i, c->moves[i] != MP_NO_MOVE && !mg_move(g, c->moves[i], &mv) ? mv.id : 0);
}

int mp_save(MpWorld *w, const char *path){
    uint8_t h[MP_SAVE_HEADER], rec[MP_SAVE_REC];
    uint32_t crc = 0, n = (uint32_t)w->n_party + w->n_box;
    SafeFile sf;
    for(uint32_t i = 0; i < n; i++){
        put_rec(w->g, i < w->n_party ? &w->party[i] : &w->box[i - w->n_party], rec);
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
    }
    memset(h, 0, sizeof h);
    memcpy(h, "CYDS", 4);
    put16(h + 4, 1);
    put16(h + 6, w->n_box);
    put32(h + 8, w->g->fingerprint);
    put32(h + 12, w->rng);
    put32(h + 16, w->area.dir.id);
    h[20] = w->x; h[21] = w->y; h[22] = w->facing; h[23] = w->n_party;
    put32(h + 24, crc);
    put32(h + 28, mg_crc32(0, h, 28));
    FILE *f = sf_open(&sf, path, "wb");
    if(!f) return werr(w, MG_EIO, "can't write the save");
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h;
    for(uint32_t i = 0; ok && i < n; i++){
        put_rec(w->g, i < w->n_party ? &w->party[i] : &w->box[i - w->n_party], rec);
        ok = fwrite(rec, 1, MP_SAVE_REC, f) == MP_SAVE_REC;
    }
    if(sf_commit(&sf, ok)) return werr(w, MG_EIO, "can't write the save");
    return 0;
}

static int get_rec(MpWorld *w, const uint8_t *p, MpCreature *c){
    MonGame *g = w->g;
    int r;
    memset(c, 0, sizeof *c);
    if(p[11] || le16(p + 22)) return werr(w, MG_EBAD, "the save is damaged");
    if(mg_find_mon(g, le32(p), &c->mon)) return werr(w, MG_EBAD, "the save has a creature this game file doesn't");
    c->exp = le32(p + 4);
    c->hp = le16(p + 8);
    c->level = p[10];
    uint32_t sid = le32(p + 12);
    if(sid){
        for(uint8_t i = 0; i < g->n_statuses; i++) if(g->statuses[i].id == sid) c->status = (uint8_t)(i + 1);
        if(!c->status) return werr(w, MG_EBAD, "the save has a status this game file doesn't");
    }
    for(int k = 0; k < MG_NSTATS; k++){
        c->iv[k] = p[16 + k];
        if(c->iv[k] > g->growth[0]) return werr(w, MG_EBAD, "the save is damaged");
    }
    int any = 0;
    for(int i = 0; i < MP_MOVES; i++){
        uint32_t id = le32(p + 24 + 4 * i);
        c->moves[i] = MP_NO_MOVE;
        if(!id) continue;
        uint16_t pos;
        if(mg_find_move(g, id, &pos)) return werr(w, MG_EBAD, "the save has a move this game file doesn't");
        if(knows(c, pos) || i >= (int)g->rules[6]) return werr(w, MG_EBAD, "the save is damaged");
        c->moves[i] = pos;
        any = 1;
    }
    if(!any || !c->level || c->level > g->rules[5]) return werr(w, MG_EBAD, "the save is damaged");
    if(c->exp < mp_exp_for(g, c->mon, c->level) ||
       (c->level < g->rules[5] && c->exp >= mp_exp_for(g, c->mon, c->level + 1u)))
        return werr(w, MG_EBAD, "the save is damaged");
    if((r = mp_stats(g, c))) return werr(w, r, "%s", g->err);
    if(c->hp > c->stat[MG_HP]) return werr(w, MG_EBAD, "the save is damaged");
    return 0;
}

int mp_load(MpWorld *w, MonGame *g, uint8_t *head, const char *path){
    uint8_t h[MP_SAVE_HEADER], rec[MP_SAVE_REC];
    int r;
    memset(w, 0, sizeof *w);
    w->g = g;
    w->head = head;
    sf_recover(path);
    FILE *f = fopen(path, "rb");
    if(!f) return werr(w, MG_EIO, "no save");
    r = MG_EBAD;
    if(fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "CYDS", 4) || mg_crc32(0, h, 28) != le32(h + 28)){
        werr(w, MG_EBAD, "the save is damaged");
        goto out;
    }
    if(le16(h + 4) != 1){ r = werr(w, MG_EVERSION, "a save from a newer version"); goto out; }
    uint32_t nbox = le16(h + 6), nparty = h[23];
    if(!nparty || nparty > MP_PARTY || nparty > g->rules[7] || nbox > MP_BOX){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
    uint32_t crc = 0;
    for(uint32_t i = 0; i < nparty + nbox; i++){
        if(fread(rec, 1, MP_SAVE_REC, f) != MP_SAVE_REC){ werr(w, MG_EBAD, "the save is cut short"); goto out; }
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
        if((r = get_rec(w, rec, i < nparty ? &w->party[i] : &w->box[i - nparty]))) goto out;
        r = MG_EBAD;
    }
    if(fgetc(f) != EOF){ werr(w, MG_EBAD, "the save is too long"); goto out; }
    if(crc != le32(h + 24)){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
    uint16_t area;
    if(mg_find_area(g, le32(h + 16), &area)){ werr(w, MG_EBAD, "the save is in an area this game file doesn't have"); goto out; }
    if(h[22] > 3){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
    if((r = enter_area(w, area, h[20], h[21], h[22]))) goto out;
    w->rng = le32(h + 12) ? le32(h + 12) : 0x9E3779B9u;
    w->n_party = (uint8_t)nparty;
    w->n_box = (uint16_t)nbox;
    r = 0;
out:
    fclose(f);
    if(r){
        char e[sizeof w->err];
        memcpy(e, w->err, sizeof e);
        memset(w, 0, sizeof *w);
        w->g = g; w->head = head;
        memcpy(w->err, e, sizeof e);
    }
    return r;
}
