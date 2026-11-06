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
        if(def->xtype && def->xtype <= g->n_types){ dm.type[0] = (uint8_t)(def->xtype - 1); dm.type[1] = MG_NONE8; }
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

const MgStatus *mp_status(const MonGame *g, const MpCreature *c){
    return c->status && c->status <= g->n_statuses ? &g->statuses[c->status - 1] : NULL;
}

/* Tuxemon's stage table (monster/stats.py NONLINEAR_TABLE) as num / den */
static const uint8_t STAGE_NUM[13] = { 2, 2, 2, 2, 2, 2, 1, 3, 4, 5, 6, 7, 8 };
static const uint8_t STAGE_DEN[13] = { 8, 7, 6, 5, 4, 3, 1, 2, 2, 2, 2, 2, 2 };
/* the SHAP order of the stats MG_X_* name */
static const uint8_t X_STAT[MG_NX] = { MG_ARMOUR, MG_DODGE, MG_MELEE_ST, MG_RANGED_ST, MG_SPEED };

void mp_view(MonGame *g, const MpBattle *b, int side, const MpCreature *c, MpCreature *out){
    const MgStatus *st = mp_status(g, c);
    if(out != c) *out = *c;
    for(int k = 0; k < MG_NX; k++){
        uint32_t v = out->stat[X_STAT[k]];
        if(st && st->mult[k] != 10) v = v * st->mult[k] / 10;
        int sg = b ? b->stage[side & 1][k] : 0;
        if(sg){
            if(sg < -6) sg = -6;
            if(sg > 6) sg = 6;
            v = v * STAGE_NUM[sg + 6] / STAGE_DEN[sg + 6];
        }
        out->stat[X_STAT[k]] = (uint16_t)(v > 65535 ? 65535 : v);
    }
}

void mp_view_move(MonGame *g, const MpCreature *c, MgMove *m){
    const MgStatus *st = mp_status(g, c);
    if(!st || st->kind != MG_S_WEAKEN || !(st->ranges >> m->range & 1)) return;
    m->power = (uint16_t)(m->power / st->divisor);
    m->potency = (uint8_t)(m->potency / st->divisor);
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
    int r = mg_area_open(w->g, i, &w->area, w->head, w->head_cap ? w->head_cap : MG_HEAD_MAX);
    if(r) return werr(w, r, "%s", w->g->err);
    for(int k = 0; k < MP_CHUNKS; k++) w->chunk_ix[k] = -1;
    w->chunk_next = 0;
    if(x >= w->area.w || y >= w->area.h) return werr(w, MG_ERANGE, "a place outside the area");
    w->x = x; w->y = y; w->facing = facing & 3;
    for(uint32_t k = 0; k < w->area.n_people; k++) w->pface[k] = w->area.people[48 * k + 2];
    return 0;
}

int mp_place(MpWorld *w, uint16_t area, uint8_t x, uint8_t y, uint8_t facing){
    return enter_area(w, area, x, y, facing);
}

/* ---- flags and people ---- */

int mp_flag(const MpWorld *w, uint16_t f){
    return f && f < MG_FLAGS && (w->flags[f / 8] >> (f % 8) & 1);
}

void mp_set_flag(MpWorld *w, uint16_t f){
    if(f && f < MG_FLAGS) w->flags[f / 8] |= (uint8_t)(1u << (f % 8));
}

int mp_present(const MpWorld *w, const MgPerson *p){
    return (!p->show_if || mp_flag(w, p->show_if)) && !(p->hide_if && mp_flag(w, p->hide_if));
}

/* the person standing at (x, y), or -1 */
uint16_t mp_table(const MpWorld *w, uint16_t table){
    const MonGame *g = w->g;
    for(uint32_t i = 0; i < g->n_steps; i++)
        if(g->steps[i].table == table && mp_flag(w, g->steps[i].flag)) table = g->steps[i].to;
    return table;
}

static int person_at(const MpWorld *w, int x, int y, MgPerson *p){
    for(uint32_t k = 0; k < w->area.n_people; k++){
        mg_person(&w->area, k, p);
        if(p->x == x && p->y == y && mp_present(w, p)) return (int)k;
    }
    return -1;
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
    mp_mark_mon(w, row.mon, 0);
    say(log, MP_EV_ENCOUNTER, 1, row.mon, level, 0);
    return mp_battle_start(w, b, &wild);
}

static int trainer_battle(MpWorld *w, uint32_t k, MpBattle *b, MpLog *log);

/* a trainer not yet beaten who has the player in sight: they look along
 * their facing, up to `sight` cells, and walls and people block the view */
