/* monster_bot.c -- Critters' Auto-play (monster_bot.h says how it plays). */
#include "monster_bot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };

#define START   0xFF            /* prev[] of the cell the search starts from */
#define HOP     8               /* prev[]: the step there hopped a ledge */
#define K_LEDGE 1               /* kind[] bits */
#define K_WARP  2
#define K_FOLK  4               /* someone stands there now */
#define K_SEEN  8               /* a keeper not yet beaten would spot the player there */
#define FAR     0xFFFFu
#define NONE    0xFFFFFFFFu

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }

int mb_init(MbBot *b, MonGame *g, uint32_t seed, uint8_t *scratch, uint32_t cap){
    memset(b, 0, sizeof *b);
    b->g = g;
    b->rng = seed ? seed : 0x2545F491u;
    b->grid_area = -1;
    b->n_areas = (uint16_t)(g->n_areas < MB_MAX_AREAS ? g->n_areas : MB_MAX_AREAS);
    for(uint32_t t = 0; t < g->n_tables && t < MB_MAX_TABLES; t++){
        uint16_t n;
        if(mg_enc_rows(g, t, &n)) continue;
        for(uint32_t j = 0; j < n; j++){
            MgEncRow row;
            int r = mg_enc_row(g, t, j, &row);
            if(r) return r;
            if(row.hi > b->table_hi[t]) b->table_hi[t] = row.hi;
        }
    }
    for(uint32_t i = 0; i < b->n_areas; i++){
        MgArea a;
        int r = mg_area_open(g, i, &a, scratch, cap);
        if(r) return r;
        for(uint32_t k = 0; k < a.n_warps && b->n_warps < MB_MAX_WARPS; k++){
            MgWarp wp;
            mg_warp(&a, k, &wp);
            if(wp.to >= b->n_areas) continue;
            MbWarp *o = &b->warps[b->n_warps++];
            o->area = (uint8_t)i; o->x = wp.x; o->y = wp.y; o->w = wp.w; o->h = wp.h;
            o->facing = wp.facing; o->to = wp.to;
        }
        for(uint32_t k = 0; k < a.n_people && b->n_people < MB_MAX_PEOPLE; k++){
            MgPerson p;
            mg_person(&a, k, &p);
            if(p.kind != MG_P_TRAINER && p.kind != MG_P_HEALER && p.kind != MG_P_SHOP) continue;
            MbPerson *o = &b->people[b->n_people++];
            o->area = (uint8_t)i; o->x = p.x; o->y = p.y; o->kind = p.kind; o->index = (uint8_t)k;
            o->flag = p.flag; o->hide_if = p.hide_if; o->show_if = p.show_if;
            for(int j = 0; j < MG_TEAM; j++) if(p.mon[j] != MG_NONE16 && p.level[j] > o->top) o->top = p.level[j];
        }
        for(uint32_t k = 0; k < a.n_zones && b->n_zones < MB_MAX_ZONES; k++){
            MgZone z;
            uint16_t n;
            mg_zone(&a, k, &z);
            if(!z.chance || mg_enc_rows(g, z.table, &n) || !n) continue;
            MbZone *o = &b->zones[b->n_zones++];
            o->area = (uint8_t)i; o->x = z.x; o->y = z.y; o->w = z.w; o->h = z.h; o->table = z.table;
        }
    }
    return 0;
}

void mb_free(MbBot *b){
    free(b->queue);
    b->queue = NULL;
    b->cell = b->kind = b->prev = b->door = NULL;
    b->grid_area = -1;
}

/* ---- the current area's cells -------------------------------------------- */

