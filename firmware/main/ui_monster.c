/* ui_monster.c -- Critters, the monster game's screens (docs/GAME_PLAN.md §7).
 *
 * The rules are monster_play.c's and the file is monster.c's; this file only
 * turns their events into pictures and words, and the buttons into calls.
 *
 * THE VIEW (240 x 176) has no canvas. Its draw event composes the part of the
 * view LVGL is drawing (at most one draw-buffer band, 40 rows) into a band
 * buffer, from a cache of 8-bit tiles and the area's palette, and hands that
 * to LVGL as ONE RGB565 image. So a frame costs one draw task per band, not
 * one per tile, and the heap holds 64 tiles at a byte a pixel (16 KB) instead
 * of a 42-84 KB canvas. A battle draws the two creatures the same way, from
 * the tile cache's memory (the map isn't drawn then). The people standing in
 * an area are drawn from the same cache: each frame is two cache entries, its
 * top and bottom halves, read from the card when first drawn.
 *
 * THE CONTROLS (240 x 120) are one object that draws its own D-pad and A and
 * B buttons. A press acts at once; holding a direction keeps walking. The
 * panel registers one touch at a time, so there are no combinations.
 *
 * MEMORY: one block, allocated when the game opens and freed when it closes
 * (about 60 KB, plus the largest area head in the file). If it can't be had,
 * for example during a sync, the game says so and doesn't open.
 *
 * Saving: when leaving (the lock included) and from the menu. A battle in
 * progress goes to pause.dat as well and comes back on the next open, so the
 * lock pauses the game rather than ending the battle (§13 Q3).
 *
 * AUTO-PLAY (§8): monster_bot.c decides, and this file presses the same
 * buttons a person would, at a pace one can watch: about four steps a
 * second, a message read in about a second, the menu cursor moving a line
 * at a time to what it chose. Touching the game turns it off. The bot's
 * memory (about 3 KB, and 6 B a cell of the area it's in) is allocated when
 * it's turned on and freed when it's turned off or the game closes. */
#include "ui_monster.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if !(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3)
#include "src/misc/cache/instance/lv_image_cache.h"   /* lvgl.h no longer exports it */
#endif
#include "lv_font_palm.h"
#include "monster.h"
#include "monster_bot.h"
#include "monster_play.h"
#include "power.h"

#define MON_FILE  MON_DIR "/game.bin"
#define MON_SAVE  MON_DIR "/save.dat"
#define MON_PAUSE MON_DIR "/pause.dat"

#define VW         240
#define VH         176
#define PAD_H      120
#define BAND_ROWS  40               /* the display's draw buffer: lvgl_port.c BUF_ROWS */
#define NCACHE     96               /* tiles kept: Kestrel shows 60, and the people take two each */
#define TICK_MS    30
#define STEP_TICKS 4                /* a step: four frames of 4 px */
#define TURN_HOLD  5                /* ticks a turn waits before a held press walks */
#define LINE_H     14               /* lv_font_palm's line height */
#define MENU_MAX   8
#define NAME_LEN   24
#define AUTO_STEP  4                /* Auto-play's pauses, in ticks: after a step (which takes four more), */
#define AUTO_TEXT  30               /* on a message, */
#define AUTO_MENU  10               /* and per line the menu cursor moves */

enum { SC_NONE, SC_ERROR, SC_PICK, SC_MAP, SC_BATTLE, SC_CARD };
enum { UI_NONE, UI_TEXT, UI_MENU, UI_CREDITS };
enum { MN_BATTLE, MN_MOVES, MN_SWAP, MN_PARTY, MN_FORGET, MN_NEW };
enum { BT_NONE = -1, BT_N = 0, BT_E, BT_S, BT_W, BT_A, BT_B };
/* after the transition card: */
enum { CARD_MAP, CARD_BATTLE };

typedef struct {
    MonGame  g;
    MpWorld  w;
    MpBattle b;
    MpLog    log;                   /* the script being shown */
    uint8_t  pos;                   /* its next event */

    uint8_t  scene, ui, menu, sel, nmenu, menu_back;
    uint8_t  started;               /* a game is under way (made or loaded) */
    uint8_t  card_next, card_ticks;
    char     err[96];

    /* walking */
    int8_t   pending, held, pressed;
    uint8_t  turn_wait, anim, anim_len, parity, walk_col;
    int16_t  from_x, from_y;        /* the step's start, in cells */
    uint8_t  battle_after;          /* a wild creature met on this step */

    /* the look, then the starter */
    uint16_t pick[3];
    uint8_t  npick, pick_note;      /* a note is showing before the choice */
    uint8_t  picking_look, look;    /* choosing how the player looks first (a file with more than one) */

    /* what a battle shows while the script plays (the rules have moved on) */
    uint16_t sh_hp[MP_PARTY], sh_mon[MP_PARTY], sh_wild_hp;
    uint8_t  sh_active, sh_wild_gone, sh_me_gone, shakes;
    uint16_t spr_mon[2];            /* the sprites loaded: 0 the player's, 1 the wild one */
    uint8_t  learn_pending, last_move, to_moves;    /* to_moves: a note, then the moves again */
    uint8_t  learn_who;
    uint16_t learn_move;
    uint8_t  to_menu;               /* a note, then the battle menu again */
    uint16_t sh_wild_mon, sh_wild_max;
    uint8_t  sh_wild_lv;
    char     keeper[NAME_LEN];      /* the trainer in this battle */

    /* the area's pictures */
    uint16_t lut[256];
    uint16_t tile_id[NCACHE];
    uint32_t tile_age[NCACHE], clock;
    uint8_t  tiles[NCACHE][MG_TILE * MG_TILE];       /* or the two battle sprites */
    uint8_t  frames[MG_CHAR_FRAMES][MG_CHAR_W * MG_CHAR_H];
    uint8_t  have_frames;
    uint16_t band[VW * BAND_ROWS] __attribute__((aligned(64)));   /* LVGL wants a draw buffer aligned */
    lv_image_dsc_t img;

    /* a menu's lines, and the text the box shows them as */
    char     lines[MENU_MAX][48];
    char     menu_txt[MENU_MAX * 48];

    /* the objects (children of the content area) */
    lv_obj_t *view, *pad, *box, *text, *cursor, *card, *foe, *me, *mehp, *credits;
    lv_timer_t *timer;
    char     *cred;

    /* Auto-play: the bot, and the battle action it's pressing its way to */
    MbBot   *bot;
    uint8_t  autoplay, bot_wait;
    int8_t   bot_act, bot_arg;      /* bot_act -1: none chosen yet this turn */

    uint32_t head_cap;
    uint8_t  head[];                /* the largest area head in the file */
} Mon;

static Mon *g_mon;

#define SPR(m, i) ((m)->tiles[0] + (i) * 4144)      /* two sprite records in the tile memory */

/* ---- small helpers ---- */

static int imin(int a, int b){ return a < b ? a : b; }
static int imax(int a, int b){ return a > b ? a : b; }
static int fdiv16(int v){ return v >= 0 ? v / 16 : -((15 - v) / 16); }

typedef struct { int x1, y1, x2, y2; } Rect;       /* inclusive, view coordinates */

/* a string into buf: 0, or 1 if it can't be read (mg_text returns a length) */
static int mtext(MonGame *g, MgStr s, char *buf, uint32_t cap){ return mg_text(g, s, buf, cap) < 0; }

static const char *mon_name(Mon *m, uint16_t mon, char *buf){
    MgMon r;
    if(mg_mon(&m->g, mon, &r) || mtext(&m->g, r.name, buf, NAME_LEN)) snprintf(buf, NAME_LEN, "?");
    return buf;
}
static const char *move_name(Mon *m, uint16_t mv, char *buf){
    MgMove r;
    if(mv == MP_NO_MOVE || mg_move(&m->g, mv, &r) || mtext(&m->g, r.name, buf, NAME_LEN)) snprintf(buf, NAME_LEN, "?");
    return buf;
}
static const char *status_name(Mon *m, uint16_t st, char *buf){
    if(st >= m->g.n_statuses || mtext(&m->g, m->g.statuses[st].name, buf, NAME_LEN)) snprintf(buf, NAME_LEN, "?");
    return buf;
}

/* ---- the tile cache and the area's pictures ---- */

static void unpack(const uint8_t *src, uint8_t *dst, int n, int bpp){
    if(bpp == 8){ memcpy(dst, src, (size_t)n); return; }
    for(int i = 0; i < n; i += 2){ dst[i] = src[i / 2] >> 4; dst[i + 1] = src[i / 2] & 15; }
}

static void cache_clear(Mon *m){
    for(int i = 0; i < NCACHE; i++){ m->tile_id[i] = 0xFFFF; m->tile_age[i] = 0; }
    m->spr_mon[0] = m->spr_mon[1] = MG_NONE16;
}

#define HALF_KEY 0x8000          /* a cache key past every tile: half of a character frame */

/* tile t's pixels, or NULL for none (tile 0, or one that can't be read). A key
 * HALF_KEY + 2 k + h is the top (h 0) or bottom half of character frame k. */
static const uint8_t *tile(Mon *m, uint16_t t){
    if(!t) return NULL;
    int old = 0;
    m->clock++;
    for(int i = 0; i < NCACHE; i++){
        if(m->tile_id[i] == t){ m->tile_age[i] = m->clock; return m->tiles[i]; }
        if(m->tile_age[i] < m->tile_age[old]) old = i;
    }
    m->tile_id[old] = 0xFFFF;
    if(t >= HALF_KEY){
        uint8_t raw[MG_CHAR_W * MG_CHAR_H];
        uint32_t k = (uint32_t)(t - HALF_KEY) / 2, h = (uint32_t)(t - HALF_KEY) % 2, bytes = MG_TILE * MG_TILE * m->w.area.bpp / 8;
        if(mg_frame(&m->g, &m->w.area, k, raw, sizeof raw)) return NULL;
        unpack(raw + h * bytes, m->tiles[old], MG_TILE * MG_TILE, m->w.area.bpp);
    }
    else{
        uint8_t raw[MG_TILE * MG_TILE];
        if(mg_tile(&m->g, &m->w.area, t, raw, sizeof raw)) return NULL;
        unpack(raw, m->tiles[old], MG_TILE * MG_TILE, m->w.area.bpp);
    }
    m->tile_id[old] = t;
    m->tile_age[old] = m->clock;
    return m->tiles[old];
}