static int spotted(MpWorld *w, int *err){
    *err = 0;
    for(uint32_t k = 0; k < w->area.n_people; k++){
        MgPerson p;
        mg_person(&w->area, k, &p);
        if(p.kind != MG_P_TRAINER || !p.sight || mp_flag(w, p.flag) || !mp_present(w, &p)) continue;
        int d = p.facing, x = p.x, y = p.y, ok;
        for(int n = 1; n <= p.sight; n++){
            if((*err = can_step(w, x, y, d, &ok))) return -1;
            if(!ok) break;
            x += DX[d]; y += DY[d];
            if(x == w->x && y == w->y){ w->pface[k] = (uint8_t)d; return (int)k; }
            MgPerson q;
            if(person_at(w, x, y, &q) >= 0) break;
        }
    }
    return -1;
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
    MgPerson pp;
    if((r = can_step(w, w->x, w->y, dir, &ok))) return r;
    if(ok && person_at(w, w->x + DX[dir], w->y + DY[dir], &pp) >= 0) ok = 0;
    if(!ok){ say(log, MP_EV_BUMP, 0, w->x, w->y, 0); return 0; }
    w->x = (uint8_t)(w->x + DX[dir]);
    w->y = (uint8_t)(w->y + DY[dir]);
    uint16_t t;
    uint8_t mv;
    if((r = mp_cell(w, w->x, w->y, &t, &mv))) return r;
    if(w->area.tflags[t] & 1){
        /* a ledge: jump on over it, if there's somewhere to land */
        if((r = can_step(w, w->x, w->y, dir, &ok))) return r;
        if(ok && person_at(w, w->x + DX[dir], w->y + DY[dir], &pp) >= 0) ok = 0;
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
    if(b && alive(w)){
        int k = spotted(w, &r);
        if(r) return r;
        if(k >= 0){
            MgPerson t;
            mg_person(&w->area, (uint32_t)k, &t);
            say(log, MP_EV_SPOTTED, 0, (uint32_t)k, 0, 0);
            w->facing = (uint8_t)((t.facing + 2) & 3);
            say(log, MP_EV_TALK, 0, (uint32_t)k, t.text.len, t.text.off);
            return trainer_battle(w, (uint32_t)k, b, log);
        }
    }
    for(uint32_t k = 0; k < w->area.n_zones; k++){
        MgZone z;
        mg_zone(&w->area, k, &z);
        if(!in_rect(w->x, w->y, z.x, z.y, z.w, z.h)) continue;
        if(alive(w) && b && mp_below(&w->rng, 10000) < z.chance) return encounter(w, mp_table(w, z.table), b, log);
        break;
    }
    return 0;
}

int mp_interact(MpWorld *w, MpBattle *b, MpLog *log){
    if(log){ log->n = 0; log->overflow = 0; }
    if(b) b->over = 1;
    int fx = w->x + DX[w->facing], fy = w->y + DY[w->facing];
    MgPerson p;
    int k = person_at(w, fx, fy, &p);
    if(k >= 0){
        int done = mp_flag(w, p.flag);
        MgStr t = done && p.after.len ? p.after : p.text;
        w->pface[k] = (uint8_t)((w->facing + 2) & 3);
        say(log, MP_EV_TALK, 0, (uint32_t)k, t.len, t.off);
        if(p.kind == MG_P_TRAINER){
            if(done || !b || !alive(w)) return 1;
            int r = trainer_battle(w, (uint32_t)k, b, log);
            return r ? r : 1;
        }
        if(p.kind == MG_P_HEALER){
            mp_heal_party(w);
            w->has_respawn = 1;
            w->respawn_area = w->area.index;
            w->respawn_x = w->x; w->respawn_y = w->y; w->respawn_facing = w->facing;
            say(log, MP_EV_HEALED, 0, (uint32_t)k, 0, 0);
        }
        if(p.kind == MG_P_SHOP) say(log, MP_EV_SHOP, 0, (uint32_t)k, 0, 0);
        if(p.kind == MG_P_GIFT && !done && p.mon[0] < MP_BAG){
            uint32_t have = w->bag[p.mon[0]], n = p.level[0];
            w->bag[p.mon[0]] = (uint8_t)(have + n > MP_BAG_MAX ? MP_BAG_MAX : have + n);
            say(log, MP_EV_GIFT, 0, (uint32_t)k, n, p.mon[0]);
        }
        mp_set_flag(w, p.flag);
        return 1;
    }
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

/* ---- what the Dex remembers (DEX_PLAN.md 4.1) ---- */

static int mark_get(const uint8_t *set, uint32_t id){
    return id < MG_MAX_DEX && set[id / 8] >> (id % 8) & 1;
}
static void mark_set(uint8_t *set, uint32_t id){
    if(id < MG_MAX_DEX) set[id / 8] |= (uint8_t)(1u << (id % 8));
}
int mp_seen(const MpWorld *w, uint32_t id){ return mark_get(w->seen, id); }
int mp_caught(const MpWorld *w, uint32_t id){ return mark_get(w->caught, id); }
void mp_mark_mon(MpWorld *w, uint16_t mon, int caught){
    MgMon m;
    if(mg_mon(w->g, mon, &m)) return;
    mark_set(w->seen, m.id);
    if(caught) mark_set(w->caught, m.id);
}

int mp_new(MpWorld *w, MonGame *g, uint8_t *head, uint32_t seed, uint16_t starter){
    uint32_t cap = w->head_cap;
    memset(w, 0, sizeof *w);
    w->g = g;
    w->head = head;
    w->head_cap = cap;
    w->rng = seed ? seed : 0x9E3779B9u;
    int r = enter_area(w, g->start_area, g->start_x, g->start_y, g->start_facing);
    if(r) return r;
    uint8_t lv = (uint8_t)(g->rules[5] < 5 ? g->rules[5] : 5);
    if((r = mp_make(g, &w->rng, starter, lv, &w->party[0]))) return werr(w, r, "no such creature");
    w->n_party = 1;
    w->money = g->money_start;
    mp_mark_mon(w, starter, 1);
    {
        MgMon first;
        if(!mg_mon(g, starter, &first)) w->starter_id = first.id;
    }
    return 0;
}

/* ---- items and money ---- */

int mp_item_helps(MpWorld *w, uint16_t item, int who, uint32_t *gain){
    MgItem it;
    if(gain) *gain = 0;
    if(item >= w->g->n_items || who < 0 || who >= w->n_party || mg_item(w->g, item, &it)) return 0;
    const MpCreature *c = &w->party[who];
    uint32_t max = c->stat[MG_HP], heal = 0;
    if((it.effects & MG_I_REVIVE) ? c->hp : !c->hp) return 0;     /* a revive is for the fainted only */
    if(it.effects & MG_I_HEAL) heal = it.amount;
    else if(it.effects & MG_I_HEAL_PCT) heal = max * it.amount / 100 ? max * it.amount / 100 : 1;
    {   /* a festering creature gets nothing from a potion (Tuxemon's heal.py) */
        const MgStatus *st = mp_status(w->g, c);
        if(st && st->kind == MG_S_FESTER) heal = 0;
    }
    if(heal > max - c->hp) heal = max - c->hp;
    if(gain) *gain = heal;
    int cure = (it.effects & MG_I_CURE) && c->status && c->status <= w->g->n_statuses &&
               (w->g->statuses[c->status - 1].flags & 1);
    return heal || cure || (it.effects & MG_I_REVIVE);
}

int mp_buy(MpWorld *w, uint16_t item){
    MgItem it;
    if(item >= w->g->n_items || mg_item(w->g, item, &it) || !it.price || it.price > w->money ||
       w->bag[item] >= MP_BAG_MAX) return MG_ERANGE;
    w->money -= it.price;
    w->bag[item]++;
    return 0;
}

/* item i on party member who: the bag's one, its effects, the event */
static void use_item(MpWorld *w, uint16_t item, int who, MpLog *log){
    MgItem it;
    uint32_t heal;
    MpCreature *c = &w->party[who];
    mp_item_helps(w, item, who, &heal);
    mg_item(w->g, item, &it);
    w->bag[item]--;
    if((it.effects & MG_I_REVIVE) && !c->hp && !heal) heal = 1;    /* back on its feet, at least */
    c->hp = (uint16_t)(c->hp + heal);
    if((it.effects & (MG_I_CURE | MG_I_REVIVE)) && c->status && c->status <= w->g->n_statuses &&
       (w->g->statuses[c->status - 1].flags & 1)) c->status = 0;
    say(log, MP_EV_ITEM, 0, item, (uint32_t)who, heal);
}

int mp_use(MpWorld *w, uint16_t item, int who, MpLog *log){
    if(log){ log->n = 0; log->overflow = 0; }
    if(item >= MP_BAG || !w->bag[item] || !mp_item_helps(w, item, who, NULL)) return MG_ERANGE;
    use_item(w, item, who, log);
    return 0;
}

/* ---- battles ------------------------------------------------------------- */

/* ---- the party and the box ---- */

/* would the party still have someone who can fight without member i? */
static int fights_on_without(const MpWorld *w, int i){
    for(int k = 0; k < w->n_party; k++) if(k != i && w->party[k].hp) return 1;
    return 0;
}

static void party_drop(MpWorld *w, int i){
    for(int k = i; k + 1 < w->n_party; k++) w->party[k] = w->party[k + 1];
    w->n_party--;
}

int mp_release(MpWorld *w, int in_box, int i){
    if(in_box){
        if(i < 0 || i >= w->n_box) return MG_ERANGE;
        for(int k = i; k + 1 < w->n_box; k++) w->box[k] = w->box[k + 1];
        w->n_box--;
        return 0;
    }
    if(i < 0 || i >= w->n_party || !fights_on_without(w, i)) return MG_ERANGE;
    party_drop(w, i);
    return 0;
}

int mp_to_box(MpWorld *w, int i){
    if(i < 0 || i >= w->n_party || w->n_box >= MP_BOX || !fights_on_without(w, i)) return MG_ERANGE;
    w->box[w->n_box++] = w->party[i];
    party_drop(w, i);
    return 0;
}

int mp_from_box(MpWorld *w, int i){
    uint32_t room = w->g->rules[7] < MP_PARTY ? w->g->rules[7] : MP_PARTY;
    if(i < 0 || i >= w->n_box || w->n_party >= room) return MG_ERANGE;
    w->party[w->n_party++] = w->box[i];
    for(int k = i; k + 1 < w->n_box; k++) w->box[k] = w->box[k + 1];
    w->n_box--;
    return 0;
}

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

static int trainer_battle(MpWorld *w, uint32_t k, MpBattle *b, MpLog *log){
    MonGame *g = w->g;
    MgPerson p;
    MpCreature team[MG_TEAM];
    int r, n = 0;
    mg_person(&w->area, k, &p);
    for(int j = 0; j < MG_TEAM && p.mon[j] != MG_NONE16; j++, n++)
        if((r = mp_make(g, &w->rng, p.mon[j], p.level[j], &team[j]))) return werr(w, r, "%s", g->err);
    if(!n) return werr(w, MG_EBAD, "a trainer with no creatures");
    if((r = mp_battle_start(w, b, &team[0]))) return werr(w, r, "no creature can fight");
    b->trainer = (uint8_t)(k + 1);
    b->n_team = (uint8_t)n;
    b->next = 1;
    memcpy(b->team, team, sizeof(MpCreature) * (size_t)n);
    say(log, MP_EV_TRAINER, 0, k, 0, 0);
    mp_mark_mon(w, team[0].mon, 0);
    say(log, MP_EV_SENDS, 1, team[0].mon, team[0].level, 0);
    return 0;
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
    if(c->xtype && c->xtype <= g->n_types){ m.type[0] = (uint8_t)(c->xtype - 1); m.type[1] = MG_NONE8; }
    uint16_t mask = g->statuses[st].immune;
    for(int j = 0; j < 2; j++)
        if(m.type[j] != MG_NONE8 && m.type[j] < 16 && (mask >> m.type[j] & 1)) return 1;
    return 0;
}

static MpCreature *side_mon(MpWorld *w, MpBattle *b, int side){
    return side ? &b->wild : &w->party[b->active];
}

static int has_kind(const MonGame *g, const MpCreature *c, int kind){
    const MgStatus *st = mp_status(g, c);
    return st && st->kind == kind;
}

static void end_status(MpCreature *c, int side, MpLog *log){
    say(log, MP_EV_STATUS_END, (uint8_t)side, c->status - 1u, 0, 0);
    c->status = 0;
    c->sturn = 0;
}

/* side's creature takes n HP of harm (not healing). A die-hard status leaves
 * it 1 HP from a blow that would faint it. Returns what it lost. */
static uint32_t harm(MpWorld *w, MpCreature *c, int side, uint32_t n, uint32_t mult, int kind, uint32_t a, MpLog *log){
    if(n > c->hp) n = c->hp;
    if(n && n == c->hp && has_kind(w->g, c, MG_S_DIEHARD)){
        n = c->hp - 1u;
        c->hp = (uint16_t)(c->hp - n);
        say(log, kind, (uint8_t)side, kind == MP_EV_DAMAGE ? mult : a, 0, n);
        say(log, MP_EV_HOLD_ON, (uint8_t)side, c->status - 1u, 0, 0);
        c->status = 0; c->sturn = 0;
        return n;
    }
    c->hp = (uint16_t)(c->hp - n);
    say(log, kind, (uint8_t)side, kind == MP_EV_DAMAGE ? mult : a, 0, n);
    if(!c->hp) say(log, MP_EV_FAINT, (uint8_t)side, 0, 0, 0);
    return n;
}

/* healing t (side ts) by up to h, from a move (festering stops only items: Tuxemon's heal.py) */
static void heal(MpWorld *w, MpCreature *t, int ts, uint32_t h, MpLog *log){
    (void)w;
    if(!t->hp) return;
    if(h > (uint32_t)t->stat[MG_HP] - t->hp) h = (uint32_t)t->stat[MG_HP] - t->hp;
    t->hp = (uint16_t)(t->hp + h);
    say(log, MP_EV_HEAL, (uint8_t)ts, 0, 0, h);
}

/* the foe (side fs) was hit by move m for d HP: its shields and comebacks */
static void struck(MpWorld *w, MpCreature *att, int as, MpCreature *foe, const MgMove *m, uint32_t d, MpLog *log){
    const MgStatus *st = mp_status(w->g, foe);
    if(!st || !foe->hp || !att->hp) return;
    uint32_t n = 0;
    if(st->kind == MG_S_THORNS && (st->ranges >> m->range & 1)) n = foe->stat[MG_HP] / st->divisor;
    else if((st->kind == MG_S_REVENGE || st->kind == MG_S_RETALIATE) && m->range != MG_SPECIAL) n = d;
    if(!n) return;
    uint32_t lost = harm(w, att, as, n, 0, MP_EV_HIT_BACK, foe->status - 1u, log);
    if(st->kind == MG_S_REVENGE) heal(w, foe, !as, lost, log);
}

/* give status st to t (side ts) */
static void give(MpWorld *w, MpBattle *b, MpCreature *t, int ts, uint8_t st, MpLog *log){
    MonGame *g = w->g;
    if(!t->hp || b->gone[ts]) return;
    if(immune(g, t, st)){ say(log, MP_EV_IMMUNE, (uint8_t)ts, st, 0, 0); return; }
    t->status = (uint8_t)(st + 1);
    t->sturn = 0;
    say(log, MP_EV_STATUS, (uint8_t)ts, st, 0, 0);
}

/* side uses its move in slot s */
static int act(MpWorld *w, MpBattle *b, int side, int s, MpLog *log){
    MonGame *g = w->g;
    MpCreature *me = side_mon(w, b, side);
    MpCreature *foe = side_mon(w, b, !side);
    MgMove m;
    if(!me->hp) return 0;
    const MgStatus *mst = mp_status(g, me);
    /* what it holds may keep it from moving, or choose for it */
    if(mst && mst->kind == MG_S_ASLEEP){
        b->land[side] = 0; b->gone[side] = 0;
        if(me->sturn < 255) me->sturn++;
        say(log, MP_EV_ASLEEP, (uint8_t)side, me->status - 1u, 0, 0);
        if(me->sturn > 1 && (mp_below(&w->rng, 100) >= mst->chance || (mst->duration && me->sturn >= mst->duration)))
            end_status(me, side, log);
        return 0;
    }
    int landing = b->land[side] != 0;
    if(landing){ s = b->land[side] - 1; b->land[side] = 0; b->gone[side] = 0; }
    else if(mst && mst->kind == MG_S_FLINCH && mp_below(&w->rng, 100) >= mst->chance){
        say(log, MP_EV_FLINCH, (uint8_t)side, me->status - 1u, 0, 0);
        end_status(me, side, log);
        return 0;
    }
    else if(mst && mst->kind == MG_S_CONFUSED && mp_below(&w->rng, 100) < mst->chance){
        int usable[MP_MOVES], n = 0;
        for(int k = 0; k < MP_MOVES; k++) if(me->moves[k] != MP_NO_MOVE && !b->recharge[side][k]) usable[n++] = k;
        if(n){ s = usable[mp_below(&w->rng, (uint32_t)n)]; say(log, MP_EV_CONFUSED, (uint8_t)side, me->status - 1u, 0, 0); }
    }
    if(s < 0){ say(log, MP_EV_RECHARGE, (uint8_t)side, 0, 0, 0); return 0; }
    int r = mg_move(g, me->moves[s], &m);
    if(r) return werr(w, r, "%s", g->err);
    uint8_t held = me->status;
    say(log, MP_EV_USE, (uint8_t)side, me->moves[s], 0, 0);
    if(!landing) b->recharge[side][s] = (uint8_t)(m.recharge + 1);     /* this turn's end takes one off */
    mp_view_move(g, me, &m);
    int splash = 0, damage = 0, hit = 1;
    for(int e = 0; e < 3; e++){
        if(m.eff_kind[e] == MG_E_SPLASH) splash = m.eff_status[e];
        if(m.eff_kind[e] == MG_E_DAMAGE) damage = 1;
    }
    if(b->gone[!side]){ say(log, MP_EV_OUT_OF_REACH, (uint8_t)side, 0, 0, 0); goto used; }
    if(!landing && mp_below(&w->rng, 10000) >= m.accuracy * 100u){
        if(!splash){ say(log, MP_EV_MISS, (uint8_t)side, 0, 0, 0); goto used; }
        hit = 0;        /* a splash still lands, divided */
    }
    MpCreature va, vf;
    for(int e = 0; e < 3; e++){
        uint8_t k = m.eff_kind[e], v = m.eff_status[e], tg = m.eff_target[e];
        if(!foe->hp && tg == MG_T_ENEMY && k != MG_E_DISAPPEAR) break;
        if(!hit && k != MG_E_SPLASH && k != MG_E_DAMAGE) continue;
        mp_view(g, b, side, me, &va);
        mp_view(g, b, !side, foe, &vf);
        switch(k){
        case MG_E_SPLASH:
            if(damage) break;           /* with a damage effect too, a splash only softens a miss */
            /* fall through */
        case MG_E_DAMAGE:
        case MG_E_DISAPPEAR: {
            if(k == MG_E_DISAPPEAR && !landing){
                b->gone[side] = 1; b->land[side] = (uint8_t)(s + 1);
                say(log, MP_EV_VANISH, (uint8_t)side, 0, 0, 0);
                goto used;
            }
            if(m.range == MG_SPECIAL) break;
            uint32_t mult, d = mp_damage(g, &va, &m, &vf, &mult);
            if(!hit && splash) d /= (uint32_t)splash;
            d = harm(w, foe, !side, d, mult, MP_EV_DAMAGE, 0, log);
            struck(w, me, side, foe, &m, d, log);
            break;
        }
        case MG_E_MULTI: {
            uint32_t hits = 0;
            for(uint32_t i = 0; i < v && foe->hp && me->hp; i++){
                if(i && mp_below(&w->rng, 10000) >= m.accuracy * 100u) break;
                mp_view(g, b, !side, foe, &vf);
                uint32_t mult, d = mp_damage(g, &va, &m, &vf, &mult);
                d = harm(w, foe, !side, d, mult, MP_EV_DAMAGE, 0, log);
                struck(w, me, side, foe, &m, d, log);
                hits++;
            }
            say(log, MP_EV_HITS, (uint8_t)side, hits, 0, 0);
            break;
        }
        case MG_E_HEALING: {
            MpCreature *t = tg == MG_T_OWN ? me : foe;
            heal(w, t, tg == MG_T_OWN ? side : !side, 7 + (uint32_t)me->level * m.heal / 100, log);
            break;
        }
        case MG_E_GIVE:
        case MG_E_REMOVE:
        case MG_E_STAGE: {
            if(mp_below(&w->rng, 10000) >= m.potency * 100u) break;
            for(int t = 0; t < 2; t++){
                if(!(tg == MG_T_BOTH || tg == (t ? MG_T_OWN : MG_T_ENEMY))) continue;
                int ts = t ? side : !side;
                MpCreature *c = t ? me : foe;
                if(!c->hp) continue;
                if(k == MG_E_GIVE) give(w, b, c, ts, v, log);
                else if(k == MG_E_REMOVE){
                    const MgStatus *cs = mp_status(g, c);
                    int neg = cs && (cs->flags & MG_SF_NEGATIVE);
                    if(cs && (v == 3 || (v == 1 && !neg) || (v == 2 && neg))){
                        say(log, MP_EV_REMOVED, (uint8_t)ts, c->status - 1u, 0, 0);
                        c->status = 0; c->sturn = 0;
                    }
                }
                else{
                    int sg = b->stage[ts][v] + m.eff_arg[e];
                    if(sg < -6) sg = -6;
                    if(sg > 6) sg = 6;
                    b->stage[ts][v] = (int8_t)sg;
                    say(log, MP_EV_STAGE, (uint8_t)ts, v, (uint16_t)(int16_t)m.eff_arg[e], (uint32_t)(sg + 6));
                }
            }
            break;
        }
        case MG_E_SWITCH:
            for(int t = 0; t < 2; t++){
                if(!(tg == MG_T_BOTH || tg == (t ? MG_T_OWN : MG_T_ENEMY))) continue;
                MpCreature *c = t ? me : foe;
                MgMon cm;
                if(!c->hp || mg_mon(g, c->mon, &cm)) continue;
                int already = c->xtype ? c->xtype - 1 == v : (cm.type[0] == v || cm.type[1] == v);
                if(!already) c->xtype = (uint8_t)(v + 1);
                say(log, MP_EV_TYPE, (uint8_t)(t ? side : !side), v, (uint16_t)already, 0);
            }
            break;
        case MG_E_PROP_DAMAGE: {
            uint32_t n = (uint32_t)foe->stat[MG_HP] * v / 100;      /* of the target's whole HP */
            for(int t = 0; t < 2; t++){
                if(!(tg == MG_T_BOTH || tg == (t ? MG_T_OWN : MG_T_ENEMY))) continue;
                MpCreature *c = t ? me : foe;
                if(c->hp) harm(w, c, t ? side : !side, n, 100, MP_EV_DAMAGE, 0, log);
            }
            break;
        }
        case MG_E_PROP_HEAL: {
            uint32_t n = (uint32_t)me->stat[MG_HP] * v / 100;       /* of the user's */
            for(int t = 0; t < 2; t++){
                if(!(tg == MG_T_BOTH || tg == (t ? MG_T_OWN : MG_T_ENEMY))) continue;
                heal(w, t ? me : foe, t ? side : !side, n, log);
            }
            break;
        }
        default:            /* appear: the damage after it does the work */
            break;
        }
    }
used:
    /* a status that lasts until its holder's next move (exhausted, charged up,
     * charging) ends now, perhaps becoming the next */
    mst = mp_status(g, me);
    if(me->hp && held && me->status == held && mst && (mst->flags & MG_SF_ON_USE)){
        uint8_t then = mst->then;
        end_status(me, side, log);
        if(then) give(w, b, me, side, (uint8_t)(then - 1), log);
    }
    return 0;
}

/* a status's effect at the end of a turn */
static void status_tick(MpWorld *w, MpBattle *b, MpCreature *c, int side, MpLog *log){
    const MgStatus *st = mp_status(w->g, c);
    if(!c->hp || !st) return;
    uint32_t amount = c->stat[MG_HP] / st->divisor;
    switch(st->kind){
    case MG_S_RECOVER:
        if(amount > (uint32_t)c->stat[MG_HP] - c->hp) amount = (uint32_t)c->stat[MG_HP] - c->hp;
        c->hp = (uint16_t)(c->hp + amount);
        say(log, MP_EV_STATUS_HEAL, (uint8_t)side, c->status - 1u, 0, amount);
        if(c->hp == c->stat[MG_HP]){
            say(log, MP_EV_STATUS_END, (uint8_t)side, c->status - 1u, 0, 0);
            c->status = 0;
        }
        return;
    case MG_S_POISON:
    case MG_S_BURN:
        if(amount > c->hp) amount = c->hp;
        c->hp = (uint16_t)(c->hp - amount);
        say(log, MP_EV_STATUS_HURT, (uint8_t)side, c->status - 1u, 0, amount);
        if(!c->hp) say(log, MP_EV_FAINT, (uint8_t)side, 0, 0, 0);
        return;
    case MG_S_WASTING:
        if(c->sturn < 255) c->sturn++;
        harm(w, c, side, amount * c->sturn, 0, MP_EV_STATUS_HURT, c->status - 1u, log);
        return;
    case MG_S_LEECH: {
        MpCreature *o = side_mon(w, b, !side);
        if(!o->hp){ end_status(c, side, log); return; }
        uint8_t st_pos = (uint8_t)(c->status - 1u);
        if(amount > c->hp) amount = c->hp;
        c->hp = (uint16_t)(c->hp - amount);
        say(log, MP_EV_DRAIN, (uint8_t)side, st_pos, 0, amount);
        if(!c->hp) say(log, MP_EV_FAINT, (uint8_t)side, 0, 0, 0);
        if(amount > (uint32_t)o->stat[MG_HP] - o->hp) amount = (uint32_t)o->stat[MG_HP] - o->hp;
        o->hp = (uint16_t)(o->hp + amount);
        return;
    }
    default:
        return;
    }
}

/* side's creature is about to change (a swap, the keeper's next): what was
 * the battle's alone goes, and a linked status on either creature ends */
static void side_leaves(MpWorld *w, MpBattle *b, int side, int swapped, MpLog *log){
    MonGame *g = w->g;
    MpCreature *out = side_mon(w, b, side), *other = side_mon(w, b, !side);
    const MgStatus *st = mp_status(g, out);
    if(swapped && st && st->kind == MG_S_SWAPHURT && out->hp)
        harm(w, out, side, out->stat[MG_HP] / st->divisor, 0, MP_EV_STATUS_HURT, out->status - 1u, log);
    st = mp_status(g, out);
    if(st && (st->flags & MG_SF_LINKED)){ out->status = 0; out->sturn = 0; }
    st = mp_status(g, other);
    if(st && (st->flags & MG_SF_LINKED) && other->hp) end_status(other, !side, log);
    memset(b->stage[side], 0, sizeof b->stage[side]);
    b->gone[side] = 0; b->land[side] = 0;
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
            mp_mark_mon(w, c->mon, 1);
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
        c->sturn = 0; c->xtype = 0;
    }
    b->over = 1;
}

/* who won, who must swap, and the experience */
static int resolve(MpWorld *w, MpBattle *b, MpLog *log){
    MonGame *g = w->g;
    int r;
    if(!alive(w)){
        mp_heal_party(w);
        if(w->has_respawn) r = enter_area(w, w->respawn_area, w->respawn_x, w->respawn_y, w->respawn_facing);
        else r = enter_area(w, g->start_area, g->start_x, g->start_y, g->start_facing);
        if(r) return r;
        say(log, MP_EV_LOSE, 0, 0, 0, 0);
        battle_end(w, b);
        return 0;
    }
    if(!b->wild.hp){
        uint32_t gain = mp_exp_for(g, b->wild.mon, b->wild.level) / b->wild.level, n = 0;
        if(b->trainer) gain = (uint32_t)((uint64_t)gain * g->keeper_bonus / 100);
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
        if(b->trainer && b->next < b->n_team){
            /* the trainer's next creature; those who fight it share its experience */
            side_leaves(w, b, 1, 0, log);
            b->wild = b->team[b->next++];
            memset(b->recharge[1], 0, sizeof b->recharge[1]);
            mp_mark_mon(w, b->wild.mon, 0);
            say(log, MP_EV_SENDS, 1, b->wild.mon, b->wild.level, 0);
            b->fought = (uint8_t)(1u << b->active);
            if(!w->party[b->active].hp){
                b->need_swap = 1;
                say(log, MP_EV_NEED_SWAP, 0, 0, 0, 0);
            }
            return 0;
        }
        say(log, MP_EV_WIN, 0, 0, 0, 0);
        if(b->trainer){
            MgPerson p;
            mg_person(&w->area, b->trainer - 1u, &p);
            mp_set_flag(w, p.flag);
            say(log, MP_EV_BEATEN, 0, b->trainer - 1u, 0, 0);
            uint32_t prize = 0;
            for(int i = 0; i < b->n_team; i++) prize += b->team[i].level;
            prize *= g->money_per_level;
            if(prize){
                w->money = w->money + prize > MP_MONEY_MAX ? MP_MONEY_MAX : w->money + prize;
                say(log, MP_EV_PRIZE, 0, b->trainer - 1u, 0, prize);
            }
            if(p.after.len) say(log, MP_EV_TALK, 0, b->trainer - 1u, p.after.len, p.after.off);
        }
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
    mp_mark_mon(w, b->wild.mon, 1);
    {
        const MgStatus *st = mp_status(g, &b->wild);
        if(st && !(st->flags & MG_SF_PERSISTS)) b->wild.status = 0;
        b->wild.sturn = 0; b->wild.xtype = 0;
    }
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
        if(b->trainer || (w->n_party >= room && w->n_box >= MP_BOX)) return MG_ERANGE;
        break;
    }
    case MP_ACT_RUN:
        if(b->trainer) return MG_ERANGE;
        break;
    case MP_ACT_ITEM:
        if(arg < 0 || (arg & 0xFF) >= MP_BAG || !w->bag[arg & 0xFF] || !mp_item_helps(w, (uint16_t)(arg & 0xFF), arg >> 8, NULL))
            return MG_ERANGE;
        break;
    default:
        return MG_ERANGE;
    }

    if(act_kind == MP_ACT_SWAP){
        side_leaves(w, b, 0, !b->need_swap, log);
        b->active = (uint8_t)arg;
        b->fought |= (uint8_t)(1u << arg);
        say(log, MP_EV_SWAP, 0, (uint32_t)arg, 0, 0);
        if(b->need_swap){ b->need_swap = 0; return 0; }      /* a fainted creature replaced: no turn */
    }
    if(act_kind == MP_ACT_ITEM) use_item(w, (uint16_t)(arg & 0xFF), arg >> 8, log);
    int foe_slot = mp_wild_choice(w, b);
    if(b->foe_set){
        b->foe_set = 0;
        if(b->foe_slot < MP_MOVES && b->wild.moves[b->foe_slot] != MP_NO_MOVE && !b->recharge[1][b->foe_slot])
            foe_slot = b->foe_slot;
    }
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
            MpCreature vm, vw;
            mp_view(g, b, 0, me, &vm);
            mp_view(g, b, 1, &b->wild, &vw);
            uint32_t sp = mp_speed_score(&vm, &pm), sf = mp_speed_score(&vw, &fm);
            player_first = sp > sf || (sp == sf && !mp_below(&w->rng, 2));
        }
        if(player_first){
            if((r = act(w, b, 0, arg, log)) || (r = act(w, b, 1, foe_slot, log))) return r;
        }
        else if((r = act(w, b, 1, foe_slot, log)) || (r = act(w, b, 0, arg, log))) return r;
    }
    else if((r = act(w, b, 1, foe_slot, log))) return r;

    status_tick(w, b, &w->party[b->active], 0, log);
    status_tick(w, b, &b->wild, 1, log);
    for(int s = 0; s < 2; s++)
        for(int k = 0; k < MP_MOVES; k++) if(b->recharge[s][k]) b->recharge[s][k]--;
    return resolve(w, b, log);
}

