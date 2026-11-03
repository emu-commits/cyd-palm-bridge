/* ui_dex.c -- Dex, a fixed gadget (docs/DEX_PLAN.md section 2).
 *
 * One LVGL object is the whole gadget: its draw event paints the case, the keys
 * and both subscreens, and its press and release events are the keyboard. So
 * "the bezel and keys never change" holds by construction: only the subscreens'
 * data changes, and only their rectangles are invalidated.
 *
 * THE UPPER SCREEN has no canvas. As in Critters' view, each draw composes the
 * part LVGL is drawing (a band of at most the draw buffer) into one heap buffer, and
 * hands that to LVGL as one RGB565 image: sprite, icons, marks and card, one draw
 * task. Text goes on top as labels. crt.c then makes a TV of the box in the flush.
 *
 * MEMORY: one block for the state and one for the band, about 28 KB, taken here
 * and freed in dex_ui_close(). The card is read when the page changes, never
 * while drawing. The Where search borrows the largest area head for its moment. */
#include "ui_dex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3)
#include "src/misc/cache/instance/lv_image_cache.h"   /* lvgl.h no longer exports it */
#endif
#include "crt.h"
#include "dex.h"
#include "lv_font_palm.h"
#include "palm_icons.h"
#include "ui_monster.h"

#define DEX_FILE MON_DIR "/game.bin"
#define DEX_SAVE MON_DIR "/save.dat"
#define TV_FILE  "/sdcard/games/tv.cfg"

/* the gadget, in screen coordinates (DEX_PLAN.md 2.1; tools/mock_dex.py) */
#define TV_X0  15
#define TV_Y0  66
#define TV_X1  224
#define TV_Y1  193
#define TV_W   (TV_X1 - TV_X0 + 1)
#define TV_H   (TV_Y1 - TV_Y0 + 1)
#define TV_RADIUS 7
#define LCD_X  12
#define LCD_Y  214
#define LCD_W_ 216
#define KEY_Y0 292
#define KEY_Y1 314
#define BAND_PX (240 * 40)          /* the display's draw buffer, lvgl_port.c: a narrower strip is taller */
#define ROWS   5                    /* the list, a page */
#define ROW_H  24
#define NAME   DX_NAME

/* RGB */
#define C_RED     0x7B1C29
#define C_RED_HI  0x9C303C
#define C_GREY    0xD6D7D6
#define C_GREEN   0x9CBD5A
#define C_INK     0x183018
#define C_KEY     0x4270BD
#define C_KEY_HI  0x6392D6
#define C_WELL    0x242428
#define C_TV      0x26468C
#define C_TV_INK  0xF4F4EC
#define C_TV_DIM  0xAAC0E8
#define C_TV_SIL  0x0C183C
#define C_CREAM   0xF7F3E7
#define C_CAPTION 0x282830
#define C_GOLD    0xFAC430

enum { K_NONE = -1, K_PREV, K_LIST, K_TALK, K_NEXT, K_COUNT };
static const struct { int16_t x0, x1; } KEYS[K_COUNT] = { { 6, 44 }, { 50, 118 }, { 124, 190 }, { 196, 233 } };

enum { V_LIST, V_ENTRY };

typedef struct {
    Dex      d;
    uint8_t  game_ok;
    char     why[96];               /* no game file */
    uint8_t  view, speaking;        /* he's over the view, with his card */
    uint8_t  only_caught, side;
    int16_t  pressed;
    uint16_t page;
    int16_t  entry;                 /* the entry's Dex position */

    /* a page of the list, as read from the card */
    int16_t  row_pos[ROWS];         /* Dex positions, or -1 */
    char     row_name[ROWS][NAME];
    uint8_t  icons[ROWS][MG_ICON_BYTES];
    uint8_t  has_icon[ROWS];

    /* an entry */
    uint8_t  has_sprite;
    char     name[NAME], next[NAME], type[2][16];
    uint8_t  evolve_at, n_types;
    uint8_t  rec[MG_SPRITE_MAX];

    /* the lower screen: a bold line, and text that wraps */
    char     head[48], body[DX_LINE + 48];

    uint16_t *band;                 /* BAND_PX */
    lv_image_dsc_t img;
    lv_obj_t *obj;
} Dx;

static Dx *g_dx;

typedef struct { int x1, y1, x2, y2; } Rect;       /* inclusive, the TV's coordinates */

static int imin(int a, int b){ return a < b ? a : b; }
static int imax(int a, int b){ return a > b ? a : b; }

static uint16_t rgb(uint32_t hex){
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 | ((hex >> 10) & 0x3F) << 5 | ((hex >> 3) & 0x1F));
}

