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
#include "data.h"
#include "lv_font_palm.h" /* authentic PalmOS system fonts */
#include "palm_icons.h"   /* Palm app launcher icons */
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#define TITLE_H     24
#define COL_TITLE   lv_color_hex(0xFFFFFF)   /* white title bar (Palm) */
#define COL_BODY    lv_color_hex(0xFFFFFF)   /* white app body   */
#define COL_GRAF    lv_color_hex(0xD6D6D6)   /* graffiti strip   */
#define COL_LINE    lv_color_hex(0x000000)   /* black rules      */

static lv_obj_t *content;      /* the swappable view area */
static lv_obj_t *title_lbl;

static const char *APPS[] = { "Date Book", "Address", "To Do List", "Memo Pad", "HotSync" };
/* authentic Palm app launcher icons (from PumpkinOS) */
static const lv_image_dsc_t *APP_ICONS[] = { &icon_datebook, &icon_address,
                                             &icon_todo, &icon_memo, &icon_hotsync };
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

/* apps backed by real data (name -> app id + iterator) */
typedef struct { const char *name; int app; void (*iter)(data_row_cb, void *); } AppDef;
static const AppDef APPDEFS[] = {
    { "Date Book",  APP_CAL,  data_datebook },
    { "Address",    APP_ADDR, data_address  },
    { "To Do List", APP_TODO, data_todo     },
};
#define NAPPDEFS ((int)(sizeof(APPDEFS)/sizeof(APPDEFS[0])))
static const AppDef *cur_app;   /* the data app whose list/detail is showing */
static uint32_t cur_uid;        /* the record currently in detail/edit (0 = none) */

/* edit state */
#define KB_H       150
#define FORM_FULL  ((PDA_H - TITLE_H) - 34)                 /* form height, no keyboard */
#define FORM_KB    (LCD_H - KB_H - TITLE_H - 34)            /* form height above keyboard */
static uint32_t edit_uid;
static lv_obj_t *g_kb;                 /* on-screen keyboard (overlay), or NULL */
static lv_obj_t *g_form;               /* scrollable field container */
static lv_obj_t *g_fields[8];          /* edit-form textareas */
static int g_nfields;

static void list_view(const AppDef *ad);
static void show_detail(uint32_t uid);
static void show_edit(uint32_t uid);

/* the keyboard lives on the screen (overlay), so it survives lv_obj_clean(content);
 * drop it whenever we navigate away from an edit form. */
static void kill_kb(void){ if(g_kb){ lv_obj_del(g_kb); g_kb=NULL; } g_form=NULL; }