/* 0 ready; 1 too big to search; < 0 the card */
static int grid(MbBot *b, MpWorld *w){
    if(b->grid_area == (int32_t)w->area.index) return 0;
    mb_free(b);
    uint32_t n = (uint32_t)w->area.w * w->area.h;
    if(!n || n > MB_MAX_CELLS) return 1;
    uint8_t *mem = malloc(n * 6);            /* the queue first, for its alignment */
    if(!mem) return 1;
    b->queue = (uint16_t *)(void *)mem;
    b->cell = mem + 2 * n;
    b->kind = b->cell + n;
    b->prev = b->kind + n;
    b->door = b->prev + n;
    for(uint32_t cy = 0; cy < w->area.crows; cy++)
        for(uint32_t cx = 0; cx < w->area.ccols; cx++){
            int r = mg_chunk(b->g, &w->area, cy * w->area.ccols + cx, b->chunk);
            if(r){ mb_free(b); return r; }
            for(uint32_t j = 0; j < MG_CHUNK * MG_CHUNK; j++){
                uint32_t x = cx * MG_CHUNK + j % MG_CHUNK, y = cy * MG_CHUNK + j / MG_CHUNK;
                if(x >= w->area.w || y >= w->area.h) continue;
                uint16_t t = le16(b->chunk + 2 * j);
                b->cell[y * w->area.w + x] = b->chunk[2 * MG_CHUNK * MG_CHUNK + j];
                b->kind[y * w->area.w + x] = (uint8_t)(t <= w->area.n_tiles && (w->area.tflags[t] & 1) ? K_LEDGE : 0);
            }
        }
    b->gw = w->area.w;
    b->gh = w->area.h;
    b->grid_area = w->area.index;
    return 0;
}

static int present(const MpWorld *w, const MbPerson *p){
    return (!p->show_if || mp_flag(w, p->show_if)) && !(p->hide_if && mp_flag(w, p->hide_if));
}

static int step_ok(const MbBot *b, int x, int y, int dir);

/* who stands where, what keepers can see, and where the doors are */
static void mark(MbBot *b, const MpWorld *w){
    uint32_t n = (uint32_t)b->gw * b->gh;
    for(uint32_t i = 0; i < n; i++) b->kind[i] &= K_LEDGE;
    for(uint32_t k = 0; k < w->area.n_people; k++){
        MgPerson p;
        mg_person(&w->area, k, &p);
        if(mp_present(w, &p) && p.x < b->gw && p.y < b->gh) b->kind[p.y * b->gw + p.x] |= K_FOLK;
    }
    for(uint32_t k = 0; k < w->area.n_people; k++){     /* their sight, as the rules' spotted() */
        MgPerson p;
        mg_person(&w->area, k, &p);
        if(p.kind != MG_P_TRAINER || !p.sight || mp_flag(w, p.flag) || !mp_present(w, &p)) continue;
        int x = p.x, y = p.y, d = w->pface[k] & 3;
        for(int i = 1; i <= p.sight && x < b->gw && y < b->gh && step_ok(b, x, y, d); i++){
            x += DX[d]; y += DY[d];
            if(b->kind[y * b->gw + x] & K_FOLK) break;
            b->kind[y * b->gw + x] |= K_SEEN;
        }
    }
    for(uint32_t k = 0; k < w->area.n_warps; k++){
        MgWarp wp;
        mg_warp(&w->area, k, &wp);
        for(int y = wp.y; y < wp.y + wp.h && y < b->gh; y++)
            for(int x = wp.x; x < wp.x + wp.w && x < b->gw; x++) b->kind[y * b->gw + x] |= K_WARP;
    }
}

/* the warp a step facing dir onto (x, y) takes, as the rules pick it, or -1 */
static int warp_at(const MbBot *b, const MpWorld *w, int x, int y, int dir, MgWarp *out){
    if(!(b->kind[y * b->gw + x] & K_WARP)) return -1;
    for(uint32_t k = 0; k < w->area.n_warps; k++){
        MgWarp wp;
        mg_warp(&w->area, k, &wp);
        if(x < wp.x || y < wp.y || x >= wp.x + wp.w || y >= wp.y + wp.h) continue;
        if(wp.facing != MG_NONE8 && wp.facing != dir) continue;
        if(out) *out = wp;
        return (int)k;
    }
    return -1;
}