/* ---- the save -------------------------------------------------------------
 * A 32-byte header: "CYDS", u16 version (3), u16 box count, u32 the game
 * file's fingerprint, u32 the generator, u32 area id, u8 x, y, facing, party
 * count, u32 CRC-32 of the records, u32 CRC-32 of bytes 0-27. Then, from
 * version 2, a 40-byte record of the story: the flags (32 B, flag f is bit
 * f % 8 of byte f / 8), u32 the area id a loss comes back to (0: the start),
 * u8 x, y, facing, u8 the player's look. Then 40 bytes per creature, the party first: u32
 * creature id, u32 experience, u16 HP, u8 level, u8 0, u32 status id (0
 * none), u8 iv x 6, u16 0, u32 move id x 4 (0 none). The records' CRC covers
 * the story's record too. Ids, not positions, so a rebuilt game file
 * (game_ids.tsv keeps the numbers) still loads the save. A version 1 save
 * (no story record) loads with no flags set. From version 3, after the story,
 * 80 bytes of money and bag: u32 coins, u32 0, then 9 x (u32 item id, u16
 * count, u16 0), unused slots zero; a version 1 or 2 save starts with META
 * 0x24's coins and nothing in the bag. Replaced whole (safefile.h). */

#define BAG_BYTES 80
#define MARK_BYTES (2 * MP_MARK_BYTES + 8)      /* version 4: seen, caught, the first critter's id, 4 B reserved */