/* the area just entered: its palette, the player's frames (their look's), a fresh cache */
static void area_pictures(Mon *m){
    for(int i = 0; i < 256; i++) m->lut[i] = mg_palette(&m->w.area, (uint32_t)i);
    cache_clear(m);
    m->have_frames = 0;
    if(m->w.area.n_chars > m->w.look){
        uint8_t raw[MG_CHAR_W * MG_CHAR_H];
        int ok = 1;
        for(int k = 0; ok && k < MG_CHAR_FRAMES; k++){
            ok = !mg_frame(&m->g, &m->w.area, (uint32_t)(m->w.look * MG_CHAR_FRAMES + k), raw, sizeof raw);
            if(ok) unpack(raw, m->frames[k], MG_CHAR_W * MG_CHAR_H, m->w.area.bpp);
        }
        m->have_frames = (uint8_t)ok;
    }
}

/* a battle sprite into slot s (0 the player's, 1 the wild one's) */
static void sprite_load(Mon *m, int s, uint16_t mon){
    if(m->spr_mon[s] == mon) return;
    MgMon r;
    m->spr_mon[s] = MG_NONE16;
    if(mg_mon(&m->g, mon, &r) || mg_sprite(&m->g, &r, SPR(m, s), MG_SPRITE_MAX) < 0) return;
    m->spr_mon[s] = mon;
}

/* ---- composing the view ---- */

static void fill(uint16_t *band, int bw, const Rect *c, int x, int y, int w, int h, uint16_t col){
    int xa = imax(x, c->x1), xb = imin(x + w - 1, c->x2), ya = imax(y, c->y1), yb = imin(y + h - 1, c->y2);
    for(int j = ya; j <= yb; j++){
        uint16_t *d = band + (j - c->y1) * bw + (xa - c->x1);
        for(int i = xa; i <= xb; i++) *d++ = col;
    }
}

/* an 8-bit picture at (x, y); index 0 skipped when `see` (transparent) */
static void blit8(uint16_t *band, int bw, const Rect *c, int x, int y, int w, int h,
                  const uint8_t *px, const uint16_t *lut, int see){
    int xa = imax(x, c->x1), xb = imin(x + w - 1, c->x2), ya = imax(y, c->y1), yb = imin(y + h - 1, c->y2);
    for(int j = ya; j <= yb; j++){
        const uint8_t *s = px + (j - y) * w + (xa - x);
        uint16_t *d = band + (j - c->y1) * bw + (xa - c->x1);
        for(int i = xa; i <= xb; i++, s++, d++) if(!see || *s) *d = lut[*s];
    }
}

/* frame f of a sprite record, 4-bit, scaled by k, transparent at 0 */
static void blit_sprite(uint16_t *band, int bw, const Rect *c, int x, int y, const uint8_t *rec, int f, int k){
    int w = rec[0], h = rec[1];
    uint16_t pal[16];
    for(int i = 0; i < 16; i++) pal[i] = (uint16_t)(rec[8 + 2 * i] | rec[9 + 2 * i] << 8);
    const uint8_t *px = rec + 40 + f * (w * h / 2);
    int xa = imax(x, c->x1), xb = imin(x + w * k - 1, c->x2), ya = imax(y, c->y1), yb = imin(y + h * k - 1, c->y2);
    for(int j = ya; j <= yb; j++){
        const uint8_t *row = px + ((j - y) / k) * (w / 2);
        uint16_t *d = band + (j - c->y1) * bw + (xa - c->x1);
        for(int i = xa; i <= xb; i++, d++){
            int u = (i - x) / k;
            int v = u & 1 ? row[u / 2] & 15 : row[u / 2] >> 4;
            if(v) *d = pal[v];
        }
    }
}

static const int8_t HOP_LIFT[8] = { 0, -3, -5, -6, -6, -5, -3, 0 };
static const int8_t DXS[4] = { 0, 1, 0, -1 }, DYS[4] = { -1, 0, 1, 0 };

/* where the player is drawn, in world pixels, and the camera */
static void player_px(Mon *m, int *px, int *py, int *lift){
    int tx = m->w.x * MG_TILE, ty = m->w.y * MG_TILE;
    *lift = 0;
    if(m->anim){
        int fx = m->from_x * MG_TILE, fy = m->from_y * MG_TILE, done = m->anim_len - m->anim;
        tx = fx + (tx - fx) * done / m->anim_len;
        ty = fy + (ty - fy) * done / m->anim_len;
        if(m->anim_len == 2 * STEP_TICKS) *lift = HOP_LIFT[done & 7];
    }
    *px = tx; *py = ty;
}
static void camera(Mon *m, int px, int py, int *cx, int *cy){
    int mw = m->w.area.w * MG_TILE, mh = m->w.area.h * MG_TILE;
    int x = px + MG_TILE / 2 - VW / 2, y = py + MG_TILE / 2 - VH / 2;
    if(mw <= VW) x = (mw - VW) / 2; else x = imax(0, imin(x, mw - VW));
    if(mh <= VH) y = (mh - VH) / 2; else y = imax(0, imin(y, mh - VH));
    *cx = x; *cy = y;
}

/* the rows of a character sheet by facing (N E S W): back, right, front, left */
static const uint8_t ROW[4] = { 3, 2, 0, 1 };

/* a standing person, frame f of the area's character frames, its feet in cell (x, y) */
static void figure(Mon *m, const Rect *c, int bw, int f, int sx, int sy){
    for(int h = 0; h < 2; h++){
        const uint8_t *p = tile(m, (uint16_t)(HALF_KEY + 2 * f + h));
        if(p) blit8(m->band, bw, c, sx, sy + h * MG_TILE, MG_TILE, MG_TILE, p, m->lut, 1);
    }
}

static void compose_map(Mon *m, const Rect *c, int bw){
    int px, py, lift, cx, cy;
    player_px(m, &px, &py, &lift);
    camera(m, px, py, &cx, &cy);
    int x0 = fdiv16(cx + c->x1), x1 = fdiv16(cx + c->x2), y0 = fdiv16(cy + c->y1), y1 = fdiv16(cy + c->y2);
    for(int y = y0; y <= y1; y++)
        for(int x = x0; x <= x1; x++){
            uint16_t t;
            uint8_t mv;
            const uint8_t *p = NULL;
            if(!mp_cell(&m->w, x, y, &t, &mv)) p = tile(m, t);
            int sx = x * MG_TILE - cx, sy = y * MG_TILE - cy;
            if(p) blit8(m->band, bw, c, sx, sy, MG_TILE, MG_TILE, p, m->lut, 0);
            else fill(m->band, bw, c, sx, sy, MG_TILE, MG_TILE, 0);
        }
    /* the people and the player, the nearer (lower) drawn last; columns step, stand, step */
    int sx = px - cx, sy = py - cy - (MG_CHAR_H - MG_TILE) + lift, drawn = 0;
    for(int pass = 0; pass < 2; pass++){
        for(uint32_t k = 0; k < m->w.area.n_people; k++){
            MgPerson q;
            mg_person(&m->w.area, k, &q);
            int qy = q.y * MG_TILE;
            if((pass == 0) != (qy <= py) || !mp_present(&m->w, &q)) continue;
            if(q.x < x0 - 1 || q.x > x1 + 1 || q.y < y0 || q.y > y1 + 2) continue;
            figure(m, c, bw, q.sheet * MG_CHAR_FRAMES + ROW[m->w.pface[k] & 3] * 3 + 1,
                   q.x * MG_TILE - cx, qy - cy - (MG_CHAR_H - MG_TILE));
        }
        if(drawn) continue;
        drawn = 1;
        if(m->have_frames){
            int f = ROW[m->w.facing & 3] * 3 + (m->anim ? m->walk_col : 1);
            blit8(m->band, bw, c, sx, sy, MG_CHAR_W, MG_CHAR_H, m->frames[f], m->lut, 1);
        }
        else fill(m->band, bw, c, sx + 3, sy + 18, 10, 12, 0xF800);
    }
    /* what's drawn over the player: tree tops, roof edges */
    for(uint32_t i = 0; i < m->w.area.n_overlays; i++){
        uint16_t cell, t;
        mg_overlay(&m->w.area, i, &cell, &t);
        int x = cell % m->w.area.w, y = cell / m->w.area.w;
        if(x < x0 || x > x1 || y < y0 || y > y1) continue;
        const uint8_t *p = tile(m, t);
        if(p) blit8(m->band, bw, c, x * MG_TILE - cx, y * MG_TILE - cy, MG_TILE, MG_TILE, p, m->lut, 1);
    }
}

static uint16_t rgb(uint32_t hex){
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 | ((hex >> 10) & 0x3F) << 5 | ((hex >> 3) & 0x1F));
}

static void hp_bar(Mon *m, const Rect *c, int bw, int x, int y, int w, uint32_t hp, uint32_t max){
    fill(m->band, bw, c, x - 1, y - 1, w + 2, 7, rgb(0x303030));
    fill(m->band, bw, c, x, y, w, 5, rgb(0xE8E8E8));
    if(!max) return;
    int f = (int)((uint64_t)w * hp / max);
    if(hp && !f) f = 1;
    uint32_t col = hp * 5 > max * 2 ? 0x30B030 : hp * 5 > max ? 0xE0B020 : 0xD03020;
    fill(m->band, bw, c, x, y, f, 5, rgb(col));
}

