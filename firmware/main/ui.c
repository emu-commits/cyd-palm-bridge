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
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

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

/* apps backed by real data (name -> app id + iterator) */
typedef struct { const char *name; int app; void (*iter)(data_row_cb, void *); } AppDef;
static const AppDef APPDEFS[] = {
    { "Date Book",  APP_CAL,  data_datebook },
    { "Address",    APP_ADDR, data_address  },
    { "To Do List", APP_TODO, data_todo     },
};
#define NAPPDEFS ((int)(sizeof(APPDEFS)/sizeof(APPDEFS[0])))
static const AppDef *cur_app;   /* the data app whose list/detail is showing */

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
    edit_uid = uid; g_nfields = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Edit");

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