/* ---- what's on show, read from the card when it changes ---- */

static int count_pages(Dx *m){
    int n = m->only_caught ? m->d.n_caught : m->d.n;
    return n ? (n + ROWS - 1) / ROWS : 1;
}

/* the Dex position of line k of the current filter, or -1 */
static int line_pos(Dx *m, int k){
    if(k < 0) return -1;
    if(m->only_caught) return k < m->d.n_caught ? m->d.caught[k] : -1;
    return k < m->d.n ? k : -1;
}

static void load_list(Dx *m){
    for(int r = 0; r < ROWS; r++){
        int p = line_pos(m, m->page * ROWS + r);
        m->row_pos[r] = (int16_t)p;
        m->has_icon[r] = 0;
        m->row_name[r][0] = 0;
        if(p < 0 || !m->d.state[p]) continue;
        uint16_t mon = m->d.mon[p];
        m->has_icon[r] = !mg_icon(&m->d.g, mon, m->icons[r], sizeof m->icons[r]);
        MgMon g;
        if(m->d.state[p] == DX_CAUGHT && (mg_mon(&m->d.g, mon, &g) || mg_text(&m->d.g, g.name, m->row_name[r], NAME) < 0))
            m->row_name[r][0] = 0;
    }
}

static void load_entry(Dx *m){
    int p = m->entry, st = m->d.state[p];
    MgMon g;
    m->has_sprite = 0; m->name[0] = m->next[0] = 0; m->n_types = 0; m->evolve_at = 0;
    if(mg_mon(&m->d.g, m->d.mon[p], &g)) return;
    if(st != DX_NEVER) m->has_sprite = mg_sprite(&m->d.g, &g, m->rec, sizeof m->rec) > 0;
    if(st != DX_CAUGHT) return;
    if(mg_text(&m->d.g, g.name, m->name, NAME) < 0) m->name[0] = 0;
    for(int t = 0; t < 2; t++){
        MgStr s;
        if(g.type[t] == MG_NONE8 || mg_type_name(&m->d.g, g.type[t], &s)) continue;
        if(mg_text(&m->d.g, s, m->type[m->n_types], sizeof m->type[0]) >= 0) m->n_types++;
    }
    if(g.evolve_to != MG_NONE16 && g.evolve_at){
        MgMon nx;
        m->evolve_at = g.evolve_at;
        int pos = -1;
        for(int q = 0; q < m->d.n; q++) if(m->d.mon[q] == g.evolve_to) pos = q;
        /* what it becomes is ??? until it's been seen */
        if(pos < 0 || !m->d.state[pos] || mg_mon(&m->d.g, g.evolve_to, &nx) || mg_text(&m->d.g, nx.name, m->next, NAME) < 0)
            snprintf(m->next, NAME, "???");
    }
}

static void lower_list(Dx *m){
    if(!m->game_ok){
        snprintf(m->head, sizeof m->head, "NO GAME FILE");
        snprintf(m->body, sizeof m->body, "%s", m->why);
        return;
    }
    snprintf(m->head, sizeof m->head, "SEEN %u    CAUGHT %u / %u", m->d.n_seen, m->d.n_caught, m->d.n);
    if(!m->d.g.has_dex){
        snprintf(m->body, sizeof m->body, "No Dex data in this file.\nTap a critter to open it.");
        return;
    }
    snprintf(m->body, sizeof m->body, "Showing %s, page %u of %u.\nTap a critter to open it.\nPress List for %s.",
             m->only_caught ? "caught" : "all", m->page + 1, count_pages(m), m->only_caught ? "all of them" : "caught only");
}

static void lower_entry(Dx *m){
    MgDex x;
    if(m->d.state[m->entry] != DX_CAUGHT || mg_dex(&m->d.g, m->d.mon[m->entry], &x)){
        snprintf(m->head, sizeof m->head, "???");
        snprintf(m->body, sizeof m->body, "HT ???     WT ???\n???");
        return;
    }
    char desc[DX_LINE];
    if(mg_text(&m->d.g, x.category, m->head, sizeof m->head) < 0) m->head[0] = 0;
    for(char *c = m->head; *c; c++) *c = (char)toupper((unsigned char)*c);
    if(mg_text(&m->d.g, x.description, desc, sizeof desc) < 0) desc[0] = 0;
    snprintf(m->body, sizeof m->body, "HT %u.%u m     WT %u.%u kg\n%s", x.height_cm / 100, x.height_cm / 10 % 10,
             x.weight_tenths / 10, x.weight_tenths % 10, desc);
}