/* A creature's card: its name, level and HP bar in a framed box, with a
 * pointer from the box's side (dir +1 right, -1 left) to its picture, so
 * which bar belongs to which creature is plain at a glance. */
static void card(Mon *m, const Rect *c, int bw, int x, int y, int w, int h, int dir){
    uint16_t edge = rgb(0x303030), face = rgb(0xFFFFFF);
    fill(m->band, bw, c, x, y, w, h, edge);
    fill(m->band, bw, c, x + 2, y + 2, w - 4, h - 4, face);
    int tip = 9, cy = y + h / 2, bx = dir > 0 ? x + w - 2 : x + 1;
    for(int i = 0; i <= tip; i++){                  /* the pointer: rows narrowing to a tip */
        int half = tip - i, px = bx + dir * i;
        fill(m->band, bw, c, px, cy - half - 1, 1, 2 * half + 3, edge);
        if(half) fill(m->band, bw, c, px, cy - half + 1, 1, 2 * half - 1, face);
    }
}

#define FOE_SPR_X  166
#define FOE_SPR_Y  0
#define ME_SPR_X   14
#define ME_SPR_Y   40
#define FOE_CARD_X 6
#define FOE_CARD_Y 8
#define ME_CARD_X  92
#define ME_CARD_Y  57
#define CARD_W     140

static void compose_battle(Mon *m, const Rect *c, int bw){
    fill(m->band, bw, c, 0, 0, VW, VH, rgb(0xF4F1E4));
    /* laid out above a four-line menu (the box's top is at y 104): the other
     * side's creature top right with its card to its left, the player's
     * bottom left with its card to its right */
    fill(m->band, bw, c, FOE_SPR_X - 8, 54, 80, 8, rgb(0xC9D8A8));     /* the ground they stand on */
    fill(m->band, bw, c, ME_SPR_X - 6, 94, 80, 8, rgb(0xC9D8A8));
    card(m, c, bw, FOE_CARD_X, FOE_CARD_Y, CARD_W, 36, +1);
    card(m, c, bw, ME_CARD_X, ME_CARD_Y, CARD_W, 46, -1);
    if(!m->sh_wild_gone && m->spr_mon[1] != MG_NONE16) blit_sprite(m->band, bw, c, FOE_SPR_X, FOE_SPR_Y, SPR(m, 1), 0, 1);
    if(!m->sh_me_gone && m->spr_mon[0] != MG_NONE16) blit_sprite(m->band, bw, c, ME_SPR_X, ME_SPR_Y, SPR(m, 0), 1, 1);
    MpCreature *me = &m->w.party[m->sh_active];
    hp_bar(m, c, bw, FOE_CARD_X + 7, FOE_CARD_Y + 22, CARD_W - 14, m->sh_wild_hp, m->sh_wild_max);
    hp_bar(m, c, bw, ME_CARD_X + 7, ME_CARD_Y + 22, CARD_W - 14, m->sh_hp[m->sh_active], me->stat[MG_HP]);
}

static void compose_pick(Mon *m, const Rect *c, int bw){
    fill(m->band, bw, c, 0, 0, VW, VH, rgb(0xF4F1E4));
    if(m->picking_look){
        /* the look, standing, at 3x: its two halves from the start area's sheets */
        const int k = 3, x0 = (VW - MG_CHAR_W * k) / 2, y0 = 4;
        for(int h = 0; h < 2; h++){
            const uint8_t *p = tile(m, (uint16_t)(HALF_KEY + 2 * (m->look * MG_CHAR_FRAMES + 1) + h));
            if(!p) continue;
            for(int j = imax(c->y1, y0 + h * MG_TILE * k); j <= imin(c->y2, y0 + (h + 1) * MG_TILE * k - 1); j++)
                for(int i = imax(c->x1, x0); i <= imin(c->x2, x0 + MG_TILE * k - 1); i++){
                    uint8_t v = p[((j - y0) / k - h * MG_TILE) * MG_TILE + (i - x0) / k];
                    if(v) m->band[(j - c->y1) * bw + (i - c->x1)] = m->lut[v];
                }
        }
        return;
    }
    if(m->spr_mon[1] != MG_NONE16) blit_sprite(m->band, bw, c, (VW - 128) / 2, 0, SPR(m, 1), 0, 2);
}

static void view_draw_cb(lv_event_t *e){
    Mon *m = g_mon;
    if(!m) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t va;
    lv_obj_get_coords(obj, &va);
    lv_area_t a = layer->_clip_area;
    if(a.x1 < va.x1) a.x1 = va.x1;
    if(a.y1 < va.y1) a.y1 = va.y1;
    if(a.x2 > va.x2) a.x2 = va.x2;
    if(a.y2 > va.y2) a.y2 = va.y2;
    if(a.x1 > a.x2 || a.y1 > a.y2) return;
    /* a band taller than the draw buffer can't happen on the device; drawn
     * a band at a time it would need a buffer per band, so it's cut */
    if(a.y2 - a.y1 + 1 > BAND_ROWS) a.y2 = a.y1 + BAND_ROWS - 1;
    Rect c = { a.x1 - va.x1, a.y1 - va.y1, a.x2 - va.x1, a.y2 - va.y1 };
    int bw = c.x2 - c.x1 + 1, bh = c.y2 - c.y1 + 1;
    switch(m->scene){
    case SC_MAP:    compose_map(m, &c, bw); break;
    case SC_BATTLE: compose_battle(m, &c, bw); break;
    case SC_PICK:   compose_pick(m, &c, bw); break;
    default:        fill(m->band, bw, &c, 0, 0, VW, VH, 0); break;
    }
    lv_image_cache_drop(&m->img);
    memset(&m->img, 0, sizeof m->img);
    m->img.header.magic = LV_IMAGE_HEADER_MAGIC;
    m->img.header.cf = LV_COLOR_FORMAT_RGB565;
    m->img.header.w = (uint32_t)bw;
    m->img.header.h = (uint32_t)bh;
    m->img.header.stride = (uint32_t)bw * 2;
    m->img.data = (const uint8_t *)m->band;
    m->img.data_size = (uint32_t)(bw * bh * 2);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &m->img;
    lv_draw_image(layer, &d, &a);
}

static void redraw(void){ if(g_mon && g_mon->view) lv_obj_invalidate(g_mon->view); }

/* ---- the text box: a message, or a menu with a cursor ---- */