static void row_cb(lv_event_t *e){
    show_detail((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

/* add one tappable row to the active list */
static void add_row(uint32_t uid, const char *primary, const char *secondary, void *ctx){
    lv_obj_t *list = (lv_obj_t *)ctx;
    char buf[128];
    if(secondary && secondary[0]) snprintf(buf,sizeof buf,"%s  (%s)",primary,secondary);
    else                          snprintf(buf,sizeof buf,"%s",primary);
    lv_obj_t *b = lv_list_add_button(list, NULL, buf);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_add_event_cb(b, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
}

/* scrolling list of records for one app */
static void list_view(const AppDef *ad){
    kill_kb();
    cur_app = ad;
    cur_uid = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, ad->name);
    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    ad->iter(add_row, list);
    if(lv_obj_get_child_count(list) == 0){
        lv_obj_t *b = lv_list_add_button(list, NULL, "(no records)");
        lv_obj_set_style_radius(b, 0, 0);
    }
}

static void done_cb(lv_event_t *e){ (void)e; if(cur_app) list_view(cur_app); }

static void edit_cb(lv_event_t *e){ show_edit((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }

/* read-only detail for one record (scrollable text + Done / Edit) */
static void show_detail(uint32_t uid){
    if(!cur_app) return;
    cur_uid = uid;
    kill_kb();
    static char buf[720];
    if(!data_detail(cur_app->app, uid, buf, sizeof buf)) snprintf(buf,sizeof buf,"(not found)");

    lv_obj_clean(content);
    int ch = PDA_H - TITLE_H;
    lv_obj_t *box = lv_obj_create(content);       /* scrolls if text overflows */
    lv_obj_set_size(box, LCD_W, ch - 40);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_bg_color(box, COL_BODY, 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LCD_W - 16);
    lv_label_set_text(l, buf);

    lv_obj_t *done = lv_button_create(content);
    lv_obj_set_size(done, 90, 34);
    lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 4, -3);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done"); lv_obj_center(dl);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *edit = lv_button_create(content);
    lv_obj_set_size(edit, 90, 34);
    lv_obj_align(edit, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
    lv_obj_t *el = lv_label_create(edit);
    lv_label_set_text(el, "Edit"); lv_obj_center(el);
    lv_obj_add_event_cb(edit, edit_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
}

/* ------------------------- edit form ------------------------- */
static void ta_click_cb(lv_event_t *e){
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_keyboard_set_textarea(g_kb, ta);
    lv_obj_clear_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    if(g_form){                                   /* shrink viewport above the keyboard, */
        lv_obj_set_height(g_form, FORM_KB);       /* then bring the focused field into view */
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    }
}
static void kb_done_cb(lv_event_t *e){ (void)e;
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    if(g_form) lv_obj_set_height(g_form, FORM_FULL);
}

/* a labeled one-line textarea; advances *y and records the textarea */
static void form_field(lv_obj_t *form, const char *label, const char *val, int maxlen, int *y){
    lv_obj_t *lb = lv_label_create(form);
    lv_label_set_text(lb, label);
    lv_obj_set_pos(lb, 2, *y);
    lv_obj_t *ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxlen);
    lv_textarea_set_text(ta, val ? val : "");
    lv_obj_set_width(ta, LCD_W - 16);
    lv_obj_set_pos(ta, 2, *y + 15);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    g_fields[g_nfields++] = ta;
    *y += 52;
}

static const char *fv(int i){ return lv_textarea_get_text(g_fields[i]); }
static const char *iv(Addr *a, const char *s){ return (s && s[0]) ? AddrIntern(a, s) : NULL; }

static void save_cb(lv_event_t *e){
    (void)e;
    if(cur_app->app == APP_CAL){
        Appt a; if(!data_get_cal(edit_uid,&a)) memset(&a,0,sizeof a);
        snprintf(a.description,sizeof a.description,"%s",fv(0));
        snprintf(a.note,sizeof a.note,"%s",fv(1));
        data_save_cal(edit_uid,&a);
    } else if(cur_app->app == APP_TODO){
        Todo t; if(!data_get_todo(edit_uid,&t)) memset(&t,0,sizeof t);
        snprintf(t.description,sizeof t.description,"%s",fv(0));
        snprintf(t.note,sizeof t.note,"%s",fv(1));
        data_save_todo(edit_uid,&t);
    } else if(cur_app->app == APP_ADDR){
        Addr old; int have=data_get_addr(edit_uid,&old);
        Addr a; memset(&a,0,sizeof a);
        a.fields[F_name]=iv(&a,fv(0));
        a.fields[F_firstName]=iv(&a,fv(1));
        a.fields[F_company]=iv(&a,fv(2));
        a.fields[F_phone1]=iv(&a,fv(3)); a.phoneLabel[0]=have?old.phoneLabel[0]:workLabel;
        a.fields[F_note]=iv(&a,fv(4));
        if(have){   /* preserve fields the form doesn't expose */
            for(int k=1;k<5;k++) if(old.fields[F_phone1+k]){ a.fields[F_phone1+k]=iv(&a,old.fields[F_phone1+k]); a.phoneLabel[k]=old.phoneLabel[k]; }
            static const int keep[]={F_address,F_city,F_state,F_zip,F_country,F_title,F_custom1,F_custom2,F_custom3,F_custom4};
            for(unsigned k=0;k<sizeof keep/sizeof keep[0];k++) if(old.fields[keep[k]]) a.fields[keep[k]]=iv(&a,old.fields[keep[k]]);
            a.displayPhone=old.displayPhone;
        }
        data_save_addr(edit_uid,&a);
    }
    list_view(cur_app);
}
static void cancel_cb(lv_event_t *e){ (void)e; list_view(cur_app); }

static void show_edit(uint32_t uid){
    if(!cur_app) return;
    edit_uid = uid; cur_uid = uid; g_nfields = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, uid ? "Edit" : "New");

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 74, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = lv_button_create(content);
    lv_obj_set_size(save, 74, 28); lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *sl=lv_label_create(save); lv_label_set_text(sl,"Save"); lv_obj_center(sl);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);

    g_form = lv_obj_create(content);
    lv_obj_t *form = g_form;
    lv_obj_set_size(form, LCD_W, FORM_FULL);
    lv_obj_set_pos(form, 0, 34);
    lv_obj_set_style_radius(form, 0, 0);
    lv_obj_set_style_border_width(form, 0, 0);
    lv_obj_set_style_bg_color(form, COL_BODY, 0);

    int y = 2;
    if(cur_app->app == APP_CAL){
        Appt a; if(!data_get_cal(uid,&a)) memset(&a,0,sizeof a);
        form_field(form,"Description",a.description,255,&y);
        form_field(form,"Note",a.note,500,&y);
    } else if(cur_app->app == APP_TODO){
        Todo t; if(!data_get_todo(uid,&t)) memset(&t,0,sizeof t);
        form_field(form,"Description",t.description,255,&y);
        form_field(form,"Note",t.note,500,&y);
    } else if(cur_app->app == APP_ADDR){
        Addr a; if(!data_get_addr(uid,&a)) memset(&a,0,sizeof a);
        form_field(form,"Last",a.fields[F_name],40,&y);
        form_field(form,"First",a.fields[F_firstName],40,&y);
        form_field(form,"Company",a.fields[F_company],60,&y);
        form_field(form,"Phone",a.fields[F_phone1],40,&y);
        form_field(form,"Note",a.fields[F_note],200,&y);
    }

    g_kb = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(g_kb, LCD_W, 150);
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_kb, kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_kb, kb_done_cb, LV_EVENT_CANCEL, NULL);
}

static void show_app(const char *name){
    for(int i=0;i<NAPPDEFS;i++)
        if(!strcmp(name, APPDEFS[i].name)){ list_view(&APPDEFS[i]); return; }
    /* Memo Pad (no codec yet) + HotSync (U7) -> placeholder */
    cur_app = NULL;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, name);
    lv_obj_t *l = lv_label_create(content);
    lv_label_set_text_fmt(l, "%s\n\n(coming soon)", name);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

static void show_launcher(void){
    kill_kb();
    cur_app = NULL;
    cur_uid = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Applications");

    /* Palm Application Launcher = an icon grid (not a list) */
    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_bg_color(grid, COL_BODY, 0);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    for(int i=0;i<NAPPS;i++){
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_size(cell, 68, 52);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(cell, 2, 0);
        lv_obj_set_style_pad_row(cell, 3, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, app_cb, LV_EVENT_CLICKED, (void *)APPS[i]);

        lv_obj_t *img = lv_image_create(cell);
        lv_image_set_src(img, APP_ICONS[i]);                       /* 1x (crisp) */
        lv_obj_set_style_image_recolor(img, COL_LINE, 0);          /* A8 mask -> black */
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, APPS[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_palm, 0);
    }
}

/* ------------------------- F1: menu bar ------------------------- */
static lv_obj_t *g_menu;   /* menu overlay root, or NULL */
static void menu_close(void){ if(g_menu){ lv_obj_del(g_menu); g_menu=NULL; } }
static void menu_backdrop_cb(lv_event_t *e){ (void)e; menu_close(); }

static void act_new(lv_event_t *e){ (void)e; const AppDef *a=cur_app; menu_close(); if(a){ cur_app=a; show_edit(0); } }
static void act_delete(lv_event_t *e){ (void)e;
    const AppDef *a=cur_app; uint32_t u=cur_uid; menu_close();
    if(a && u){ data_delete(a->app, u); list_view(a); }
}
static void act_categories(lv_event_t *e){ (void)e; menu_close(); /* TODO F2 */ }
static void act_about(lv_event_t *e){ (void)e;
    menu_close();
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, "CYD Palm");
    lv_msgbox_add_text(mb, "A PalmOS-style PDA that\nsyncs to iCloud.\n\nv0.1");
    lv_msgbox_add_close_button(mb);
}

static void menu_item(lv_obj_t *par, const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}
static void menu_header(lv_obj_t *par, const char *txt){
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_palm_bold, 0);
    lv_obj_set_style_pad_top(l, 3, 0);
}