/* the rules' can_step, on the bot's copy of the cells */
static int step_ok(const MbBot *b, int x, int y, int dir){
    int nx = x + DX[dir], ny = y + DY[dir];
    if(nx < 0 || ny < 0 || nx >= b->gw || ny >= b->gh) return 0;
    return !(b->cell[y * b->gw + x] & (16 << dir)) && !(b->cell[ny * b->gw + nx] & (1 << ((dir + 2) & 3)));
}

/* Breadth first from the player: prev[] says how each cell was first
 * reached to stand on, door[] how it was first reached as a warp. A step
 * that warps ends there (the cell is a door, not a place to go on from), and
 * the same cell may be a place too when it's entered facing another way (a
 * warp that wants "up" on a row one can walk along). A ledge hops on, as
 * mp_step does. */
static void search(MbBot *b, const MpWorld *w, int unseen){
    uint32_t n = (uint32_t)b->gw * b->gh, head = 0, tail = 0;
    memset(b->prev, 0, n);
    memset(b->door, 0, n);
    uint32_t s = (uint32_t)w->y * b->gw + w->x;
    b->prev[s] = START;
    b->queue[tail++] = (uint16_t)s;
    while(head < tail){
        uint32_t u = b->queue[head++];
        int ux = (int)(u % b->gw), uy = (int)(u / b->gw);
        for(int d = 0; d < 4; d++){
            if(!step_ok(b, ux, uy, d)) continue;
            int vx = ux + DX[d], vy = uy + DY[d], hop = 0;
            if(b->kind[vy * b->gw + vx] & (unseen ? K_FOLK | K_SEEN : K_FOLK)) continue;
            if(b->kind[vy * b->gw + vx] & K_LEDGE && step_ok(b, vx, vy, d) &&
               !(b->kind[(vy + DY[d]) * b->gw + vx + DX[d]] & K_FOLK)){
                vx += DX[d]; vy += DY[d]; hop = 1;
            }
            uint32_t f = (uint32_t)vy * b->gw + (uint32_t)vx;
            uint8_t how = (uint8_t)(1 + d + (hop ? HOP : 0));
            if(warp_at(b, w, vx, vy, d, NULL) >= 0){
                if(!b->door[f]) b->door[f] = how;
                continue;
            }
            if(b->prev[f]) continue;
            b->prev[f] = how;
            b->queue[tail++] = (uint16_t)f;
        }
    }
}

/* the path to cell c, as a place (or through it, a door): its first step's
 * direction (-1 if c is the start) and its length */
static int path(const MbBot *b, uint32_t c, int door, int *len){
    int first = -1, n = 0;
    for(;;){
        int p = door ? b->door[c] : b->prev[c];
        door = 0;
        if(p == START) break;
        int d = (p - 1) & 3, k = p & HOP ? 2 : 1;
        c = (uint32_t)((int)c - k * (DY[d] * b->gw + DX[d]));
        first = d;
        n++;
    }
    if(len) *len = n;
    return first;
}

/* a cell the player can reach and stand on */
static int standable(const MbBot *b, int x, int y){
    return x >= 0 && y >= 0 && x < b->gw && y < b->gh && b->prev[(uint32_t)y * b->gw + x];
}

/* ---- between areas --------------------------------------------------------- */

/* hops[a]: how many warps from area a to area `to` */
static void hops_to(const MbBot *b, uint16_t to, uint16_t *hops){
    for(int i = 0; i < b->n_areas; i++) hops[i] = FAR;
    hops[to] = 0;
    for(int pass = 0; pass < b->n_areas; pass++){
        int changed = 0;
        for(int k = 0; k < b->n_warps; k++){
            const MbWarp *wp = &b->warps[k];
            if(hops[wp->to] != FAR && hops[wp->to] + 1 < hops[wp->area]){
                hops[wp->area] = (uint16_t)(hops[wp->to] + 1);
                changed = 1;
            }
        }
        if(!changed) break;
    }
}