static void box_hide(Mon *m){
    lv_obj_add_flag(m->box, LV_OBJ_FLAG_HIDDEN);
    m->ui = UI_NONE;
}
static void box_say(Mon *m, const char *t){
    lv_label_set_long_mode(m->text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(m->text, t);
    lv_obj_add_flag(m->cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->box, LV_OBJ_FLAG_HIDDEN);
    m->ui = UI_TEXT;
}

static void menu_show(Mon *m, int kind, const char *const *lines, int n, int back){
    char *p = m->menu_txt;
    size_t room = sizeof m->menu_txt;
    p[0] = 0;
    for(int i = 0; i < n && i < MENU_MAX; i++){
        int k = snprintf(p, room, "%s%s", i ? "\n" : "", lines[i]);
        if(k < 0 || (size_t)k >= room) break;
        p += k; room -= (size_t)k;
    }
    lv_label_set_long_mode(m->text, LV_LABEL_LONG_CLIP);      /* a line per item, always */
    lv_label_set_text(m->text, m->menu_txt);
    m->menu = (uint8_t)kind;
    m->nmenu = (uint8_t)imin(n, MENU_MAX);
    if(m->sel >= m->nmenu) m->sel = 0;
    m->menu_back = (uint8_t)back;
    lv_obj_clear_flag(m->cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(m->cursor, m->sel * LINE_H);
    lv_obj_clear_flag(m->box, LV_OBJ_FLAG_HIDDEN);
    m->ui = UI_MENU;
}
static void menu_cursor(Mon *m, int d){
    if(!m->nmenu) return;
    m->sel = (uint8_t)((m->sel + m->nmenu + d) % m->nmenu);
    lv_obj_set_y(m->cursor, m->sel * LINE_H);
}

/* ---- battles: the labels, the script ---- */

static void battle_labels(Mon *m){
    char a[NAME_LEN], t[64];
    if(m->scene != SC_BATTLE){
        lv_obj_add_flag(m->foe, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m->me, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m->mehp, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    MpCreature *me = &m->w.party[m->sh_active];
    snprintf(t, sizeof t, "%s  Lv %u", mon_name(m, m->sh_wild_mon, a), m->sh_wild_lv);
    lv_label_set_text(m->foe, t);
    snprintf(t, sizeof t, "%s  Lv %u", mon_name(m, m->sh_mon[m->sh_active], a), me->level);
    lv_label_set_text(m->me, t);
    snprintf(t, sizeof t, "HP %u / %u", m->sh_hp[m->sh_active], me->stat[MG_HP]);
    lv_label_set_text(m->mehp, t);
    lv_obj_clear_flag(m->foe, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->me, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m->mehp, LV_OBJ_FLAG_HIDDEN);
}

/* what a battle shows = the rules' state (between scripts) */
static void shown_now(Mon *m){
    for(int i = 0; i < m->w.n_party; i++){ m->sh_hp[i] = m->w.party[i].hp; m->sh_mon[i] = m->w.party[i].mon; }
    m->sh_active = m->b.active;
    m->sh_wild_hp = m->b.wild.hp;
    m->sh_wild_mon = m->b.wild.mon;
    m->sh_wild_lv = m->b.wild.level;
    m->sh_wild_max = m->b.wild.stat[MG_HP];
    m->sh_me_gone = !m->w.party[m->b.active].hp;
    m->sh_wild_gone = !m->b.wild.hp;
}

static void battle_show(Mon *m){
    sprite_load(m, 0, m->sh_mon[m->sh_active]);
    sprite_load(m, 1, m->sh_wild_mon);
    battle_labels(m);
    redraw();
}

/* the keeper in this battle: their name, from the area's people */
static void keeper_name(Mon *m){
    MgPerson q;
    m->keeper[0] = 0;
    if(!m->b.trainer || m->b.trainer > m->w.area.n_people) return;
    mg_person(&m->w.area, m->b.trainer - 1u, &q);
    if(mtext(&m->g, q.name, m->keeper, sizeof m->keeper)) m->keeper[0] = 0;
}

/* person k's name */
static const char *person_name(Mon *m, uint32_t k, char *buf){
    MgPerson q;
    buf[0] = 0;
    if(k < m->w.area.n_people){
        mg_person(&m->w.area, k, &q);
        if(mtext(&m->g, q.name, buf, NAME_LEN)) buf[0] = 0;
    }
    return buf;
}

static void battle_menu(Mon *m){
    static const char *const L[] = { "Fight", "Swap", "Catch", "Run" };
    menu_show(m, MN_BATTLE, L, 4, 0);
}

static void party_menu(Mon *m, int kind, int back){
    char (*lines)[48] = m->lines;
    const char *ptr[MP_PARTY + 1];
    char a[NAME_LEN];
    int n = 0;
    for(int i = 0; i < m->w.n_party; i++, n++){
        MpCreature *c = &m->w.party[i];
        snprintf(lines[n], 48, "%s  Lv %u  HP %u/%u%s", mon_name(m, c->mon, a), c->level,
                 c->hp, c->stat[MG_HP], c->hp ? "" : "  fainted");
        ptr[n] = lines[n];
    }
    if(back){ snprintf(lines[n], sizeof lines[n], "Back"); ptr[n] = lines[n]; n++; }
    m->sel = 0;
    menu_show(m, kind, ptr, n, back);
}

static void moves_menu(Mon *m){
    char (*lines)[48] = m->lines;
    const char *ptr[MP_MOVES + 1];
    char a[NAME_LEN];
    MpCreature *c = &m->w.party[m->b.active];
    int n = 0;
    for(int s = 0; s < MP_MOVES; s++){
        if(c->moves[s] == MP_NO_MOVE) snprintf(lines[n], sizeof lines[n], "-");
        else if(m->b.recharge[0][s])
            snprintf(lines[n], 48, "%s  (rests %u)", move_name(m, c->moves[s], a), m->b.recharge[0][s]);
        else snprintf(lines[n], sizeof lines[n], "%s", move_name(m, c->moves[s], a));
        ptr[n] = lines[n]; n++;
    }
    snprintf(lines[n], 48, "Back"); ptr[n] = lines[n]; n++;
    /* the last move used, unless it's resting: then the first that can be used */
    m->sel = m->last_move < MP_MOVES ? m->last_move : 0;
    if(c->moves[m->sel] == MP_NO_MOVE || m->b.recharge[0][m->sel])
        for(int k = 0; k < MP_MOVES; k++)
            if(c->moves[k] != MP_NO_MOVE && !m->b.recharge[0][k]){ m->sel = (uint8_t)k; break; }
    menu_show(m, MN_MOVES, ptr, n, 1);
}

static void forget_menu(Mon *m){
    char (*lines)[48] = m->lines;
    const char *ptr[MP_MOVES + 1];
    char a[NAME_LEN];
    MpCreature *c = &m->w.party[m->learn_who];
    int n = 0;
    for(int s = 0; s < MP_MOVES; s++){
        snprintf(lines[n], 48, "Forget %s", move_name(m, c->moves[s], a));
        ptr[n] = lines[n]; n++;
    }
    snprintf(lines[n], 48, "Don't learn %s", move_name(m, m->learn_move, a));
    ptr[n] = lines[n]; n++;
    m->sel = 0;
    menu_show(m, MN_FORGET, ptr, n, 0);
}

/* One event of the script: change what's shown, and its words into t.
 * Returns 1 if there's something to say. */
static int ev_text(Mon *m, const MpEvent *e, char *t, size_t cap){
    char a[NAME_LEN], b[NAME_LEN];
    int s = e->side;
    char kp[NAME_LEN + 4];
    snprintf(kp, sizeof kp, "%s's ", m->keeper);
    const char *who = s ? (m->b.trainer && m->keeper[0] ? kp : "The wild ") : "";
    const char *nm = s ? mon_name(m, m->sh_wild_mon, a) : mon_name(m, m->sh_mon[m->sh_active], a);
    uint16_t *hp = s ? &m->sh_wild_hp : &m->sh_hp[m->sh_active];
    switch(e->kind){
    case MP_EV_ENCOUNTER:
        snprintf(t, cap, "A wild %s appears!", mon_name(m, e->a, a)); return 1;
    case MP_EV_SIGN: {
        MgStr st = { e->n, e->b };
        if(mtext(&m->g, st, t, (uint32_t)cap)) snprintf(t, cap, "The sign can't be read.");
        return 1;
    }
    case MP_EV_TALK: {
        MgStr st = { e->n, e->b };
        int k = snprintf(t, cap, "%s%s", person_name(m, e->a, a), a[0] ? ": " : "");
        if(k < 0 || (size_t)k >= cap) k = 0;
        if(!e->b || mtext(&m->g, st, t + k, (uint32_t)(cap - (size_t)k))) return 0;
        redraw();                                   /* they turned to face the player */
        return 1;
    }
    case MP_EV_SPOTTED:
        redraw();
        snprintf(t, cap, "%s spots you!", person_name(m, e->a, a)); return 1;
    case MP_EV_TRAINER:
        person_name(m, e->a, m->keeper);
        return 0;
    case MP_EV_SENDS:
        m->sh_wild_mon = e->a;
        m->sh_wild_lv = (uint8_t)e->b;
        m->sh_wild_hp = m->sh_wild_max = m->b.wild.mon == e->a ? m->b.wild.stat[MG_HP] : 1;
        if(m->b.wild.mon == e->a) m->sh_wild_hp = m->b.wild.hp;
        m->sh_wild_gone = 0;
        battle_show(m);
        snprintf(t, cap, "%s sends out %s!", m->keeper[0] ? m->keeper : "The keeper", mon_name(m, e->a, a)); return 1;
    case MP_EV_BEATEN:
        snprintf(t, cap, "You beat %s!", person_name(m, e->a, a)); return 1;
    case MP_EV_HEALED:
        snprintf(t, cap, "Your critters are rested."); return 1;
    case MP_EV_USE:
        snprintf(t, cap, "%s%s uses %s.", who, nm, move_name(m, e->a, b)); return 1;
    case MP_EV_MISS:
        snprintf(t, cap, "It misses."); return 1;
    case MP_EV_DAMAGE: {
        *hp = (uint16_t)(*hp > e->n ? *hp - e->n : 0);
        battle_labels(m); redraw();
        const char *how = e->a >= 200 ? " It's very effective." : e->a == 0 ? " It has no effect." :
                          e->a <= 50 ? " It's not very effective." : "";
        snprintf(t, cap, "%s%s loses %u HP.%s", who, nm, (unsigned)e->n, how); return 1;
    }
    case MP_EV_HEAL:
        *hp = (uint16_t)(*hp + e->n);
        battle_labels(m); redraw();
        snprintf(t, cap, "%s%s gains %u HP.", who, nm, (unsigned)e->n); return 1;
    case MP_EV_STATUS:
        snprintf(t, cap, "%s%s gets %s.", who, nm, status_name(m, e->a, b)); return 1;
    case MP_EV_IMMUNE:
        snprintf(t, cap, "%s%s can't get %s.", who, nm, status_name(m, e->a, b)); return 1;
    case MP_EV_STATUS_HURT:
        *hp = (uint16_t)(*hp > e->n ? *hp - e->n : 0);
        battle_labels(m); redraw();
        snprintf(t, cap, "%s%s loses %u HP to %s.", who, nm, (unsigned)e->n, status_name(m, e->a, b)); return 1;
    case MP_EV_STATUS_HEAL:
        if(!e->n) return 0;                         /* already full: nothing to say */
        *hp = (uint16_t)(*hp + e->n);
        battle_labels(m); redraw();
        snprintf(t, cap, "%s%s gains %u HP from %s.", who, nm, (unsigned)e->n, status_name(m, e->a, b)); return 1;
    case MP_EV_STATUS_END:
        snprintf(t, cap, "%s%s's %s ends.", who, nm, status_name(m, e->a, b)); return 1;
    case MP_EV_RECHARGE:
        snprintf(t, cap, "%s%s has to rest.", who, nm); return 1;
    case MP_EV_FAINT:
        if(s) m->sh_wild_gone = 1; else m->sh_me_gone = 1;
        redraw();
        snprintf(t, cap, "%s%s faints.", who, nm); return 1;
    case MP_EV_SWAP:
        m->sh_active = (uint8_t)e->a;
        m->sh_me_gone = 0;
        battle_show(m);
        snprintf(t, cap, "Go, %s!", mon_name(m, m->sh_mon[e->a], a)); return 1;
    case MP_EV_THROW:
        m->shakes = 0;
        snprintf(t, cap, "You throw a capture ball."); return 1;
    case MP_EV_SHAKE:
        m->shakes = (uint8_t)e->a;
        return 0;
    case MP_EV_BREAK_FREE:
        if(m->shakes) snprintf(t, cap, "It shakes %u time%s and breaks free!", m->shakes, m->shakes == 1 ? "" : "s");
        else snprintf(t, cap, "It breaks free at once!");
        return 1;
    case MP_EV_CAUGHT:
        m->sh_wild_gone = 1;
        redraw();
        snprintf(t, cap, "Caught %s! It %s.", mon_name(m, m->sh_wild_mon, a),
                 e->b ? "goes into storage" : "joins your party");
        return 1;
    case MP_EV_RUN_FAIL:
        snprintf(t, cap, "You can't get away!"); return 1;
    case MP_EV_RUN:
        snprintf(t, cap, "You got away."); return 1;
    case MP_EV_EXP:
        snprintf(t, cap, "%s gains %u experience.", mon_name(m, m->sh_mon[e->a], a), (unsigned)e->n); return 1;
    case MP_EV_LEVEL:
        battle_labels(m);
        snprintf(t, cap, "%s reaches level %u!", mon_name(m, m->sh_mon[e->a], a), e->b); return 1;
    case MP_EV_LEARN:
        snprintf(t, cap, "%s learns %s!", mon_name(m, m->sh_mon[e->a], a), move_name(m, e->b, b)); return 1;
    case MP_EV_LEARN_FULL:
        m->learn_pending = 1;
        m->learn_who = (uint8_t)e->a;
        m->learn_move = e->b;
        snprintf(t, cap, "%s could learn %s, but it knows four moves already.",
                 mon_name(m, m->sh_mon[e->a], a), move_name(m, e->b, b));
        return 1;
    case MP_EV_EVOLVE:
        mon_name(m, m->sh_mon[e->a], a);
        m->sh_mon[e->a] = e->b;
        if(e->a == m->sh_active) battle_show(m);
        snprintf(t, cap, "%s becomes %s!", a, mon_name(m, e->b, b)); return 1;
    case MP_EV_LOSE:
        snprintf(t, cap, "Your critters are all worn out. You hurry back to where you last rested, "
                 "and they're rested."); return 1;
    case MP_EV_WIN:
        return 0;
    default:
        return 0;
    }
}

static void card_show(Mon *m, const char *t, int next, int ticks);
static void scene_map(Mon *m);

static void script_done(Mon *m){
    if(m->scene == SC_BATTLE){
        if(m->b.over){
            /* back to the map: the tile memory held the sprites, and a lost
             * battle moved the player to the start */
            card_show(m, "", CARD_MAP, 6);
            return;
        }
        shown_now(m);
        battle_show(m);
        if(m->b.need_swap) party_menu(m, MN_SWAP, 0);
        else { m->sel = 0; battle_menu(m); }      /* each turn's menu starts at Fight */
        return;
    }
    box_hide(m);
}

static void script_next(Mon *m){
    char t[320];
    while(m->pos < m->log.n){
        const MpEvent *e = &m->log.ev[m->pos];
        if(m->scene == SC_MAP && (e->kind == MP_EV_TRAINER || e->kind == MP_EV_ENCOUNTER)){
            ev_text(m, e, t, sizeof t);
            m->pos++;
            card_show(m, "", CARD_BATTLE, 8);     /* the battle goes on from the next event */
            return;
        }
        m->pos++;
        if(ev_text(m, e, t, sizeof t)){ box_say(m, t); return; }
    }
    script_done(m);
}

static void script_start(Mon *m){
    m->pos = 0;
    m->learn_pending = 0;
    script_next(m);
}

/* ---- scenes ---- */

static void fail(Mon *m, const char *what, const char *why){
    snprintf(m->err, sizeof m->err, "%s", why ? why : "");
    char t[200];
    snprintf(t, sizeof t, "%s%s%s", what, why && why[0] ? "\n" : "", why ? why : "");
    m->scene = SC_ERROR;
    battle_labels(m);
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_HIDDEN);
    box_say(m, t);
    redraw();
}

static void card_show(Mon *m, const char *t, int next, int ticks){
    m->scene = SC_CARD;
    m->card_next = (uint8_t)next;
    m->card_ticks = (uint8_t)ticks;
    battle_labels(m);
    box_hide(m);
    lv_label_set_text(m->card, t);
    lv_obj_clear_flag(m->card, LV_OBJ_FLAG_HIDDEN);
    redraw();
}

static void scene_map(Mon *m){
    m->scene = SC_MAP;
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_HIDDEN);
    battle_labels(m);
    box_hide(m);
    area_pictures(m);
    redraw();
}

static void scene_battle(Mon *m){
    m->scene = SC_BATTLE;
    m->bot_act = -1;
    m->sel = 0;                     /* a new battle's menu starts at Fight */
    m->last_move = 0;
    m->to_moves = 0;
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_HIDDEN);
    cache_clear(m);
    shown_now(m);
    keeper_name(m);
    battle_show(m);
}

static void card_end(Mon *m){
    if(m->card_next == CARD_BATTLE){
        scene_battle(m);
        if(m->log.n && m->pos && m->log.ev[m->pos - 1].kind == MP_EV_ENCOUNTER) m->pos--;
        script_next(m);             /* the step's log from where it got to: "A wild X appears!" */
    }
    else scene_map(m);
}

/* the starters: the three the file names (META 0x12); or, in a file without
 * them, the first three creatures that begin an evolution line, each of a
 * different first type, then any creature at its first stage */
static void pick_candidates(Mon *m){
    m->npick = 0;
    if(m->g.n_starters){                    /* the file names them */
        for(int i = 0; i < m->g.n_starters; i++) m->pick[m->npick++] = m->g.starters[i];
        return;
    }
    for(int pass = 0; pass < 3 && m->npick < 3; pass++)
        for(uint32_t i = 0; i < m->g.n_mons && m->npick < 3; i++){
            MgMon r;
            if(mg_mon(&m->g, i, &r)) continue;
            if(pass < 2 && r.stage != 0) continue;
            if(pass == 0 && r.evolve_to == MG_NONE16) continue;
            int dup = 0;
            for(int k = 0; k < m->npick; k++){
                MgMon o;
                if(m->pick[k] == i) dup = 1;
                else if(pass == 0 && !mg_mon(&m->g, m->pick[k], &o) && o.type[0] == r.type[0]) dup = 1;
            }
            if(!dup) m->pick[m->npick++] = (uint16_t)i;
        }
}

static void look_show(Mon *m){
    char t[96];
    snprintf(t, sizeof t, "Choose how you look.\n<  %u of %u  >", m->look + 1u, m->g.looks);
    box_say(m, t);
    redraw();
}

static void pick_show(Mon *m){
    char a[NAME_LEN], ty[NAME_LEN] = "", t[96];
    MgMon r;
    uint16_t mon = m->pick[m->sel];
    MgStr s;
    if(!mg_mon(&m->g, mon, &r) && r.type[0] != MG_NONE8 && !mg_type_name(&m->g, r.type[0], &s))
        mtext(&m->g, s, ty, sizeof ty);
    sprite_load(m, 1, mon);
    snprintf(t, sizeof t, "Choose your first critter.\n%s %s%s%s  %s", m->npick > 1 ? "<" : "",
             mon_name(m, mon, a), ty[0] ? ", " : "", ty, m->npick > 1 ? ">" : "");
    box_say(m, t);
    redraw();
}

static void scene_pick(Mon *m){
    pick_candidates(m);
    if(!m->npick){ fail(m, "This game file has no creatures.", NULL); return; }
    m->scene = SC_PICK;
    m->started = 0;
    m->sel = 0;
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_HIDDEN);
    battle_labels(m);
    cache_clear(m);
    m->picking_look = 0;
    if(m->g.looks > 1 && !mg_area_open(&m->g, m->g.start_area, &m->w.area, m->head, m->head_cap)){
        /* the looks are every area's first sheets: the start area's will do */
        for(int i = 0; i < 256; i++) m->lut[i] = mg_palette(&m->w.area, (uint32_t)i);
        m->picking_look = 1;
        if(m->look >= m->g.looks) m->look = 0;
        look_show(m);
        return;
    }
    pick_show(m);
}

