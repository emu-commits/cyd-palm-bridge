/* ui.c -- Palm-style app shell (LVGL): launcher + navigation.
 *
 * Layout (portrait 240x320):
 *   title bar   0..24    navy, home button (left) + current-screen title
 *   content    24..208   the active view (launcher list, or an app screen)
 *   Graffiti  208..320   input strip (letters | numbers) -- wired up in U6
 *
 * U3.2 launcher lists the classic Palm apps; U3.3 navigation opens a placeholder
 * per app and returns home. Real data views arrive in U4, authentic fonts/icons
 * in U3a.
 */
#include "ui.h"
#include "display.h"      /* LCD_W, PDA_H, GRAFFITI_H */
#include "lvgl.h"

#define TITLE_H     24
#define COL_TITLE   lv_color_hex(0x000080)   /* navy title bar   */
#define COL_BODY    lv_color_hex(0xC6C6C6)   /* light gray body  */
#define COL_GRAF    lv_color_hex(0x808080)   /* graffiti strip   */
#define COL_LINE    lv_color_hex(0x404040)

static lv_obj_t *content;      /* the swappable view area */
static lv_obj_t *title_lbl;

static const char *APPS[] = { "Date Book", "Address", "To Do List", "Memo Pad", "HotSync" };
#define NAPPS ((int)(sizeof(APPS)/sizeof(APPS[0])))

static void show_launcher(void);
static void show_app(const char *name);

/* a borderless, non-scrolling panel with a solid fill */
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t col){
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, col, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void home_cb(lv_event_t *e){ (void)e; show_launcher(); }
static void app_cb(lv_event_t *e){ show_app((const char *)lv_event_get_user_data(e)); }

static void show_app(const char *name){
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, name);
    lv_obj_t *l = lv_label_create(content);
    lv_label_set_text_fmt(l, "%s\n\n(coming soon)", name);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

static void show_launcher(void){
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Applications");

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    for(int i=0;i<NAPPS;i++){
        lv_obj_t *b = lv_list_add_button(list, NULL, APPS[i]);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_add_event_cb(b, app_cb, LV_EVENT_CLICKED, (void *)APPS[i]);
    }
}

void ui_init(void){
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BODY, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* title bar: home button (left) + title label */
    lv_obj_t *bar = panel(scr, 0, 0, LCD_W, TITLE_H, COL_TITLE);
    lv_obj_t *home = lv_button_create(bar);
    lv_obj_set_size(home, 30, TITLE_H);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(home, 0, 0);
    lv_obj_set_style_bg_color(home, COL_TITLE, 0);
    lv_obj_t *hi = lv_label_create(home);
    lv_label_set_text(hi, LV_SYMBOL_HOME);
    lv_obj_center(hi);
    lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);

    title_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 38, 0);

    /* content area (swappable views) */
    content = panel(scr, 0, TITLE_H, LCD_W, PDA_H - TITLE_H, COL_BODY);

    /* Graffiti input strip with the letters | numbers split */
    lv_obj_t *graf = panel(scr, 0, PDA_H, LCD_W, GRAFFITI_H, COL_GRAF);
    lv_obj_t *sep = panel(graf, LCD_W*3/5, 0, 2, GRAFFITI_H, COL_LINE);
    (void)sep;
    lv_obj_t *gl = lv_label_create(graf);
    lv_label_set_text(gl, "abc");
    lv_obj_align(gl, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t *gr = lv_label_create(graf);
    lv_label_set_text(gr, "123");
    lv_obj_align(gr, LV_ALIGN_RIGHT_MID, -20, 0);

    show_launcher();
}
