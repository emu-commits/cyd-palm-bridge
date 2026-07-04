/* ui.c -- Palm-style app shell (LVGL). U3.1: integration proof (title bar,
 * a button that counts taps, and the Graffiti strip zone). The real launcher +
 * navigation land in U3.2/U3.3; authentic Palm fonts/icons in U3a. */
#include "ui.h"
#include "display.h"      /* LCD_W, PDA_H, GRAFFITI_H */
#include "lvgl.h"

#define COL_TITLE   lv_color_hex(0x000080)   /* Palm-ish navy title bar */
#define COL_BODY    lv_color_hex(0xC6C6C6)   /* light gray app body     */
#define COL_GRAF    lv_color_hex(0x808080)   /* graffiti strip          */

static void tap_cb(lv_event_t *e){
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    static int n = 0;
    lv_label_set_text_fmt(lbl, "Tapped %d", ++n);
}

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

void ui_init(void){
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BODY, 0);

    /* title bar */
    lv_obj_t *bar = panel(scr, 0, 0, LCD_W, 24, COL_TITLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "CYD Palm");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_center(title);

    /* a tappable button to prove touch flows through LVGL */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 52);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Tap me");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, tap_cb, LV_EVENT_CLICKED, bl);

    /* Graffiti input strip with the letters | numbers split */
    lv_obj_t *graf = panel(scr, 0, PDA_H, LCD_W, GRAFFITI_H, COL_GRAF);
    lv_obj_t *gl = lv_label_create(graf);
    lv_label_set_text(gl, "abc  |  123");
    lv_obj_center(gl);
}