/* Toward area `to`: the nearest reachable warp of this area that leads
 * closer. Its length and first step, or FAR if there's none. */
static uint32_t toward(MbBot *b, const MpWorld *w, uint16_t to, int *dir){
    uint16_t hops[MB_MAX_AREAS];
    uint32_t best = FAR;
    hops_to(b, to, hops);
    uint16_t here = w->area.index;
    if(here >= b->n_areas || hops[here] == FAR) return FAR;
    for(uint32_t k = 0; k < w->area.n_warps; k++){
        MgWarp wp;
        mg_warp(&w->area, k, &wp);
        if(wp.to >= b->n_areas || hops[wp.to] >= hops[here]) continue;
        for(int y = wp.y; y < wp.y + wp.h && y < b->gh; y++)
            for(int x = wp.x; x < wp.x + wp.w && x < b->gw; x++){
                uint32_t c = (uint32_t)y * b->gw + x;
                MgWarp got;
                if(!b->door[c] || warp_at(b, w, x, y, (b->door[c] - 1) & 3, &got) < 0 || got.to != wp.to) continue;
                int len, d = path(b, c, 1, &len);
                uint32_t score = (uint32_t)hops[wp.to] * 4096u + (uint32_t)len;
                if(score < best){ best = score; *dir = d; }
            }
    }
    return best;
}

/* ---- the party ------------------------------------------------------------- */

static int best_level(const MpWorld *w){
    int lv = 0;
    for(int i = 0; i < w->n_party; i++) if(w->party[i].level > lv) lv = w->party[i].level;
    return lv;
}

/* the party's HP, in hundredths of its whole */
static uint32_t health(const MpWorld *w){
    uint32_t hp = 0, max = 0;
    for(int i = 0; i < w->n_party; i++){ hp += w->party[i].hp; max += w->party[i].stat[MG_HP]; }
    return max ? hp * 100 / max : 0;
}

/* worn down: under 40 % of the party's HP, or its best creature under a third */
static int worn(const MpWorld *w){
    uint32_t hp = 0, max = 0;
    int best = -1;
    for(int i = 0; i < w->n_party; i++){
        hp += w->party[i].hp;
        max += w->party[i].stat[MG_HP];
        if(best < 0 || w->party[i].level > w->party[best].level) best = i;
    }
    if(!max || hp * 10 < max * 4) return 1;
    return best >= 0 && w->party[best].hp * 3u < w->party[best].stat[MG_HP];
}

/* ---- on the map ------------------------------------------------------------ */

/* Toward person p: next to them facing them, then A. Its score (fewer areas
 * away first, then fewer steps) and what to do, or FAR if they can't be reached. */
static uint32_t to_person(MbBot *b, const MpWorld *w, const MbPerson *p, int *what, int *dir){
    if(p->area != w->area.index){
        int d = 0;
        uint32_t s = toward(b, w, p->area, &d);
        if(s == FAR) return FAR;
        *what = MB_STEP; *dir = d;
        return s;
    }
    uint32_t best = FAR;
    for(int d = 0; d < 4; d++){             /* stand at p - d, face d */
        int x = p->x - DX[d], y = p->y - DY[d];
        if(!standable(b, x, y)) continue;
        if(x == w->x && y == w->y){
            *what = w->facing == d ? MB_TALK : MB_STEP;
            *dir = d;
            return 0;
        }
        int len, first = path(b, (uint32_t)y * b->gw + x, 0, &len);
        if((uint32_t)len < best){ best = (uint32_t)len; *what = MB_STEP; *dir = first; }
    }
    return best;
}

static int in_zone(const MbBot *b, const MpWorld *w, int x, int y){
    for(int k = 0; k < b->n_zones; k++){
        const MbZone *z = &b->zones[k];
        if(z->area == w->area.index && x >= z->x && y >= z->y && x < z->x + z->w && y < z->y + z->h) return 1;
    }
    return 0;
}