static lv_area_t area_tv(void){ lv_area_t a = { TV_X0, TV_Y0, TV_X1, TV_Y1 }; return a; }
static lv_area_t area_lcd(void){ lv_area_t a = { 7, 213, 232, 285 }; return a; }

static void show(Dx *m){
    if(m->game_ok){
        if(m->view == V_LIST) load_list(m);
        if(m->view == V_ENTRY || m->speaking) { if(m->view == V_ENTRY) load_entry(m); }
    }
    if(!m->speaking){
        if(m->view == V_LIST) lower_list(m); else lower_entry(m);
    }
    lv_area_t t = area_tv(), l = area_lcd();
    lv_obj_invalidate_area(m->obj, &t);
    lv_obj_invalidate_area(m->obj, &l);
}

/* his line about the player's progress, or about this critter: the search borrows
 * the largest head; without the memory he has less to say, but still speaks */
static void speak(Dx *m){
    uint8_t *head = m->game_ok && m->d.head_cap ? malloc(m->d.head_cap) : NULL;
    m->head[0] = 0;
    if(!m->game_ok) snprintf(m->body, sizeof m->body, "%s", m->why);
    else if(m->view == V_ENTRY) dx_talk(&m->d, m->entry, m->body, sizeof m->body, head);
    else dx_greeting(&m->d, m->body, sizeof m->body, head);
    free(head);
    m->speaking = 1;
}

/* ---- composing the upper screen ---- */

static void fill(uint16_t *band, int bw, const Rect *c, int x, int y, int w, int h, uint16_t col){
    int xa = imax(x, c->x1), xb = imin(x + w - 1, c->x2), ya = imax(y, c->y1), yb = imin(y + h - 1, c->y2);
    for(int j = ya; j <= yb; j++){
        uint16_t *d = band + (j - c->y1) * bw + (xa - c->x1);
        for(int i = xa; i <= xb; i++) *d++ = col;
    }
}

/* 16 colours then 4-bit rows, w across by h down at scale k; `sil`: every drawn
 * pixel that colour instead (a critter seen, not caught; DEX_PLAN.md 2.3) */
static void blit4(uint16_t *band, int bw, const Rect *c, int x, int y, const uint8_t *pal8, const uint8_t *px,
                  int w, int h, int k, int sil, uint16_t silcol){
    int xa = imax(x, c->x1), xb = imin(x + w * k - 1, c->x2), ya = imax(y, c->y1), yb = imin(y + h * k - 1, c->y2);
    for(int j = ya; j <= yb; j++){
        const uint8_t *row = px + ((j - y) / k) * (w / 2);
        uint16_t *d = band + (j - c->y1) * bw + (xa - c->x1);
        for(int i = xa; i <= xb; i++, d++){
            int u = (i - x) / k;
            int v = u & 1 ? row[u / 2] & 15 : row[u / 2] >> 4;
            if(v) *d = sil ? silcol : (uint16_t)(pal8[2 * v] | pal8[2 * v + 1] << 8);
        }
    }
}

/* frame f of the entry's sprite record */
static void blit_entry(Dx *m, const Rect *c, int bw, int x, int y, int f, int k){
    const uint8_t *rec = m->rec;
    int w = rec[0], h = rec[1];
    blit4(m->band, bw, c, x, y, rec + 8, rec + 40 + f * (w * h / 2), w, h, k, m->d.state[m->entry] == DX_SEEN, rgb(C_TV_SIL));
}

/* the marks (DEX_PLAN.md 2.4): an 11 x 11 diamond, solid gold or hollow */
static void diamond(uint16_t *band, int bw, const Rect *c, int x, int y, int caught){
    uint16_t edge = caught ? rgb(C_TV_SIL) : rgb(C_TV_INK), face = caught ? rgb(C_GOLD) : rgb(C_TV);
    for(int j = 0; j < 11; j++){
        int half = 5 - abs(j - 5);
        fill(band, bw, c, x + 5 - half, y + j, 2 * half + 1, 1, face);
        fill(band, bw, c, x + 5 - half, y + j, 1, 1, edge);
        fill(band, bw, c, x + 5 + half, y + j, 1, 1, edge);
    }
    if(caught){ fill(band, bw, c, x + 3, y + 4, 1, 1, rgb(0xFFFFFF)); fill(band, bw, c, x + 4, y + 3, 1, 1, rgb(0xFFFFFF)); }
}

#define TX(v) ((v) - TV_X0)
#define TY(v) ((v) - TV_Y0)