static uint32_t new_seed(void){
#ifdef UI_DEVTOOLS
    return 1;                       /* the smoke tour meets the same creatures every run */
#else
    return (uint32_t)time(NULL) ^ (lv_tick_get() << 16) ^ 0x5DEECE66u;
#endif
}

static void pick_choose(Mon *m){
    m->w.head_cap = m->head_cap;
    if(mp_new(&m->w, &m->g, m->head, new_seed(), m->pick[m->sel])){ fail(m, "The game couldn't start.", m->w.err); return; }
    m->w.look = m->look < m->g.looks ? m->look : 0;
    m->started = 1;
    char a[NAME_LEN], t[200], area[NAME_LEN];
    if(mtext(&m->g, m->w.area.dir.name, area, sizeof area)) area[0] = 0;
    scene_map(m);
    snprintf(t, sizeof t, "%s is yours. Wild critters live in the tall grass, and A talks to the people of %s.",
             mon_name(m, m->w.party[0].mon, a), area[0] ? area : "these parts");
    box_say(m, t);
}

/* ---- the pause file: a battle survives leaving and the lock ---- */

typedef struct {
    char     magic[4];
    uint32_t size, fingerprint;
    MpBattle b;
    uint32_t crc;
} MonPause;

static void pause_write(Mon *m){
    MonPause p;
    memset(&p, 0, sizeof p);
    memcpy(p.magic, "CYDP", 4);
    p.size = sizeof p;
    p.fingerprint = m->g.fingerprint;
    p.b = m->b;
    p.crc = mg_crc32(0, &p, offsetof(MonPause, crc));
    FILE *f = fopen(MON_PAUSE, "wb");
    if(!f) return;
    fwrite(&p, 1, sizeof p, f);
    fclose(f);
}