/* Palm menu: tap Menu (silkscreen) -> pull-down of the context's commands,
 * grouped by Palm's menu categories (Record / Options). */
static void menu_open(void){
    if(g_menu) return;
    g_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_menu, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_menu, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_menu, 0, 0);
    lv_obj_set_style_radius(g_menu, 0, 0);
    lv_obj_set_style_pad_all(g_menu, 0, 0);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_menu, menu_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_menu);
    lv_obj_set_width(panel, 150);
    lv_obj_set_style_max_height(panel, LCD_H - 20, 0);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 4, TITLE_H);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* absorb clicks (don't close) */

    if(cur_app){
        menu_header(panel, "Record");
        menu_item(panel, "New", act_new);
        if(cur_uid) menu_item(panel, "Delete", act_delete);
    }
    menu_header(panel, "Options");
    menu_item(panel, "Categories", act_categories);
    menu_item(panel, "About", act_about);
}

/* silkscreen buttons */
static void menu_cb(lv_event_t *e){ (void)e; menu_open(); }
static void find_cb(lv_event_t *e){ (void)e; /* TODO global Find */ }
static void calc_cb(lv_event_t *e){ (void)e; /* TODO calculator */ }

/* a small bordered silkscreen button with a recolored icon */
static lv_obj_t *mk_silk(lv_obj_t *par, const lv_image_dsc_t *ic, lv_align_t al,
                         int xo, int yo, lv_event_cb_t cb){
    lv_obj_t *b = lv_obj_create(par);
    lv_obj_set_size(b, 30, 30);
    lv_obj_align(b, al, xo, yo);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_LINE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *im = lv_image_create(b);
    lv_image_set_src(im, ic);
    lv_obj_center(im);
    lv_obj_set_style_image_recolor(im, COL_LINE, 0);
    lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
    return b;
}