static void compose_list(Dx *m, const Rect *c, int bw){
    for(int r = 0; r < ROWS; r++){
        int p = m->row_pos[r], y = 4 + r * ROW_H;
        if(p < 0 || y > c->y2 || y + ROW_H <= c->y1) continue;
        int st = m->d.state[p];
        if(m->has_icon[r])
            blit4(m->band, bw, c, 29, y, m->icons[r], m->icons[r] + 32, MG_ICON_SIZE, MG_ICON_SIZE, 1, st == DX_SEEN, rgb(C_TV_SIL));
        if(st) diamond(m->band, bw, c, 187, y + 7, st == DX_CAUGHT);
    }
    /* a Palm scroll bar */
    int pages = count_pages(m), h = TV_H - 2, th = imax(h / pages, 8), ty = pages > 1 ? 1 + (h - th) * m->page / (pages - 1) : 1;
    fill(m->band, bw, c, 201, 1, 7, h, rgb(0x000000));
    fill(m->band, bw, c, 202, 2, 5, h - 2, rgb(0xFFFFFF));
    fill(m->band, bw, c, 202, ty + 1, 5, th - 2 > 0 ? th - 2 : 1, rgb(0x000000));
}

static void compose_entry(Dx *m, const Rect *c, int bw){
    if(m->has_sprite) blit_entry(m, c, bw, 0, 0, m->side, 2);
    if(m->d.state[m->entry] == DX_CAUGHT) diamond(m->band, bw, c, TX(147), TY(161), 1);
}

/* the card's inside, in the TV's coordinates: speaker() in tools/mock_dex.py */
#define CARD_X0 3
#define CARD_Y0 3
#define CARD_X1 (TV_W - 102 - 3)
#define CARD_Y1 (TV_H - 4)

static void compose_speaker(Dx *m, const Rect *c, int bw){
    int px = TV_W - 102, py = TV_H - 102;
    for(int j = imax(py, c->y1); j <= imin(py + 101, c->y2); j++){
        int xa = imax(px, c->x1), xb = imin(px + 101, c->x2);
        if(xa <= xb) memcpy(m->band + (j - c->y1) * bw + (xa - c->x1), dex_speaker + (j - py) * 102 + (xa - px), (size_t)(xb - xa + 1) * 2);
    }
    /* a card has its own background, never TV blue (DEX_PLAN.md 2.4) */
    int w = CARD_X1 - CARD_X0 + 1, h = CARD_Y1 - CARD_Y0 + 1;
    fill(m->band, bw, c, CARD_X0, CARD_Y0, w, h, rgb(0x000000));
    fill(m->band, bw, c, CARD_X0 + 1, CARD_Y0 + 1, w - 2, h - 2, rgb(C_CREAM));
    fill(m->band, bw, c, CARD_X0 + 1, CARD_Y0 + 1, w - 2, 16, rgb(C_RED));
    if(m->view == V_ENTRY){
        fill(m->band, bw, c, CARD_X0 + 1, CARD_Y1 - 32, w - 2, 32, rgb(C_CAPTION));
        if(m->has_sprite) blit_entry(m, c, bw, CARD_X0 + 1 + (w - 2 - 64) / 2, CARD_Y0 + 20, 0, 1);
    }
}

static void compose(Dx *m, const Rect *c, int bw){
    fill(m->band, bw, c, 0, 0, TV_W, TV_H, rgb(C_TV));
    if(!m->game_ok && !m->speaking) return;
    if(m->speaking) compose_speaker(m, c, bw);
    else if(m->view == V_LIST) compose_list(m, c, bw);
    else compose_entry(m, c, bw);
}

/* ---- drawing the gadget ---- */

static void text(lv_layer_t *l, const lv_area_t *clip, const lv_font_t *f, uint32_t col, lv_text_align_t al,
                 int x1, int y1, int x2, const char *s){
    if(y1 > clip->y2 || y1 + f->line_height <= clip->y1) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.font = f;
    d.text = s;
    d.align = al;
    lv_area_t k = { x1, y1, x2, y1 + f->line_height - 1 };
    lv_draw_label(l, &d, &k);
}

static void box(lv_layer_t *l, const lv_area_t *clip, int x0, int y0, int x1, int y1, uint32_t col, int radius, int edge){
    if(y0 > clip->y2 || y1 < clip->y1 || x0 > clip->x2 || x1 < clip->x1) return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(col);
    r.radius = radius;
    if(edge){ r.border_color = lv_color_hex(0x000000); r.border_width = edge; }
    lv_area_t k = { x0, y0, x1, y1 };
    lv_draw_rect(l, &r, &k);
}