/* the battle that was paused, checked against this file and this party */
static int pause_read(Mon *m){
    MonPause p;
    FILE *f = fopen(MON_PAUSE, "rb");
    if(!f) return 0;
    int ok = fread(&p, 1, sizeof p, f) == sizeof p;
    fclose(f);
    remove(MON_PAUSE);
    if(!ok || memcmp(p.magic, "CYDP", 4) || p.size != sizeof p || p.fingerprint != m->g.fingerprint ||
       p.crc != mg_crc32(0, &p, offsetof(MonPause, crc)) || p.b.over) return 0;
    MpBattle *b = &p.b;
    if(b->active >= m->w.n_party || b->wild.mon >= m->g.n_mons || !b->wild.hp ||
       b->wild.status > m->g.n_statuses || !b->wild.level) return 0;
    if(b->trainer){
        MgPerson q;
        if(b->trainer > m->w.area.n_people || b->n_team > MG_TEAM || !b->n_team || b->next > b->n_team) return 0;
        mg_person(&m->w.area, b->trainer - 1u, &q);
        if(q.kind != MG_P_TRAINER || mp_flag(&m->w, q.flag)) return 0;
        for(int i = b->next; i < b->n_team; i++)
            if(b->team[i].mon >= m->g.n_mons || !b->team[i].level || b->team[i].status > m->g.n_statuses) return 0;
    }
    for(int s = 0; s < MP_MOVES; s++)
        if(b->wild.moves[s] != MP_NO_MOVE && b->wild.moves[s] >= m->g.n_moves) return 0;
    MpCreature c = b->wild;
    if(mp_stats(&m->g, &c) || memcmp(c.stat, b->wild.stat, sizeof c.stat) || b->wild.hp > c.stat[MG_HP]) return 0;
    m->b = *b;
    return 1;
}

/* ---- input ---- */

static void do_turn(Mon *m, int act, int arg){
    shown_now(m);
    box_hide(m);
    int r = mp_turn(&m->w, &m->b, act, arg, &m->log);
    m->bot_act = -1;
    if(m->bot) mb_events(m->bot, &m->b, &m->log);
    if(r == MG_ERANGE){ box_say(m, "That can't be done now."); m->log.n = 0; m->pos = 0; return; }
    if(r){ fail(m, "Something went wrong in the battle.", m->w.err); return; }
    script_start(m);
}

static void choose(Mon *m){
    int s = m->sel;
    switch(m->menu){
    case MN_BATTLE:
        if(s == 0){
            MpCreature *c = &m->w.party[m->b.active];
            int any = 0;
            for(int k = 0; k < MP_MOVES; k++) if(c->moves[k] != MP_NO_MOVE && !m->b.recharge[0][k]) any = 1;
            if(any) moves_menu(m); else do_turn(m, MP_ACT_MOVE, -1);
        }
        else if(s == 1) party_menu(m, MN_SWAP, 1);
        else if(m->b.trainer){
            m->to_menu = 1;
            box_say(m, s == 2 ? "You can't catch another keeper's critter." : "There's no running from a keeper's battle.");
        }
        else if(s == 2) do_turn(m, MP_ACT_CATCH, 0);
        else do_turn(m, MP_ACT_RUN, 0);
        break;
    case MN_MOVES: {
        if(s >= MP_MOVES){ m->sel = 0; battle_menu(m); break; }
        MpCreature *c = &m->w.party[m->b.active];
        if(c->moves[s] == MP_NO_MOVE) break;
        if(m->b.recharge[0][s]){
            char a[NAME_LEN], t[96];
            unsigned k = m->b.recharge[0][s];
            snprintf(t, sizeof t, "%s needs %u more turn%s to rest.", move_name(m, c->moves[s], a), k, k == 1 ? "" : "s");
            m->to_moves = 1;
            box_say(m, t);
            break;
        }
        m->last_move = (uint8_t)s;
        do_turn(m, MP_ACT_MOVE, s);
        break;
    }
    case MN_SWAP:
        if(s >= m->w.n_party){ m->sel = 1; battle_menu(m); break; }
        if(s == m->b.active || !m->w.party[s].hp) break;     /* not that one */
        m->last_move = 0;
        do_turn(m, MP_ACT_SWAP, s);
        break;
    case MN_PARTY:
        box_hide(m);
        break;
    case MN_FORGET: {
        char a[NAME_LEN], b[NAME_LEN], c[NAME_LEN], t[160];
        MpCreature *cr = &m->w.party[m->learn_who];
        m->learn_pending = 0;
        if(s < MP_MOVES){
            move_name(m, cr->moves[s], b);
            mp_learn(&m->g, cr, m->learn_move, s);
            snprintf(t, sizeof t, "%s forgets %s and learns %s.", mon_name(m, m->sh_mon[m->learn_who], a), b,
                     move_name(m, m->learn_move, c));
        }
        else snprintf(t, sizeof t, "%s keeps its moves.", mon_name(m, m->sh_mon[m->learn_who], a));
        box_say(m, t);
        break;
    }
    case MN_NEW:
        if(s == 0) scene_pick(m); else box_hide(m);
        break;
    }
}

static void press_a(Mon *m){
    switch(m->ui){
    case UI_TEXT:
        if(m->scene == SC_ERROR) return;
        if(m->learn_pending){ forget_menu(m); return; }
        if(m->to_moves){ m->to_moves = 0; moves_menu(m); return; }
        if(m->to_menu){ m->to_menu = 0; m->sel = 0; battle_menu(m); return; }
        if(m->scene == SC_PICK){
            if(m->pick_note){ m->pick_note = 0; if(m->picking_look) look_show(m); else pick_show(m); }
            else if(m->picking_look){ m->picking_look = 0; cache_clear(m); pick_show(m); }
            else pick_choose(m);
            return;
        }
        script_next(m);
        return;
    case UI_MENU:
        choose(m);
        return;
    case UI_CREDITS:
        return;
    }
    if(m->scene == SC_MAP && !m->anim){
        int r = mp_interact(&m->w, &m->b, &m->log);
        if(m->bot) mb_events(m->bot, &m->b, &m->log);
        if(r < 0){ fail(m, "Something went wrong.", m->w.err); return; }
        if(r > 0) script_start(m);
    }
}

static void credits_close(Mon *m){
    if(m->credits){ lv_obj_delete(m->credits); m->credits = NULL; }
    free(m->cred); m->cred = NULL;
    m->ui = m->scene == SC_ERROR ? UI_TEXT : UI_NONE;
}

static void press_b(Mon *m){
    switch(m->ui){
    case UI_TEXT:
        press_a(m);
        return;
    case UI_MENU:
        if(m->menu_back){
            if(m->menu == MN_MOVES || m->menu == MN_SWAP){ m->sel = m->menu == MN_SWAP; battle_menu(m); }
            else box_hide(m);
        }
        return;
    case UI_CREDITS:
        credits_close(m);
        return;
    }
}

static void press_dir(Mon *m, int d){
    if(m->ui == UI_MENU){ if(d == BT_N) menu_cursor(m, -1); else if(d == BT_S) menu_cursor(m, 1); return; }
    if(m->ui == UI_CREDITS){
        if(d == BT_N || d == BT_S){               /* a few lines, never past either end */
            int y = lv_obj_get_scroll_y(m->credits), most = y + lv_obj_get_scroll_bottom(m->credits);
            y = imax(0, imin(most, y + (d == BT_N ? -56 : 56)));
            lv_obj_scroll_to_y(m->credits, y, LV_ANIM_OFF);
        }
        return;
    }
    if(m->scene == SC_PICK && m->ui == UI_TEXT && m->picking_look && !m->pick_note){
        int n = m->g.looks;
        m->look = (uint8_t)((m->look + (d == BT_E || d == BT_S ? 1 : n - 1)) % n);
        look_show(m);
        return;
    }
    if(m->scene == SC_PICK && m->ui == UI_TEXT && m->npick > 1 && !m->pick_note){
        if(d == BT_E || d == BT_S) m->sel = (uint8_t)((m->sel + 1) % m->npick);
        else m->sel = (uint8_t)((m->sel + m->npick - 1) % m->npick);
        pick_show(m);
        return;
    }
    if(m->scene == SC_MAP && m->ui == UI_NONE) m->pending = (int8_t)d;
}

/* one press of a direction on the map */
static void walk(Mon *m, int d){
    int fx = m->w.x, fy = m->w.y, area = m->w.area.index;
    int r = mp_step(&m->w, d, &m->b, &m->log);
    if(m->bot) mb_events(m->bot, &m->b, &m->log);
    m->pos = 0;
    if(r){ fail(m, "The map couldn't be read.", m->w.err); return; }
    int warp = 0, hop = 0, step = 0;
    for(int i = 0; i < m->log.n; i++){
        int k = m->log.ev[i].kind;
        if(k == MP_EV_WARP) warp = 1;
        if(k == MP_EV_HOP) hop = 1;
        if(k == MP_EV_STEP) step = 1;
        if(k == MP_EV_TURN) m->turn_wait = TURN_HOLD;
    }
    if(warp || m->w.area.index != area){
        char t[NAME_LEN];
        if(mtext(&m->g, m->w.area.dir.name, t, sizeof t)) t[0] = 0;
        card_show(m, t, CARD_MAP, 20);
        return;
    }
    if(step || hop){
        m->from_x = (int16_t)fx; m->from_y = (int16_t)fy;
        m->anim_len = (uint8_t)(hop ? 2 * STEP_TICKS : STEP_TICKS);
        m->anim = m->anim_len;
        m->parity ^= 1;
        m->walk_col = (uint8_t)(m->parity ? 0 : 2);
        m->battle_after = !m->b.over;
    }
    redraw();
}

/* ---- Auto-play ---- */

static void auto_set(Mon *m, int on){
    if(on && !m->bot && m->started){
        MbBot *b = malloc(sizeof *b);
        uint8_t *scratch = malloc(m->head_cap ? m->head_cap : 1);
        if(b && scratch && !mb_init(b, &m->g, m->w.rng ^ 0x9E3779B9u, scratch, m->head_cap)) m->bot = b;
        else free(b);
        free(scratch);
    }
    if(!on && m->bot){ mb_free(m->bot); free(m->bot); m->bot = NULL; }
    m->autoplay = (uint8_t)(m->bot != NULL);
    m->bot_wait = 0;
    m->bot_act = -1;
    ui_mon_auto_changed(m->autoplay);
}