void ui_init(void){
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BODY, 0);
    lv_obj_set_style_text_font(scr, &lv_font_palm, 0);   /* authentic Palm font, inherited */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* title bar: app title + category picker (F2), black rule underneath (Palm).
     * Home/Menu live on the silkscreen buttons below, not here. */
    lv_obj_t *bar = panel(scr, 0, 0, LCD_W, TITLE_H, COL_TITLE);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, COL_LINE, 0);

    title_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(title_lbl, COL_LINE, 0);   /* black on white */
    lv_obj_set_style_text_font(title_lbl, &lv_font_palm_bold, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 6, 0);

    /* content area (swappable views) */
    content = panel(scr, 0, TITLE_H, LCD_W, PDA_H - TITLE_H, COL_BODY);

    /* Graffiti strip: silkscreen buttons flank the writing area, Palm-style:
     * [Home][Menu] ... abc | 123 ... [Find][Calc] */
    lv_obj_t *graf = panel(scr, 0, PDA_H, LCD_W, GRAFFITI_H, COL_GRAF);
    mk_silk(graf, &silk_home, LV_ALIGN_TOP_LEFT,     3,  3, home_cb);
    mk_silk(graf, &silk_menu, LV_ALIGN_BOTTOM_LEFT,  3, -3, menu_cb);
    mk_silk(graf, &silk_find, LV_ALIGN_TOP_RIGHT,   -3,  3, find_cb);
    mk_silk(graf, &silk_calc, LV_ALIGN_BOTTOM_RIGHT,-3, -3, calc_cb);

    lv_obj_t *sep = panel(graf, LCD_W/2, 6, 2, GRAFFITI_H-12, COL_LINE);
    (void)sep;
    lv_obj_t *gl = lv_label_create(graf);
    lv_label_set_text(gl, "abc");
    lv_obj_align(gl, LV_ALIGN_CENTER, -28, 0);
    lv_obj_t *gr = lv_label_create(graf);
    lv_label_set_text(gr, "123");
    lv_obj_align(gr, LV_ALIGN_CENTER, 28, 0);

    show_launcher();
}