static uint32_t type_colour(const char *t){
    static const struct { const char *n; uint32_t c; } T[] = {
        { "Fire", 0xE06030 }, { "Water", 0x3070D0 }, { "Wood", 0x389038 }, { "Metal", 0x7E7E8A },
        { "Earth", 0x9A6E40 }, { "Venom", 0x8C4CA8 }, { "Lightning", 0xC89C10 }, { "Aether", 0x5C50B0 },
    };
    for(size_t i = 0; i < sizeof T / sizeof T[0]; i++) if(!strcmp(t, T[i].n)) return T[i].c;
    return 0x606068;
}

static void upper_text(Dx *m, lv_layer_t *l, const lv_area_t *c){
    char t[32];
    if(m->speaking){
        int x0 = TV_X0 + CARD_X0, x1 = TV_X0 + CARD_X1, y0 = TV_Y0 + CARD_Y0, y1 = TV_Y0 + CARD_Y1;
        text(l, c, &lv_font_palm_bold, 0xFFFFFF, LV_TEXT_ALIGN_CENTER, x0, y0 + 2, x1, m->view == V_ENTRY ? "FIELD REPORT" : "YOUR DEX");
        if(m->view == V_ENTRY){
            int st = m->game_ok ? m->d.state[m->entry] : DX_NEVER;
            if(st == DX_NEVER) text(l, c, &lv_font_palm_bold_2x, 0x000000, LV_TEXT_ALIGN_CENTER, x0, y0 + 34, x1, "?");
            text(l, c, &lv_font_palm_bold, 0xFFFFFF, LV_TEXT_ALIGN_CENTER, x0, y1 - 31, x1, st == DX_CAUGHT ? m->name : "???");
            snprintf(t, sizeof t, "No. %03d", m->entry + 1);
            text(l, c, &lv_font_palm, 0xFFFFFF, LV_TEXT_ALIGN_CENTER, x0, y1 - 16, x1, t);
        } else {
            text(l, c, &lv_font_palm_bold, 0x000000, LV_TEXT_ALIGN_LEFT, x0 + 7, y0 + 23, x1, "CAUGHT");
            snprintf(t, sizeof t, "%u", m->d.n_caught);
            text(l, c, &lv_font_palm_bold_2x, 0x000000, LV_TEXT_ALIGN_LEFT, x0 + 7, y0 + 37, x1, t);
            snprintf(t, sizeof t, "of %u", m->d.n);
            text(l, c, &lv_font_palm, 0x000000, LV_TEXT_ALIGN_RIGHT, x0, y0 + 51, x1 - 7, t);
            text(l, c, &lv_font_palm_bold, 0x000000, LV_TEXT_ALIGN_LEFT, x0 + 7, y0 + 75, x1, "SEEN");
            snprintf(t, sizeof t, "%u", m->d.n_seen);
            text(l, c, &lv_font_palm_bold_2x, 0x000000, LV_TEXT_ALIGN_LEFT, x0 + 7, y0 + 89, x1, t);
        }
        return;
    }
    if(!m->game_ok) return;
    if(m->view == V_LIST){
        for(int r = 0; r < ROWS; r++){
            int p = m->row_pos[r], y = TV_Y0 + 4 + r * ROW_H + 5;
            if(p < 0) continue;
            snprintf(t, sizeof t, "%03d", p + 1);
            text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, 19, y, 44, t);
            text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, 74, y, 200, m->d.state[p] == DX_CAUGHT ? m->row_name[r] : "???");
        }
        return;
    }
    int st = m->d.state[m->entry], tx = 147;
    snprintf(t, sizeof t, "No. %03d", m->entry + 1);
    text(l, c, &lv_font_palm_bold, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx, 74, TV_X1 - 2, t);
    text(l, c, &lv_font_palm_bold, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx, 90, TV_X1 - 2, st == DX_CAUGHT ? m->name : "???");
    if(st == DX_NEVER) text(l, c, &lv_font_palm_bold_2x, C_TV_INK, LV_TEXT_ALIGN_CENTER, TV_X0, 115, TV_X0 + 127, "?");
    if(st != DX_CAUGHT){
        text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx, 108, TV_X1 - 2, st == DX_SEEN ? "seen" : "never seen");
        return;
    }
    /* the type chips: side by side if they fit, else one above the other */
    char up[2][16];
    int wide[2] = { 0, 0 }, total = 0;
    for(int i = 0; i < m->n_types; i++){
        lv_point_t sz;
        snprintf(up[i], sizeof up[i], "%s", m->type[i]);
        for(char *p = up[i]; *p; p++) *p = (char)toupper((unsigned char)*p);
        lv_text_get_size(&sz, up[i], &lv_font_palm_bold, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        wide[i] = sz.x + 7;
        total += wide[i] + (i ? 2 : 0);
    }
    int stacked = tx + total - 1 > TV_X1 - 2;
    for(int i = 0; i < m->n_types; i++){
        int x = !stacked && i ? tx + wide[0] + 2 : tx, y = stacked ? 105 + i * 14 : 108, h = stacked ? 14 : 16;
        box(l, c, x, y, x + wide[i] - 1, y + h - 1, type_colour(m->type[i]), 0, 0);
        text(l, c, &lv_font_palm_bold, 0xFFFFFF, LV_TEXT_ALIGN_LEFT, x + 4, y + (stacked ? 0 : 1), x + wide[i], up[i]);
    }
    if(m->next[0]){
        int y = stacked ? 133 : 128;
        snprintf(t, sizeof t, "Lv %u to", m->evolve_at);
        text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx, y, TV_X1 - 2, t);
        text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx, y + (stacked ? 12 : 14), TV_X1 - 2, m->next);
    }
    text(l, c, &lv_font_palm, C_TV_INK, LV_TEXT_ALIGN_LEFT, tx + 13, 158, TV_X1 - 2, "caught");
    text(l, c, &lv_font_palm, C_TV_DIM, LV_TEXT_ALIGN_LEFT, tx, 176, TV_X1 - 2, m->side ? "Back" : "Front");
}