/* a menu: the cursor a line nearer what the bot chose, or, there, choose it */
static void bot_menu(Mon *m){
    int want = 0;
    switch(m->menu){
    case MN_BATTLE:
    case MN_MOVES:
    case MN_SWAP:
        if(m->bot_act < 0 || (m->b.need_swap && m->bot_act != MP_ACT_SWAP)){
            int a, g;
            mb_battle(m->bot, &m->w, &m->b, &a, &g);
            m->bot_act = (int8_t)a; m->bot_arg = (int8_t)g;
        }
        if(m->menu == MN_BATTLE)
            want = m->bot_act == MP_ACT_MOVE ? 0 : m->bot_act == MP_ACT_SWAP ? 1 : m->bot_act == MP_ACT_CATCH ? 2 : 3;
        else if(m->menu == MN_MOVES) want = m->bot_act == MP_ACT_MOVE && m->bot_arg >= 0 ? m->bot_arg : MP_MOVES;
        else want = m->bot_act == MP_ACT_SWAP ? m->bot_arg : m->w.n_party;
        break;
    case MN_FORGET:
        want = mb_learn(m->bot, &m->w, m->learn_who, m->learn_move);
        if(want < 0) want = MP_MOVES;
        break;
    case MN_NEW:
        want = 1;                   /* keep playing */
        break;
    default:
        box_hide(m);
        return;
    }
    if(want >= m->nmenu) want = m->nmenu - 1;
    if(want != m->sel) menu_cursor(m, want > m->sel ? 1 : -1);
    else choose(m);
}

/* one of Auto-play's presses, when its pause is over */
static void bot_tick(Mon *m){
    if(m->bot_wait){ m->bot_wait--; return; }
    if(m->scene == SC_ERROR){ auto_set(m, 0); return; }
    if(m->ui == UI_CREDITS){ credits_close(m); m->bot_wait = AUTO_MENU; return; }
    if(m->ui == UI_TEXT){ press_a(m); m->bot_wait = AUTO_TEXT; return; }
    if(m->ui == UI_MENU){ bot_menu(m); m->bot_wait = AUTO_MENU; return; }
    if(m->scene != SC_MAP) return;
    int d, k = mb_world(m->bot, &m->w, &d);
    if(k < 0){ fail(m, "The map couldn't be read.", m->g.err); return; }
    if(k == MB_DONE){
        auto_set(m, 0);
        box_say(m, "Every keeper in the region is beaten. Auto-play hands the game back to you.");
        return;
    }
    if(k == MB_TALK) press_a(m);
    else walk(m, d);
    m->bot_wait = AUTO_STEP;
}

static void tick_cb(lv_timer_t *t){
    (void)t;
    Mon *m = g_mon;
    if(!m) return;
    if(m->scene == SC_CARD){
        if(m->card_ticks) m->card_ticks--;
        else card_end(m);
        return;
    }
    if(m->anim){
        m->anim--;
        if(m->anim == m->anim_len / 2) m->walk_col = 1;
        redraw();
        if(!m->anim && m->battle_after){
            m->battle_after = 0;
            script_start(m);          /* a keeper's words first; then the card, at the battle */
        }
        return;
    }
    if(m->autoplay){ bot_tick(m); return; }
    if(m->turn_wait) m->turn_wait--;
    if(m->scene != SC_MAP || m->ui != UI_NONE){ m->pending = -1; return; }
    int d = m->pending;
    m->pending = -1;
    if(d < 0 && m->held >= 0 && !m->turn_wait) d = m->held;
    if(d >= 0) walk(m, d);
}

/* ---- the controls ---- */

#define DPAD_X 64
#define DPAD_Y 60
#define KEY    36
#define A_X    204
#define A_Y    46
#define B_X    150
#define B_Y    80
#define AB_R   24

static int pad_hit(lv_obj_t *pad){
    lv_point_t p;
    lv_area_t a;
    lv_indev_t *in = lv_indev_active();
    if(!in) return BT_NONE;
    lv_indev_get_point(in, &p);
    lv_obj_get_coords(pad, &a);
    int x = p.x - a.x1, y = p.y - a.y1;
    int dx = x - A_X, dy = y - A_Y;
    if(dx * dx + dy * dy <= (AB_R + 8) * (AB_R + 8)) return BT_A;
    dx = x - B_X; dy = y - B_Y;
    if(dx * dx + dy * dy <= (AB_R + 8) * (AB_R + 8)) return BT_B;
    dx = x - DPAD_X; dy = y - DPAD_Y;
    if(dx * dx + dy * dy > 64 * 64 || (abs(dx) < 8 && abs(dy) < 8)) return BT_NONE;
    if(abs(dx) > abs(dy)) return dx > 0 ? BT_E : BT_W;
    return dy > 0 ? BT_S : BT_N;
}

static void pad_cb(lv_event_t *e){
    Mon *m = g_mon;
    if(!m) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *pad = lv_event_get_current_target(e);
    if(code == LV_EVENT_PRESSED && m->autoplay){ auto_set(m, 0); return; }     /* a touch takes over */
    if(code == LV_EVENT_PRESSED){
        int b = pad_hit(pad);
        m->pressed = (int8_t)b;
        if(b >= BT_N && b <= BT_W){ m->held = (int8_t)b; press_dir(m, b); }
        else if(b == BT_A) press_a(m);
        else if(b == BT_B) press_b(m);
        lv_obj_invalidate(pad);
    }
    else if(code == LV_EVENT_PRESSING){
        if(m->held < 0) return;
        int b = pad_hit(pad);
        if(b >= BT_N && b <= BT_W && b != m->held){ m->held = (int8_t)b; m->pressed = (int8_t)b; lv_obj_invalidate(pad); }
    }
    else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST){
        m->held = -1;
        m->pressed = BT_NONE;
        lv_obj_invalidate(pad);
    }
}

static void pad_draw_cb(lv_event_t *e){
    Mon *m = g_mon;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *pad = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(pad, &a);
    int ox = a.x1, oy = a.y1;
    int pressed = m ? m->pressed : BT_NONE;

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(0x404040);
    r.radius = 4;
    lv_area_t k;
    /* the cross */
    k.x1 = ox + DPAD_X - KEY / 2; k.x2 = k.x1 + KEY - 1; k.y1 = oy + DPAD_Y - KEY * 3 / 2; k.y2 = k.y1 + 3 * KEY - 1;
    lv_draw_rect(layer, &r, &k);
    k.x1 = ox + DPAD_X - KEY * 3 / 2; k.x2 = k.x1 + 3 * KEY - 1; k.y1 = oy + DPAD_Y - KEY / 2; k.y2 = k.y1 + KEY - 1;
    lv_draw_rect(layer, &r, &k);
    if(pressed >= BT_N && pressed <= BT_W){
        lv_draw_rect_dsc_t h;
        lv_draw_rect_dsc_init(&h);
        h.bg_color = lv_color_hex(0x000000);
        h.radius = 4;
        k.x1 = ox + DPAD_X - KEY / 2 + DXS[pressed] * KEY; k.x2 = k.x1 + KEY - 1;
        k.y1 = oy + DPAD_Y - KEY / 2 + DYS[pressed] * KEY; k.y2 = k.y1 + KEY - 1;
        lv_draw_rect(layer, &h, &k);
    }
    /* the arrows */
    lv_draw_triangle_dsc_t t;
    for(int d = 0; d < 4; d++){
        lv_draw_triangle_dsc_init(&t);
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3      /* the sim's 9.2 */
        t.bg_color = lv_color_hex(0xFFFFFF);
#else
        t.color = lv_color_hex(0xFFFFFF);
#endif
        int cx = ox + DPAD_X + DXS[d] * KEY, cy = oy + DPAD_Y + DYS[d] * KEY;
        int px = DYS[d], py = -DXS[d];            /* across the arrow */
        t.p[0].x = cx + DXS[d] * 7;  t.p[0].y = cy + DYS[d] * 7;
        t.p[1].x = cx - DXS[d] * 5 + px * 7; t.p[1].y = cy - DYS[d] * 5 + py * 7;
        t.p[2].x = cx - DXS[d] * 5 - px * 7; t.p[2].y = cy - DYS[d] * 5 - py * 7;
        lv_draw_triangle(layer, &t);
    }
    /* A and B */
    static const char *const AB[2] = { "A", "B" };
    const int X[2] = { A_X, B_X }, Y[2] = { A_Y, B_Y };
    for(int i = 0; i < 2; i++){
        lv_draw_rect_dsc_t c;
        lv_draw_rect_dsc_init(&c);
        c.bg_color = lv_color_hex(pressed == BT_A + i ? 0x000000 : i ? 0x404040 : 0x7A1F2B);
        c.radius = LV_RADIUS_CIRCLE;
        k.x1 = ox + X[i] - AB_R; k.x2 = ox + X[i] + AB_R - 1; k.y1 = oy + Y[i] - AB_R; k.y2 = oy + Y[i] + AB_R - 1;
        lv_draw_rect(layer, &c, &k);
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.color = lv_color_hex(0xFFFFFF);
        l.font = &lv_font_palm_bold_2x;
        l.text = AB[i];
        l.align = LV_TEXT_ALIGN_CENTER;
        k.y1 = oy + Y[i] - lv_font_palm_bold_2x.line_height / 2; k.y2 = k.y1 + lv_font_palm_bold_2x.line_height;
        lv_draw_label(layer, &l, &k);
    }
}

static void box_click_cb(lv_event_t *e){
    Mon *m = g_mon;
    if(!m) return;
    if(m->autoplay){ auto_set(m, 0); return; }
    if(m->ui == UI_TEXT){ press_a(m); return; }
    if(m->ui != UI_MENU) return;
    lv_point_t p;
    lv_area_t a;
    lv_indev_t *in = lv_indev_active();
    if(!in) return;
    lv_indev_get_point(in, &p);
    lv_obj_get_coords(m->text, &a);
    int line = (p.y - a.y1) / LINE_H;
    (void)e;
    if(line < 0 || line >= m->nmenu) return;
    m->sel = (uint8_t)line;
    lv_obj_set_y(m->cursor, m->sel * LINE_H);
    choose(m);
}