/* Train: the zone whose wild levels suit the party best (its top a level or
 * two above the party's best), the nearest of those; on it, walk up and down. */
static int grind(MbBot *b, const MpWorld *w, int *dir){
    int want = best_level(w) + 2, here = -1;
    uint32_t best = NONE;
    int bd = 0;
    for(int k = 0; k < b->n_zones; k++){
        const MbZone *z = &b->zones[k];
        uint16_t t = mp_table(w, z->table);
        uint32_t fit = (uint32_t)abs((t < MB_MAX_TABLES ? b->table_hi[t] : 0) - want);
        if(z->area != w->area.index){
            int d = 0;
            uint32_t s = toward(b, w, z->area, &d);
            if(s != FAR && fit * 65536u + s < best){ best = fit * 65536u + s; bd = d; }
            continue;
        }
        if(w->x >= z->x && w->y >= z->y && w->x < z->x + z->w && w->y < z->y + z->h && (here < 0 || (int)fit < here))
            here = (int)fit;
        for(int y = z->y; y < z->y + z->h && y < b->gh; y++)
            for(int x = z->x; x < z->x + z->w && x < b->gw; x++){
                if(!standable(b, x, y) || (x == w->x && y == w->y)) continue;
                int len, d = path(b, (uint32_t)y * b->gw + x, 0, &len);
                if(fit * 65536u + (uint32_t)len < best){ best = fit * 65536u + (uint32_t)len; bd = d; }
            }
    }
    if(here >= 0 && (best == NONE || (uint32_t)here <= best / 65536u)){
        /* on good grass: on along the same way, else back, else aside */
        static const uint8_t TURN[4] = { 0, 2, 1, 3 };
        for(int t = 0; t < 4; t++){
            int d = (b->pace + TURN[t]) & 3, x = w->x + DX[d], y = w->y + DY[d];
            if(x < 0 || y < 0 || x >= b->gw || y >= b->gh) continue;
            if(b->prev[(uint32_t)y * b->gw + x] != 1 + d || !in_zone(b, w, x, y) || !standable(b, x, y)) continue;
            b->pace = (uint8_t)d;
            *dir = d;
            return 1;
        }
    }
    if(best == NONE) return 0;
    *dir = bd;
    return 1;
}

/* to the nearest healer (or shop): its score (fewer areas away first, then
 * fewer steps) and the press, or FAR if none can be reached */
static uint32_t to_nearest(MbBot *b, const MpWorld *w, int kind, int *what, int *dir){
    uint32_t near = FAR;
    for(int k = 0; k < b->n_people; k++){
        const MbPerson *p = &b->people[k];
        int wh, dd;
        if(p->kind != kind || !present(w, p)) continue;
        uint32_t s = to_person(b, w, p, &wh, &dd);
        if(s < near){ near = s; *what = wh; *dir = dd; }
    }
    return near;
}

/* a healing item (not a revive): its HP for a creature of max HP `max`, or 0 */
static uint32_t heals(const MgItem *it, uint32_t max){
    if(it->effects & MG_I_REVIVE) return 0;
    if(it->effects & MG_I_HEAL) return it->amount;
    if(it->effects & MG_I_HEAL_PCT) return max * it->amount / 100;
    return 0;
}

static uint32_t top_hp(const MpWorld *w){
    uint32_t m = 0;
    for(int i = 0; i < w->n_party; i++) if(w->party[i].stat[MG_HP] > m) m = w->party[i].stat[MG_HP];
    return m;
}

int mb_shop(const MbBot *b, MpWorld *w){
    uint32_t have = 0, max = top_hp(w), best = 0;
    int pick = -1;
    for(uint32_t i = 0; i < b->g->n_items && i < MP_BAG; i++){
        MgItem it;
        if(mg_item(b->g, i, &it) || !heals(&it, max)) continue;
        have += w->bag[i];
        if(it.price && it.price * 2 <= w->money && w->bag[i] < MP_BAG_MAX && heals(&it, max) > best){
            best = heals(&it, max);
            pick = (int)i;
        }
    }
    return have < MB_HEALS ? pick : -1;
}