static void key_draw(Dx *m, lv_layer_t *l, const lv_area_t *c, int k){
    int x0 = KEYS[k].x0, x1 = KEYS[k].x1, down = m->pressed == k;
    uint32_t face = down ? 0xFFFFFF : C_KEY, ink = down ? C_KEY : 0xFFFFFF;
    box(l, c, x0, KEY_Y0, x1, KEY_Y1, face, 3, 1);
    if(!down) box(l, c, x0 + 2, KEY_Y0 + 2, x1 - 2, KEY_Y0 + 2, C_KEY_HI, 0, 0);
    if(k == K_LIST || k == K_TALK){
        text(l, c, &lv_font_palm_bold, ink, LV_TEXT_ALIGN_CENTER, x0, 296, x1, k == K_LIST ? "List" : "Talk");
        return;
    }
    if(c->y2 < 296 || c->y1 > 310) return;
    int cx = (x0 + x1) / 2, cy = 303, dir = k == K_NEXT ? 1 : -1;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3      /* the sim's 9.2 */
    t.bg_color = lv_color_hex(ink);
#else
    t.color = lv_color_hex(ink);
#endif
    t.p[0].x = cx + dir * 4; t.p[0].y = cy;
    t.p[1].x = cx - dir * 3; t.p[1].y = cy - 6;
    t.p[2].x = cx - dir * 3; t.p[2].y = cy + 6;
    lv_draw_triangle(l, &t);
}

static void draw_cb(lv_event_t *e){
    Dx *m = g_dx;
    if(!m) return;
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t c = l->_clip_area;
    /* the case's parts: nothing here depends on what's on the screens */
    box(l, &c, 0, 24, 239, 24, C_RED_HI, 0, 0);
    box(l, &c, 0, 56, 239, 56, C_RED_HI, 0, 0);
    box(l, &c, 5, 27, 31, 53, 0xFFFFFF, LV_RADIUS_CIRCLE, 1);    /* the lens */
    box(l, &c, 9, 31, 27, 49, 0x2E7BD6, LV_RADIUS_CIRCLE, 1);
    box(l, &c, 13, 34, 18, 39, 0xBFE4FF, LV_RADIUS_CIRCLE, 0);
    box(l, &c, 38, 36, 45, 43, 0xD62929, LV_RADIUS_CIRCLE, 1);   /* three lights */
    box(l, &c, 49, 36, 56, 43, 0xE8C020, LV_RADIUS_CIRCLE, 1);
    box(l, &c, 60, 36, 67, 43, 0x38B048, LV_RADIUS_CIRCLE, 1);
    box(l, &c, 6, 60, 233, 206, C_GREY, 5, 1);                   /* the upper bezel */
    box(l, &c, TV_X0 - 1, TV_Y0 - 1, TV_X1 + 1, TV_Y1 + 1, C_WELL, TV_RADIUS + 1, 0);   /* its well */
    box(l, &c, 26, 197, 32, 203, 0xD62929, 0, 1);                /* its light */
    box(l, &c, 6, 212, 233, 286, C_GREEN, 4, 1);                 /* the lower screen */

    /* the upper screen: one image, a band at a time; crt.c does the rest */
    lv_area_t a = c;
    if(a.x1 < TV_X0) a.x1 = TV_X0;
    if(a.y1 < TV_Y0) a.y1 = TV_Y0;
    if(a.x2 > TV_X1) a.x2 = TV_X1;
    if(a.y2 > TV_Y1) a.y2 = TV_Y1;
    if(a.x1 <= a.x2 && a.y1 <= a.y2){
        if((a.y2 - a.y1 + 1) * (a.x2 - a.x1 + 1) > BAND_PX) a.y2 = a.y1 + BAND_PX / (a.x2 - a.x1 + 1) - 1;
        Rect r = { a.x1 - TV_X0, a.y1 - TV_Y0, a.x2 - TV_X0, a.y2 - TV_Y0 };
        int bw = r.x2 - r.x1 + 1, bh = r.y2 - r.y1 + 1;
        compose(m, &r, bw);
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
        lv_draw_image(l, &d, &a);
        upper_text(m, l, &c);
    }

    /* the lower screen */
    int y = LCD_Y;
    if(m->head[0]){
        text(l, &c, &lv_font_palm_bold, C_INK, LV_TEXT_ALIGN_LEFT, LCD_X, y, LCD_X + LCD_W_ - 1, m->head);
        y += lv_font_palm_bold.line_height;
    }
    if(m->body[0] && y <= c.y2 && 284 >= c.y1){
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.color = lv_color_hex(C_INK);
        d.font = &lv_font_palm;
        d.text = m->body;
        lv_area_t k = { LCD_X, y, LCD_X + LCD_W_ - 1, 284 };
        lv_draw_label(l, &d, &k);
    }
    for(int i = 0; i < K_COUNT; i++) key_draw(m, l, &c, i);
}