static void view_press_cb(lv_event_t *e){
    (void)e;
    if(g_mon && g_mon->autoplay) auto_set(g_mon, 0);
}

/* ---- opening and closing ---- */

static lv_obj_t *label(lv_obj_t *par, const lv_font_t *f, lv_color_t col){
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static void build(Mon *m, lv_obj_t *parent){
    m->view = lv_obj_create(parent);
    lv_obj_remove_style_all(m->view);
    lv_obj_set_size(m->view, VW, VH);
    lv_obj_set_pos(m->view, 0, 0);
    lv_obj_clear_flag(m->view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m->view, view_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(m->view, LV_OBJ_FLAG_CLICKABLE);           /* a touch on the picture stops Auto-play */
    lv_obj_add_event_cb(m->view, view_press_cb, LV_EVENT_PRESSED, NULL);

    m->card = label(m->view, &lv_font_palm_bold_2x, lv_color_hex(0xFFFFFF));
    lv_obj_center(m->card);
    lv_obj_add_flag(m->card, LV_OBJ_FLAG_HIDDEN);

    m->foe = label(m->view, &lv_font_palm_bold, lv_color_hex(0x000000));
    lv_obj_set_pos(m->foe, FOE_CARD_X + 6, FOE_CARD_Y + 4);
    m->me = label(m->view, &lv_font_palm_bold, lv_color_hex(0x000000));
    lv_obj_set_pos(m->me, ME_CARD_X + 6, ME_CARD_Y + 4);
    m->mehp = label(m->view, &lv_font_palm, lv_color_hex(0x000000));
    lv_obj_set_pos(m->mehp, ME_CARD_X + 6, ME_CARD_Y + 29);

    m->box = lv_obj_create(m->view);
    lv_obj_remove_style_all(m->box);
    lv_obj_set_style_bg_color(m->box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(m->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m->box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(m->box, 2, 0);
    lv_obj_set_style_radius(m->box, 3, 0);
    lv_obj_set_style_pad_all(m->box, 5, 0);
    lv_obj_set_width(m->box, VW - 4);
    lv_obj_set_height(m->box, LV_SIZE_CONTENT);
    lv_obj_align(m->box, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(m->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->box, box_click_cb, LV_EVENT_PRESSED, NULL);
    m->cursor = label(m->box, &lv_font_palm_bold, lv_color_hex(0x000000));
    lv_label_set_text(m->cursor, ">");
    lv_obj_set_pos(m->cursor, 0, 0);
    m->text = label(m->box, &lv_font_palm, lv_color_hex(0x000000));
    lv_obj_set_pos(m->text, 12, 0);
    lv_obj_set_width(m->text, VW - 4 - 14 - 12);
    lv_label_set_long_mode(m->text, LV_LABEL_LONG_WRAP);
    box_hide(m);

    m->pad = lv_obj_create(parent);
    lv_obj_remove_style_all(m->pad);
    lv_obj_set_size(m->pad, VW, PAD_H);
    lv_obj_set_pos(m->pad, 0, VH);
    lv_obj_set_style_bg_color(m->pad, lv_color_hex(0xD6D6D6), 0);
    lv_obj_set_style_bg_opa(m->pad, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m->pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m->pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->pad, pad_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(m->pad, pad_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
}

static void no_memory(lv_obj_t *parent){
    lv_obj_t *l = label(parent, &lv_font_palm, lv_color_hex(0x000000));
    lv_obj_set_width(l, VW - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "There isn't enough memory free to play just now (a sync uses a lot). "
                         "Try again in a moment.");
    lv_obj_center(l);
}

void mon_ui_show(lv_obj_t *parent){
    if(g_mon) mon_ui_close();
    Mon *m = calloc(1, sizeof *m);
    if(!m){ no_memory(parent); return; }
    int r = mg_open(&m->g, MON_FILE);
    uint32_t cap = 0;
    for(uint32_t i = 0; !r && i < m->g.n_areas; i++){
        MgAreaDir d;
        if(!mg_area_dir(&m->g, i, &d) && d.head_len > cap) cap = d.head_len;
    }
    if(!r){
        Mon *n = realloc(m, sizeof *m + cap);
        if(!n){ mg_close(&m->g); free(m); no_memory(parent); return; }
        m = n;
    }
    m->head_cap = cap;
    g_mon = m;
    m->pending = m->held = m->pressed = -1;
    cache_clear(m);
    build(m, parent);
    m->timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    if(r){
        char why[sizeof m->g.err];
        memcpy(why, m->g.err, sizeof why);
        if(r == MG_EIO)
            fail(m, "Critters' game file isn't on the card.", "The web flasher can install it, or copy "
                 "game.bin to games/monster on the card.");
        else if(r == MG_EVERSION) fail(m, "This game file needs a newer firmware.", why);
        else fail(m, "The game file is damaged.", why);
        return;
    }
    mkdir("/sdcard/games", 0777);
    mkdir(MON_DIR, 0777);
    m->w.head_cap = cap;
    r = mp_load(&m->w, &m->g, m->head, MON_SAVE);
    if(r == MG_EIO){ scene_pick(m); return; }
    if(r){
        char t[240];
        snprintf(t, sizeof t, "The saved game can't be read (%s). A starts a new one; the old save is "
                 "replaced when the new game is saved.", m->w.err);
        scene_pick(m);
        if(m->scene == SC_PICK){ box_say(m, t); m->pick_note = 1; }
        return;
    }
    m->started = 1;
    if(pause_read(m)){
        scene_battle(m);
        if(m->b.need_swap) party_menu(m, MN_SWAP, 0); else battle_menu(m);
        return;
    }
    scene_map(m);
}

void mon_ui_close(void){
    Mon *m = g_mon;
    if(!m) return;
    if(m->started){
        mp_save(&m->w, MON_SAVE);
        if(m->scene == SC_BATTLE && !m->b.over) pause_write(m);
        else if(m->scene == SC_CARD && m->card_next == CARD_BATTLE) pause_write(m);
        else remove(MON_PAUSE);
    }
    if(m->timer) lv_timer_delete(m->timer);
    if(m->bot){ mb_free(m->bot); free(m->bot); }
    free(m->cred);
    mg_close(&m->g);
    g_mon = NULL;
    free(m);
}

int mon_ui_open(void){ return g_mon != NULL; }
int mon_ui_auto(void){ return g_mon && g_mon->autoplay; }
void mon_ui_set_auto(int on){
    Mon *m = g_mon;
    if(!m || (on && (!m->started || m->scene == SC_ERROR))) return;
    auto_set(m, on);
}
int mon_ui_keep_awake(void){ return g_mon && power_on_usb(); }
int mon_ui_playing(void){ return g_mon && g_mon->started; }

int mon_ui_save(void){
    Mon *m = g_mon;
    if(!m || !m->started) return -1;
    int r = mp_save(&m->w, MON_SAVE);
    if(r) snprintf(m->err, sizeof m->err, "%s", m->w.err);
    return r;
}
const char *mon_ui_error(void){ return g_mon ? g_mon->err : ""; }

void mon_ui_party(void){
    Mon *m = g_mon;
    if(!m || !m->started || m->anim) return;
    if(m->scene == SC_BATTLE){ if(m->ui == UI_MENU && m->menu == MN_BATTLE) party_menu(m, MN_SWAP, 1); return; }
    if(m->scene != SC_MAP || m->ui != UI_NONE) return;
    party_menu(m, MN_PARTY, 0);
    m->menu_back = 1;
}

void mon_ui_new_game(void){
    /* the old save stays until the new game is saved (on leaving, or Save) */
    static const char *const L[] = { "Start a new game", "Keep playing" };
    Mon *m = g_mon;
    if(!m || m->anim || (m->scene != SC_MAP && m->scene != SC_PICK && m->scene != SC_ERROR)) return;
    if(m->scene == SC_ERROR && m->g.f == NULL) return;          /* no game file */
    if(!m->started){ scene_pick(m); return; }
    m->sel = 1;
    menu_show(m, MN_NEW, L, 2, 1);
}

#ifdef UI_DEVTOOLS
void mon_ui_test_place(int area, int x, int y, int facing){
    Mon *m = g_mon;
    if(!m || !m->started || m->scene != SC_MAP || m->anim || area < 0 || x < 0 || y < 0 || facing < 0) return;
    if(mp_place(&m->w, (uint16_t)area, (uint8_t)x, (uint8_t)y, (uint8_t)(facing & 3))){
        fail(m, "That place couldn't be reached.", m->w.err);
        return;
    }
    scene_map(m);
}
#endif

void mon_ui_credits(void){
    Mon *m = g_mon;
    if(!m || !m->g.f || m->anim || (m->ui != UI_NONE && m->scene != SC_ERROR) || m->scene == SC_CARD) return;
    uint32_t len = m->g.s[MG_CRED].len;
    const char *t;
    m->cred = malloc(len + 1);
    if(!m->cred) t = "There isn't enough memory free to show the credits just now.";
    else if(mg_read(&m->g, MG_CRED, 0, m->cred, len) || mg_crc32(0, m->cred, len) != m->g.s[MG_CRED].crc)
        t = "The credits in the game file are damaged.";
    else { m->cred[len] = 0; t = m->cred; }
    m->credits = lv_obj_create(m->view);
    lv_obj_set_size(m->credits, VW, VH);
    lv_obj_set_pos(m->credits, 0, 0);
    lv_obj_set_style_radius(m->credits, 0, 0);
    lv_obj_set_style_border_width(m->credits, 0, 0);
    lv_obj_set_style_pad_all(m->credits, 6, 0);
    lv_obj_set_style_bg_color(m->credits, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *l = label(m->credits, &lv_font_palm, lv_color_hex(0x000000));
    lv_obj_set_width(l, VW - 18);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(l, t);             /* several KB: kept in m->cred, not the pool */
    m->ui = UI_CREDITS;
}