/* the healing item that suits party member who best: the smallest that makes
 * up two thirds of what it's lost, else the biggest; -1 if none would help */
static int heal_for(MbBot *b, MpWorld *w, int who){
    const MpCreature *c = &w->party[who];
    uint32_t lost = (uint32_t)c->stat[MG_HP] - c->hp, best_fit = 0xFFFFFFFFu, biggest = 0;
    int fit = -1, big = -1;
    for(uint32_t i = 0; i < b->g->n_items && i < MP_BAG; i++){
        MgItem it;
        uint32_t g;
        if(!w->bag[i] || mg_item(b->g, i, &it) || !heals(&it, c->stat[MG_HP]) || !mp_item_helps(w, (uint16_t)i, who, &g))
            continue;
        if(g * 3 >= lost * 2 && heals(&it, c->stat[MG_HP]) < best_fit){ best_fit = heals(&it, c->stat[MG_HP]); fit = (int)i; }
        if(g > biggest){ biggest = g; big = (int)i; }
    }
    return fit >= 0 ? fit : big;
}

/* every keeper beaten, or gone for good (a rematch's first match, say) */
int mb_done(const MbBot *b, const MpWorld *w){
    for(int k = 0; k < b->n_people; k++){
        const MbPerson *p = &b->people[k];
        if(p->kind == MG_P_TRAINER && !mp_flag(w, p->flag) && !(p->hide_if && mp_flag(w, p->hide_if))) return 0;
    }
    return 1;
}

int mb_world(MbBot *b, MpWorld *w, int *dir){
    *dir = 0;
    int r = grid(b, w);
    if(r < 0) return r;
    if(r > 0){
        snprintf(b->why, sizeof b->why, "lost: the area is too big to search");
        *dir = (int)(mp_rand(&b->rng) & 3);
        return MB_STUCK;
    }
    if(mb_done(b, w)){ snprintf(b->why, sizeof b->why, "every keeper beaten"); return MB_DONE; }
    mark(b, w);
    search(b, w, 0);
    int what = MB_STEP, d = 0;
    /* the keeper it's after: the one it chose last while they're still
     * there to beat and in reach (so it doesn't turn back at an area's edge
     * because another is nearer there), else the nearest not yet beaten */
    uint32_t best = FAR;
    int top = 0, goal = -1;
    for(int pass = 0; pass < 2 && goal < 0; pass++)
        for(int k = 0; k < b->n_people; k++){
            const MbPerson *p = &b->people[k];
            int wh, dd;
            if(p->kind != MG_P_TRAINER || mp_flag(w, p->flag) || !present(w, p)) continue;
            if(pass == 0 && k + 1 != b->goal) continue;
            uint32_t s = to_person(b, w, p, &wh, &dd);
            if(s < best){ best = s; what = wh; d = dd; top = p->top + p->margin; goal = k; }
        }
    b->goal = (uint8_t)(goal + 1);
    int train = best == FAR || best_level(w) < top;
    int tired = worn(w) || (best != FAR && !train && health(w) < 75);
    /* shopping: when it has the money for what it wants, and isn't worn down.
     * A shop it can't reach isn't tried again until the story moves on. */
    uint32_t story = 1;
    for(uint32_t i = 0; i < sizeof w->flags; i++) story = story * 31u + w->flags[i];
    if(!worn(w) && b->no_shop != story && mb_shop(b, w) >= 0){
        int wh = MB_STEP, dd = 0;
        if(to_nearest(b, w, MG_P_SHOP, &wh, &dd) != FAR){
            snprintf(b->why, sizeof b->why, "going shopping");
            *dir = dd;
            return wh;
        }
        b->no_shop = story;
    }
    /* resting keeps out of keepers' sight if that isn't further: a healer
     * past a keeper's line in this area beats one two areas back, or it
     * would turn round at the area's edge and come back for ever */
    if(tired){
        int wh1 = MB_STEP, d1 = 0, wh0 = MB_STEP, d0 = 0;
        search(b, w, 1);
        uint32_t s1 = to_nearest(b, w, MG_P_HEALER, &wh1, &d1);
        search(b, w, 0);
        uint32_t s0 = to_nearest(b, w, MG_P_HEALER, &wh0, &d0);
        if(s1 != FAR || s0 != FAR){
            int seen = s1 == FAR || (s0 != FAR && s0 / 4096 < s1 / 4096);
            snprintf(b->why, sizeof b->why, "going to rest");
            *dir = seen ? d0 : d1;
            return seen ? wh0 : wh1;
        }
    }
    /* training keeps out of keepers' sight if there's a way */
    for(int unseen = 1; train && unseen >= 0; unseen--){
        search(b, w, unseen);
        if(grind(b, w, dir)){
            if(best == FAR) snprintf(b->why, sizeof b->why, "training: no keeper in reach");
            else snprintf(b->why, sizeof b->why, "training for a level %d keeper", top);
            return MB_STEP;
        }
    }
    if(best != FAR){
        snprintf(b->why, sizeof b->why, "on to a keeper");
        *dir = d;
        return what;
    }
    snprintf(b->why, sizeof b->why, "stuck");
    *dir = (int)(mp_rand(&b->rng) & 3);
    return MB_STUCK;
}