/* ---- the keys and taps (DEX_PLAN.md 2.2) ---- */

static int key_at(int x, int y){
    if(y < KEY_Y0 - 4) return K_NONE;
    for(int k = 0; k < K_COUNT; k++) if(x >= KEYS[k].x0 - 2 && x <= KEYS[k].x1 + 2) return k;
    return K_NONE;
}

static void key_press(Dx *m, int k){
    if(!m->game_ok) return;
    int was_speaking = m->speaking;
    m->speaking = 0;
    switch(k){
    case K_PREV:
    case K_NEXT: {
        int d = k == K_NEXT ? 1 : -1;
        if(m->view == V_LIST){
            int pages = count_pages(m);
            m->page = (uint16_t)((m->page + pages + d) % pages);
        } else {
            m->entry = (int16_t)((m->entry + m->d.n + d) % m->d.n);
            m->side = 0;
        }
        break;
    }
    case K_LIST:
        if(m->view == V_ENTRY || was_speaking){
            if(m->view == V_ENTRY){
                /* back at this critter's page; caught-only is dropped if it isn't there */
                int at = -1;
                if(m->only_caught) for(int q = 0; q < m->d.n_caught; q++) if(m->d.caught[q] == m->entry) at = q;
                if(m->only_caught && at < 0) m->only_caught = 0;
                if(at < 0) at = m->entry;
                m->page = (uint16_t)(at / ROWS);
            }
            m->view = V_LIST;
        } else {
            m->only_caught = !m->only_caught;
            m->page = 0;
        }
        break;
    case K_TALK:
        if(!was_speaking){
            if(m->view == V_ENTRY && !m->has_sprite) load_entry(m);
            speak(m);
        }
        break;
    }
    show(m);
}

static void screen_tap(Dx *m, int x, int y){
    if(!m->game_ok) return;
    if(m->speaking){ m->speaking = 0; show(m); return; }
    if(x < TV_X0 || x > TV_X1 || y < TV_Y0 || y > TV_Y1) return;
    if(m->view == V_LIST){
        if(x >= TV_X0 + 201) return;      /* the scroll bar: ◀ and ▶ turn the pages */
        int r = (y - TV_Y0 - 4) / ROW_H;
        if(y - TV_Y0 < 4 || r >= ROWS || m->row_pos[r] < 0) return;
        m->entry = m->row_pos[r];
        m->view = V_ENTRY;
        m->side = 0;
        show(m);
    }
    else if(x < TV_X0 + 128 && m->d.state[m->entry] != DX_NEVER){
        m->side = !m->side;
        lv_area_t t = area_tv();
        lv_obj_invalidate_area(m->obj, &t);
    }
}