static void put_bag(MpWorld *w, uint8_t *p){
    memset(p, 0, BAG_BYTES);
    put32(p, w->money);
    int n = 0;
    for(uint32_t i = 0; i < w->g->n_items && i < MP_BAG; i++){
        MgItem it;
        if(!w->bag[i] || mg_item(w->g, i, &it)) continue;
        put32(p + 8 + 8 * n, it.id);
        put16(p + 12 + 8 * n, w->bag[i]);
        n++;
    }
}

static void put_marks(MpWorld *w, uint8_t *p){
    memset(p, 0, MARK_BYTES);
    memcpy(p, w->seen, MP_MARK_BYTES);
    memcpy(p + MP_MARK_BYTES, w->caught, MP_MARK_BYTES);
    put32(p + 2 * MP_MARK_BYTES, w->starter_id);
}

static void put_story(MpWorld *w, uint8_t *p){
    MgAreaDir d;
    memset(p, 0, MP_SAVE_REC);
    memcpy(p, w->flags, sizeof w->flags);
    if(w->has_respawn && !mg_area_dir(w->g, w->respawn_area, &d)){
        put32(p + 32, d.id);
        p[36] = w->respawn_x; p[37] = w->respawn_y; p[38] = w->respawn_facing;
    }
    p[39] = w->look;
}

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
    uint8_t h[MP_SAVE_HEADER], rec[MP_SAVE_REC], story[MP_SAVE_REC], bag[BAG_BYTES], marks[MARK_BYTES];
    uint32_t crc, n = (uint32_t)w->n_party + w->n_box;
    SafeFile sf;
    put_story(w, story);
    put_bag(w, bag);
    put_marks(w, marks);
    crc = mg_crc32(0, story, MP_SAVE_REC);
    crc = mg_crc32(crc, bag, BAG_BYTES);
    crc = mg_crc32(crc, marks, MARK_BYTES);
    for(uint32_t i = 0; i < n; i++){
        put_rec(w->g, i < w->n_party ? &w->party[i] : &w->box[i - w->n_party], rec);
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
    }
    memset(h, 0, sizeof h);
    memcpy(h, "CYDS", 4);
    put16(h + 4, MP_SAVE_VERSION);
    put16(h + 6, w->n_box);
    put32(h + 8, w->g->fingerprint);
    put32(h + 12, w->rng);
    put32(h + 16, w->area.dir.id);
    h[20] = w->x; h[21] = w->y; h[22] = w->facing; h[23] = w->n_party;
    put32(h + 24, crc);
    put32(h + 28, mg_crc32(0, h, 28));
    FILE *f = sf_open(&sf, path, "wb");
    if(!f) return werr(w, MG_EIO, "can't write the save");
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h && fwrite(story, 1, sizeof story, f) == sizeof story &&
             fwrite(bag, 1, sizeof bag, f) == sizeof bag && fwrite(marks, 1, sizeof marks, f) == sizeof marks;
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
    uint32_t cap = w->head_cap;
    memset(w, 0, sizeof *w);
    w->g = g;
    w->head = head;
    w->head_cap = cap;
    sf_recover(path);
    FILE *f = fopen(path, "rb");
    if(!f) return werr(w, MG_EIO, "no save");
    r = MG_EBAD;
    if(fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "CYDS", 4) || mg_crc32(0, h, 28) != le32(h + 28)){
        werr(w, MG_EBAD, "the save is damaged");
        goto out;
    }
    uint32_t version = le16(h + 4);
    if(!version || version > MP_SAVE_VERSION){ r = werr(w, MG_EVERSION, "a save from a newer version"); goto out; }
    uint32_t nbox = le16(h + 6), nparty = h[23];
    if(!nparty || nparty > MP_PARTY || nparty > g->rules[7] || nbox > MP_BOX){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
    uint32_t crc = 0;
    if(version >= 2){
        if(fread(rec, 1, MP_SAVE_REC, f) != MP_SAVE_REC){ werr(w, MG_EBAD, "the save is cut short"); goto out; }
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
        if(rec[0] & 1 || rec[39] >= g->looks){ werr(w, MG_EBAD, "the save is damaged"); goto out; }   /* flag 0 is none */
        memcpy(w->flags, rec, sizeof w->flags);
        w->look = rec[39];
        uint32_t rid = le32(rec + 32);
        if(rid){
            MgAreaDir d;
            if(mg_find_area(g, rid, &w->respawn_area) || mg_area_dir(g, w->respawn_area, &d) ||
               rec[36] >= d.w || rec[37] >= d.h || rec[38] > 3){
                werr(w, MG_EBAD, "the save rests in an area this game file doesn't have"); goto out;
            }
            w->has_respawn = 1;
            w->respawn_x = rec[36]; w->respawn_y = rec[37]; w->respawn_facing = rec[38];
        }
        else if(rec[36] || rec[37] || rec[38]){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
    }
    w->money = g->money_start;
    if(version >= 3){
        uint8_t bag[BAG_BYTES];
        if(fread(bag, 1, BAG_BYTES, f) != BAG_BYTES){ werr(w, MG_EBAD, "the save is cut short"); goto out; }
        crc = mg_crc32(crc, bag, BAG_BYTES);
        w->money = le32(bag);
        if(w->money > MP_MONEY_MAX || le32(bag + 4)){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
        for(int i = 0; i < 9; i++){
            const uint8_t *s = bag + 8 + 8 * i;
            uint16_t pos;
            if(!le32(s)){
                if(le16(s + 4) || le16(s + 6)){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
                continue;
            }
            if(mg_find_item(g, le32(s), &pos)){ werr(w, MG_EBAD, "the save has an item this game file doesn't"); goto out; }
            if(!le16(s + 4) || le16(s + 4) > MP_BAG_MAX || le16(s + 6) || w->bag[pos]){
                werr(w, MG_EBAD, "the save is damaged"); goto out;
            }
            w->bag[pos] = (uint8_t)le16(s + 4);
        }
    }
    if(version >= 4){
        uint8_t marks[MARK_BYTES];
        if(fread(marks, 1, MARK_BYTES, f) != MARK_BYTES){ werr(w, MG_EBAD, "the save is cut short"); goto out; }
        crc = mg_crc32(crc, marks, MARK_BYTES);
        if(le32(marks + 2 * MP_MARK_BYTES + 4)){ werr(w, MG_EBAD, "the save is damaged"); goto out; }
        memcpy(w->seen, marks, MP_MARK_BYTES);
        memcpy(w->caught, marks + MP_MARK_BYTES, MP_MARK_BYTES);
        w->starter_id = le32(marks + 2 * MP_MARK_BYTES);
    }
    for(uint32_t i = 0; i < nparty + nbox; i++){
        if(fread(rec, 1, MP_SAVE_REC, f) != MP_SAVE_REC){ werr(w, MG_EBAD, "the save is cut short"); goto out; }
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
        if((r = get_rec(w, rec, i < nparty ? &w->party[i] : &w->box[i - nparty]))) goto out;
        r = MG_EBAD;
        /* an older save: what the player owns now is all it knows of */
        if(version < 4){
            mark_set(w->seen, le32(rec));
            mark_set(w->caught, le32(rec));
        }
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
        w->g = g; w->head = head; w->head_cap = cap;
        memcpy(w->err, e, sizeof e);
    }
    return r;
}

/* The Dex's reading of a save: everything checked as mp_load does but the
 * records' meanings, which only matter to a game file that's being played. */
int mp_read_marks(const char *path, MpMarks *out){
    uint8_t h[MP_SAVE_HEADER], rec[MP_SAVE_REC];
    int r = MG_EBAD;
    memset(out, 0, sizeof *out);
    sf_recover(path);
    FILE *f = fopen(path, "rb");
    if(!f) return MG_EIO;
    if(fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "CYDS", 4) || mg_crc32(0, h, 28) != le32(h + 28)) goto out;
    uint32_t version = le16(h + 4), n = (uint32_t)h[23] + le16(h + 6), crc = 0;
    if(!version || version > MP_SAVE_VERSION){ r = MG_EVERSION; goto out; }
    if(!h[23] || h[23] > MP_PARTY || le16(h + 6) > MP_BOX) goto out;
    out->version = (uint8_t)version;
    if(version >= 2){
        if(fread(rec, 1, MP_SAVE_REC, f) != MP_SAVE_REC) goto out;
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
        memcpy(out->flags, rec, sizeof out->flags);
    }
    if(version >= 3){
        uint8_t bag[BAG_BYTES];
        if(fread(bag, 1, BAG_BYTES, f) != BAG_BYTES) goto out;
        crc = mg_crc32(crc, bag, BAG_BYTES);
    }
    if(version >= 4){
        uint8_t marks[MARK_BYTES];
        if(fread(marks, 1, MARK_BYTES, f) != MARK_BYTES) goto out;
        crc = mg_crc32(crc, marks, MARK_BYTES);
        memcpy(out->seen, marks, MP_MARK_BYTES);
        memcpy(out->caught, marks + MP_MARK_BYTES, MP_MARK_BYTES);
        out->starter = le32(marks + 2 * MP_MARK_BYTES);
    }
    for(uint32_t i = 0; i < n; i++){
        if(fread(rec, 1, MP_SAVE_REC, f) != MP_SAVE_REC) goto out;
        crc = mg_crc32(crc, rec, MP_SAVE_REC);
        if(version < 4){
            mark_set(out->seen, le32(rec));
            mark_set(out->caught, le32(rec));
        }
    }
    if(fgetc(f) != EOF || crc != le32(h + 24)) goto out;
    r = 0;
out:
    fclose(f);
    if(r){ uint8_t v = out->version; memset(out, 0, sizeof *out); out->version = v; }
    return r;
}