/* ---- in a battle ----------------------------------------------------------- */

static int owned(const MpWorld *w, uint16_t mon){
    for(int i = 0; i < w->n_party; i++) if(w->party[i].mon == mon) return 1;
    for(int i = 0; i < w->n_box; i++) if(w->box[i].mon == mon) return 1;
    return 0;
}

/* what slot s is worth now: expected damage (or healing when low), times accuracy */
static uint32_t worth(MbBot *b, const MpCreature *me, const MpCreature *foe, int s, const uint8_t *rech){
    MgMove m;
    if(me->moves[s] == MP_NO_MOVE || rech[s] || mg_move(b->g, me->moves[s], &m)) return 0;
    uint32_t v = 0;
    for(int e = 0; e < 3; e++){
        if(m.eff_kind[e] == MG_E_DAMAGE && m.range != MG_SPECIAL){
            uint32_t d = mp_damage(b->g, me, &m, foe, NULL);
            v += d < foe->hp ? d : foe->hp;
        }
        else if(m.eff_kind[e] == MG_E_HEALING && m.eff_target[e] == MG_T_OWN && me->hp * 2u < me->stat[MG_HP]){
            uint32_t h = 7 + (uint32_t)me->level * m.heal / 100, room = (uint32_t)me->stat[MG_HP] - me->hp;
            v += h < room ? h : room;
        }
        else if(m.eff_kind[e] == MG_E_GIVE && m.eff_target[e] == MG_T_ENEMY && !foe->status) v += 1;
    }
    return v * m.accuracy + 1;              /* + 1: any move beats none */
}

static int best_move(MbBot *b, const MpCreature *me, const MpCreature *foe, const uint8_t *rech, uint32_t *score){
    int best = -1;
    uint32_t top = 0;
    for(int s = 0; s < MP_MOVES; s++){
        uint32_t v = worth(b, me, foe, s, rech);
        if(v > top){ top = v; best = s; }
    }
    if(score) *score = top;
    return best;
}

/* the strongest creature that can still fight, or -1 */
static int strongest(const MpWorld *w, int but){
    int best = -1;
    for(int i = 0; i < w->n_party; i++){
        const MpCreature *c = &w->party[i];
        if(i == but || !c->hp) continue;
        if(best < 0 || c->level > w->party[best].level ||
           (c->level == w->party[best].level && c->hp > w->party[best].hp)) best = i;
    }
    return best;
}