static void press_cb(lv_event_t *e){
    Dx *m = g_dx;
    if(!m) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if(in) lv_indev_get_point(in, &p);
    lv_area_t keys = { 0, KEY_Y0 - 2, 239, KEY_Y1 + 2 };
    if(code == LV_EVENT_PRESSED){
        m->pressed = (int16_t)key_at(p.x, p.y);
        if(m->pressed != K_NONE) lv_obj_invalidate_area(m->obj, &keys);
    }
    else if(code == LV_EVENT_RELEASED){
        int k = m->pressed;
        m->pressed = K_NONE;
        if(k != K_NONE){
            lv_obj_invalidate_area(m->obj, &keys);
            if(key_at(p.x, p.y) == k) key_press(m, k);
        }
        else screen_tap(m, p.x, p.y);
    }
    else if(code == LV_EVENT_PRESS_LOST && m->pressed != K_NONE){
        m->pressed = K_NONE;
        lv_obj_invalidate_area(m->obj, &keys);
    }
}

/* ---- opening and closing ---- */

void tv_strength_load(void){
    FILE *f = fopen(TV_FILE, "rb");
    if(!f) return;
    int c = fgetc(f);
    fclose(f);
    if(c >= '0' && c < '0' + CRT_NSTRENGTHS) crt_set_strength(c - '0');
}

void tv_strength_next(void){
    /* Light, Standard, Strong, Off, Light... */
    crt_set_strength((crt_strength() + 1) % CRT_NSTRENGTHS);
    mkdir("/sdcard/games", 0777);
    FILE *f = fopen(TV_FILE, "wb");
    if(f){ fputc('0' + crt_strength(), f); fclose(f); }
    lv_obj_invalidate(lv_screen_active());
}

void dex_ui_show(lv_obj_t *parent){
    if(g_dx) dex_ui_close();
    Dx *m = calloc(1, sizeof *m);
    uint16_t *band = m ? malloc(BAND_PX * sizeof *band) : NULL;
    if(!band){
        free(m);
        lv_obj_t *t = lv_label_create(parent);
        lv_obj_set_width(t, 220);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_label_set_text(t, "There isn't enough memory free for the Dex just now (a sync uses a lot). Try again in a moment.");
        lv_obj_center(t);
        return;
    }
    m->band = band;
    m->pressed = K_NONE;
    g_dx = m;
    int r = dx_open(&m->d, DEX_FILE, DEX_SAVE);
    m->game_ok = !r;
    if(r == MG_EIO) snprintf(m->why, sizeof m->why, "Critters' game file isn't on the card. The web flasher can install it.");
    else if(r == MG_EVERSION) snprintf(m->why, sizeof m->why, "This game file needs a newer firmware.");
    else if(r) snprintf(m->why, sizeof m->why, "The game file is damaged.");

    m->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(m->obj);
    lv_obj_set_size(m->obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(m->obj, lv_color_hex(C_RED), 0);
    lv_obj_set_style_bg_opa(m->obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(m->obj, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(m->obj, press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(m->obj, press_cb, LV_EVENT_PRESS_LOST, NULL);

    tv_strength_load();
    crt_on(TV_X0, TV_Y0, TV_X1, TV_Y1, TV_RADIUS, rgb(C_WELL));
    m->view = V_LIST;
    if(m->game_ok) load_list(m);
    speak(m);                       /* the opening: his line, over the list */
    show(m);
}

void dex_ui_close(void){
    Dx *m = g_dx;
    if(!m) return;
    crt_off();
    if(m->game_ok) dx_close(&m->d);
    g_dx = NULL;
    free(m->band);
    free(m);
}

int dex_ui_open(void){ return g_dx != NULL; }

#ifdef UI_DEVTOOLS
static void mark_list(MpWorld *w, MonGame *g, const char *s, int caught){
    while(s && *s && *s != '-'){
        uint16_t pos;
        if(!mg_find_mon(g, (uint32_t)atoi(s), &pos)) mp_mark_mon(w, pos, caught);
        s = strchr(s, ',');
        if(s) s++;
    }
}

void dex_ui_test_save(const char *seen, const char *caught){
    MonGame *g = malloc(sizeof *g);
    MpWorld *w = calloc(1, sizeof *w);
    uint8_t *head = malloc(MG_HEAD_MAX);
    if(g && w && head && !mg_open(g, DEX_FILE)){
        if(!mp_new(w, g, head, 1, g->n_starters ? g->starters[0] : 0)){
            memset(w->seen, 0, sizeof w->seen);
            memset(w->caught, 0, sizeof w->caught);
            mark_list(w, g, seen, 0);
            mark_list(w, g, caught, 1);
            mp_save(w, DEX_SAVE);
        }
        mg_close(g);
    }
    free(g); free(w); free(head);
}

void dex_ui_test_clear(void){ remove(DEX_SAVE); }
#endif