void mb_events(MbBot *b, const MpBattle *bt, const MpLog *log){
    for(int i = 0; log && i < log->n; i++)
        if(log->ev[i].kind == MP_EV_LOSE && b->fighting && b->people[b->fighting - 1].margin < 6)
            b->people[b->fighting - 1].margin++;
    if(bt->over) b->fighting = 0;
}

void mb_battle(MbBot *b, MpWorld *w, MpBattle *bt, int *act, int *arg){
    b->fighting = 0;
    for(int k = 0; bt->trainer && k < b->n_people; k++)
        if(b->people[k].area == w->area.index && b->people[k].index == bt->trainer - 1) b->fighting = (uint8_t)(k + 1);
    uint32_t sig = bt->wild.mon | (uint32_t)bt->wild.level << 16 | (uint32_t)bt->wild.iv[0] << 24;
    if(sig != b->foe_mon){ b->foe_mon = sig; b->tries = 0; }
    *act = MP_ACT_MOVE; *arg = -1;
    if(bt->need_swap){
        int s = strongest(w, bt->active);
        *act = MP_ACT_SWAP; *arg = s < 0 ? 0 : s;
        return;
    }
    MpCreature *me = &w->party[bt->active];
    uint32_t room = b->g->rules[7] < MP_PARTY ? b->g->rules[7] : MP_PARTY;
    if(!bt->trainer){
        if(worn(w)){ *act = MP_ACT_RUN; *arg = 0; return; }
        uint32_t sc;
        int s = best_move(b, me, &bt->wild, bt->recharge[0], &sc);
        uint32_t dmg = s >= 0 ? sc / 100 : 0;
        if(w->n_party < room && !owned(w, bt->wild.mon) && b->tries < 3 &&
           (bt->wild.hp * 2u <= bt->wild.stat[MG_HP] || dmg >= bt->wild.hp)){
            b->tries++;
            *act = MP_ACT_CATCH; *arg = 0;
            return;
        }
    }
    if(bt->trainer && me->hp * 3u < me->stat[MG_HP]){
        /* low in a keeper's battle: heal, unless the next blow wins */
        uint32_t sc;
        int mv = best_move(b, me, &bt->wild, bt->recharge[0], &sc), it = heal_for(b, w, bt->active);
        if(it >= 0 && !(mv >= 0 && sc / 100 >= bt->wild.hp)){
            *act = MP_ACT_ITEM; *arg = it | bt->active << 8;
            return;
        }
    }
    int s = strongest(w, -1);
    if(s >= 0 && s != bt->active && w->party[s].level >= me->level + 2){
        *act = MP_ACT_SWAP; *arg = s;
        return;
    }
    *arg = best_move(b, me, &bt->wild, bt->recharge[0], NULL);
}

/* a move's worth with no foe in mind */
static uint32_t plain_worth(MbBot *b, uint16_t mv){
    MgMove m;
    if(mv == MP_NO_MOVE || mg_move(b->g, mv, &m)) return 0;
    uint32_t v = 0;
    for(int e = 0; e < 3; e++){
        if(m.eff_kind[e] == MG_E_DAMAGE && m.range != MG_SPECIAL) v += m.power;
        else if(m.eff_kind[e] == MG_E_HEALING) v += 60;
        else if(m.eff_kind[e] == MG_E_GIVE) v += 10;
    }
    return v * m.accuracy;
}

int mb_learn(MbBot *b, MpWorld *w, int who, uint16_t move){
    if(who < 0 || who >= w->n_party) return -1;
    MpCreature *c = &w->party[who];
    int worst = -1;
    uint32_t low = 0;
    for(int s = 0; s < MP_MOVES; s++){
        uint32_t v = plain_worth(b, c->moves[s]);
        if(worst < 0 || v < low){ worst = s; low = v; }
    }
    return plain_worth(b, move) > low ? worst : -1;
}
