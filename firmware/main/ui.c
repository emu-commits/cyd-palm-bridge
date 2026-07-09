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
#include "hotsync.h"
#include "graffiti.h"
#include "calc.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

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
    { "Memo Pad",   APP_MEMO, data_memo     },
};
#define NAPPDEFS ((int)(sizeof(APPDEFS)/sizeof(APPDEFS[0])))
static const AppDef *cur_app;   /* the data app whose list/detail is showing */
static uint32_t cur_uid;        /* the record currently in detail/edit (0 = none) */

/* edit state */
static int edit_cat;            /* category chosen for the record being edited */
#define FORM_FULL  ((PDA_H - TITLE_H) - 34)                 /* form height (Graffiti is the input) */
static uint32_t edit_uid;
static lv_obj_t *g_form;               /* scrollable field container */
static lv_obj_t *edit_cat_lbl;         /* label on the edit-form category trigger */
static lv_obj_t *g_fields[8];          /* edit-form textareas */
static int g_nfields;
static lv_obj_t *active_ta;            /* last-focused textarea (Graffiti target) */

static void list_view(const AppDef *ad);
static void show_detail(uint32_t uid);
static void show_edit(uint32_t uid);
static void update_cat_trigger(void);
static void cat_trigger_cb(lv_event_t *e);
static void details_open(void);

/* HotSync screen state (status label + polling timer live in `content`) */
static lv_obj_t *hs_status;
static lv_timer_t *hs_timer;
static void kill_hs(void){ if(hs_timer){ lv_timer_delete(hs_timer); hs_timer=NULL; } hs_status=NULL; }

/* tear down per-form / per-screen state when navigating away. Text entry is
 * Graffiti-only (no on-screen keyboard), so there's no overlay to drop here. */
static void kill_kb(void){ g_form=NULL; active_ta=NULL; edit_cat_lbl=NULL; kill_hs(); }

static void row_cb(lv_event_t *e){
    show_detail((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

/* add one tappable row to the active list. To Do rows keep the "[x]"/"[ ]"
 * prefix from the data layer to show completion; tapping opens the record (where
 * the done state is toggled). Using plain list buttons for every app -- custom
 * checkbox rows made LVGL's compositor loop forever behind the menu overlay. */
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
    update_cat_trigger();
}

static void done_cb(lv_event_t *e){ (void)e; if(cur_app) list_view(cur_app); }

static void edit_cb(lv_event_t *e){ show_edit((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }

/* ToDo detail: flip completed and redraw the detail so the status updates */
static void todo_toggle_detail_cb(lv_event_t *e){
    uint32_t u = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    data_toggle_todo(u);
    show_detail(u);
}

/* ---- delete a record (with confirmation, like PalmOS) ----
 * PalmOS never deletes without a "Delete <record>?" alert; we mirror that so a
 * mis-tap can't destroy data. On confirm the record is removed and the list
 * redraws. (data_delete rewrites the PDB without the record; its uid stays in
 * the sync map, so the next HotSync detects the deletion and pushes it up.) */
static uint32_t del_uid;
static lv_obj_t *g_confirm;
static void confirm_close(void){ if(g_confirm){ lv_obj_del(g_confirm); g_confirm=NULL; } }
static void confirm_cancel_cb(lv_event_t *e){ (void)e; confirm_close(); }
static void confirm_delete_cb(lv_event_t *e){ (void)e;
    const AppDef *a = cur_app; uint32_t u = del_uid;
    confirm_close();
    if(a && u){ data_delete(a->app, u); cur_uid = 0; list_view(a); }
}
static void ask_delete(uint32_t uid){
    if(g_confirm || !cur_app) return;
    del_uid = uid;
    const char *what = cur_app->app==APP_CAL ? "event"
                     : cur_app->app==APP_ADDR ? "address"
                     : cur_app->app==APP_TODO ? "item" : "memo";

    g_confirm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_confirm, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_confirm, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_confirm, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_confirm, 0, 0);
    lv_obj_set_style_pad_all(g_confirm, 0, 0);
    lv_obj_add_flag(g_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_confirm, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_confirm);
    lv_obj_set_width(panel, 200);
    lv_obj_set_height(panel, 96);   /* fixed: LV_SIZE_CONTENT collapses under the bottom-aligned buttons */
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* absorb clicks */

    lv_obj_t *q = lv_label_create(panel);
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(q, 180);
    lv_label_set_text_fmt(q, "Delete this %s?", what);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(panel);
    lv_obj_set_size(cancel, 82, 30);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel); lv_label_set_text(cl, "Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *del = lv_button_create(panel);
    lv_obj_set_size(del, 82, 30);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *dll = lv_label_create(del); lv_label_set_text(dll, "Delete"); lv_obj_center(dll);
    lv_obj_add_event_cb(del, confirm_delete_cb, LV_EVENT_CLICKED, NULL);
}
static void del_btn_cb(lv_event_t *e){ ask_delete((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }

/* read-only detail for one record (scrollable text + Done / Delete / Edit) */
static void show_detail(uint32_t uid){
    if(!cur_app) return;
    cur_uid = uid;
    kill_kb();
    static char buf[720];
    if(!data_detail(cur_app->app, uid, buf, sizeof buf)) snprintf(buf,sizeof buf,"(not found)");

    lv_obj_clean(content);
    int ch = PDA_H - TITLE_H;
    int istodo = (cur_app->app == APP_TODO);
    /* leave room for the action row (and a second row for ToDo's Mark Done) */
    lv_obj_t *box = lv_obj_create(content);       /* scrolls if text overflows */
    lv_obj_set_size(box, LCD_W, ch - (istodo ? 78 : 40));
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_bg_color(box, COL_BODY, 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LCD_W - 16);
    lv_label_set_text(l, buf);

    /* primary actions, three across: Done | Delete | Edit */
    lv_obj_t *done = lv_button_create(content);
    lv_obj_set_size(done, 72, 34);
    lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 4, -3);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done"); lv_obj_center(dl);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *del = lv_button_create(content);
    lv_obj_set_size(del, 72, 34);
    lv_obj_align(del, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_t *dell = lv_label_create(del);
    lv_label_set_text(dell, "Delete"); lv_obj_center(dell);
    lv_obj_add_event_cb(del, del_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);

    lv_obj_t *edit = lv_button_create(content);
    lv_obj_set_size(edit, 72, 34);
    lv_obj_align(edit, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
    lv_obj_t *el = lv_label_create(edit);
    lv_label_set_text(el, "Edit"); lv_obj_center(el);
    lv_obj_add_event_cb(edit, edit_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);

    /* ToDo: a full-width row above toggles completion (replaces the inline checkbox) */
    if(istodo){
        Todo t; int isdone = data_get_todo(uid,&t) ? t.completed : 0;
        lv_obj_t *mk = lv_button_create(content);
        lv_obj_set_size(mk, LCD_W - 8, 34);
        lv_obj_align(mk, LV_ALIGN_BOTTOM_MID, 0, -41);
        lv_obj_t *ml = lv_label_create(mk);
        lv_label_set_text(ml, isdone ? "Mark Not Done" : "Mark Done"); lv_obj_center(ml);
        lv_obj_add_event_cb(mk, todo_toggle_detail_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)uid);
    }
}

/* ------------------------- edit form ------------------------- */
/* tapping a field just makes it the Graffiti target (and shows its cursor);
 * there is no on-screen keyboard -- all text entry is via the Graffiti strip. */
static void ta_click_cb(lv_event_t *e){
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if(active_ta && active_ta != ta) lv_obj_clear_state(active_ta, LV_STATE_FOCUSED);
    active_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if(g_form) lv_obj_scroll_to_view(ta, LV_ANIM_ON);
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

/* fill a fresh appointment with today's date + the next half hour (Palm default) */
static void default_appt(Appt *a){
    memset(a,0,sizeof *a);
    time_t now=0; time(&now);
    struct tm tmv; localtime_r(&now,&tmv);
    if(tmv.tm_year+1900 < 2024){ tmv.tm_year=2026-1900; tmv.tm_mon=0; tmv.tm_mday=1; tmv.tm_hour=9; tmv.tm_min=0; }
    int h=tmv.tm_hour, m=tmv.tm_min;
    if(m<30) m=30; else { m=0; h=(h+1)%24; }          /* round up to next :00/:30 */
    a->hasTime=1; a->sH=h; a->sM=m; a->eH=(h+1)%24; a->eM=m;
    a->year=tmv.tm_year+1900; a->month=tmv.tm_mon+1; a->day=tmv.tm_mday;
}

static const char *fv(int i){ return lv_textarea_get_text(g_fields[i]); }
static const char *iv(Addr *a, const char *s){ return (s && s[0]) ? AddrIntern(a, s) : NULL; }

static void save_cb(lv_event_t *e){
    (void)e;
    if(cur_app->app == APP_CAL){
        Appt a; if(!data_get_cal(edit_uid,&a)) default_appt(&a);
        snprintf(a.description,sizeof a.description,"%s",fv(0));
        int mo,dd,yy; if(sscanf(fv(1),"%d/%d/%d",&mo,&dd,&yy)==3){ a.month=mo; a.day=dd; a.year=yy; }
        int hh,mm; if(sscanf(fv(2),"%d:%d",&hh,&mm)==2){ a.hasTime=1; a.sH=hh; a.sM=mm; a.eH=(hh+1)%24; a.eM=mm; }
        snprintf(a.note,sizeof a.note,"%s",fv(3));
        data_save_cal(edit_uid,edit_cat,&a);
    } else if(cur_app->app == APP_TODO){
        Todo t; if(!data_get_todo(edit_uid,&t)) memset(&t,0,sizeof t);
        snprintf(t.description,sizeof t.description,"%s",fv(0));
        snprintf(t.note,sizeof t.note,"%s",fv(1));
        data_save_todo(edit_uid,edit_cat,&t);
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
        data_save_addr(edit_uid,edit_cat,&a);
    } else if(cur_app->app == APP_MEMO){
        data_save_memo(edit_uid,edit_cat,fv(0));
    }
    list_view(cur_app);
}
static void cancel_cb(lv_event_t *e){ (void)e; list_view(cur_app); }

static void details_btn_cb(lv_event_t *e){ (void)e; details_open(); }

/* show the record's current category on the edit-form trigger button */
static void set_editcat_label(void){
    if(!edit_cat_lbl) return;
    CatTable t;
    if(cur_app && data_get_categories(cur_app->app,&t) && edit_cat>=0 && t.name[edit_cat][0])
        lv_label_set_text_fmt(edit_cat_lbl, "%s", t.name[edit_cat]);
    else
        lv_label_set_text(edit_cat_lbl, "Unfiled");
}

static void show_edit(uint32_t uid){
    if(!cur_app) return;
    edit_uid = uid; cur_uid = uid; g_nfields = 0;
    int rc = uid ? data_record_category(cur_app->app, uid) : data_get_category();
    edit_cat = rc < 0 ? 0 : rc;   /* new records default to Unfiled / the current filter */
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, uid ? "Edit" : "New");

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 60, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    /* middle button = category trigger (Palm sets a record's category here). Shows
     * the current category; tap to choose. */
    lv_obj_t *det = lv_button_create(content);
    lv_obj_set_size(det, 112, 28); lv_obj_align(det, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_pad_hor(det, 2, 0);
    edit_cat_lbl = lv_label_create(det); lv_obj_center(edit_cat_lbl);
    lv_obj_add_event_cb(det, details_btn_cb, LV_EVENT_CLICKED, NULL);
    set_editcat_label();
    lv_obj_t *save = lv_button_create(content);
    lv_obj_set_size(save, 60, 28); lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -2, 2);
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
        Appt a; if(!data_get_cal(uid,&a)) default_appt(&a);
        char ds[24]; snprintf(ds,sizeof ds,"%d/%d/%d",a.month,a.day,a.year);
        char ts[16]; snprintf(ts,sizeof ts,"%d:%02d",a.sH,a.sM);
        form_field(form,"Description",a.description,255,&y);
        form_field(form,"Date (M/D/YYYY)",ds,16,&y);
        form_field(form,"Time (h:mm)",ts,8,&y);
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
    } else if(cur_app->app == APP_MEMO){
        static char mtext[1200];
        if(!data_get_memo(uid, mtext, sizeof mtext)) mtext[0]=0;
        lv_obj_t *ta = lv_textarea_create(form);       /* one big multi-line field */
        lv_textarea_set_text(ta, mtext);
        lv_textarea_set_max_length(ta, sizeof mtext - 1);
        lv_obj_set_size(ta, LCD_W - 16, FORM_FULL - 8);
        lv_obj_set_pos(ta, 2, 2);
        lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
        g_fields[g_nfields++] = ta;
    }

    /* focus the first field so Graffiti has a target immediately */
    if(g_nfields > 0){ active_ta = g_fields[0]; lv_obj_add_state(g_fields[0], LV_STATE_FOCUSED); }
}

/* U7: HotSync screen (Sync Now + a status line polled from the background task) */
static void hs_tick(lv_timer_t *t){ (void)t; if(hs_status) lv_label_set_text(hs_status, hotsync_status()); }
static void hs_sync_cb(lv_event_t *e){ (void)e; hotsync_start(); }

static void show_hotsync(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "HotSync");
    update_cat_trigger();

    lv_obj_t *btn = lv_button_create(content);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Sync Now");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, hs_sync_cb, LV_EVENT_CLICKED, NULL);

    hs_status = lv_label_create(content);
    lv_label_set_long_mode(hs_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hs_status, LCD_W - 12);
    lv_obj_align(hs_status, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_text_align(hs_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hs_status, hotsync_status());

    hs_timer = lv_timer_create(hs_tick, 400, NULL);
}

static void show_app(const char *name){
    for(int i=0;i<NAPPDEFS;i++)
        if(!strcmp(name, APPDEFS[i].name)){ data_set_category(-1); list_view(&APPDEFS[i]); return; }
    if(!strcmp(name, "HotSync")){ show_hotsync(); return; }
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
    update_cat_trigger();   /* hides it (no data app) */

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
    uint32_t u=cur_uid; menu_close();
    if(u) ask_delete(u);   /* shared confirm dialog */
}
static void act_categories(lv_event_t *e){ (void)e; menu_close(); cat_trigger_cb(NULL); }

static lv_obj_t *g_about;
static void about_close(void){ if(g_about){ lv_obj_del(g_about); g_about=NULL; } }
static void about_backdrop_cb(lv_event_t *e){ (void)e; about_close(); }
static void act_about(lv_event_t *e){ (void)e;
    menu_close();
    if(g_about) return;
    g_about = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_about, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_about, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_about, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_about, 0, 0);
    lv_obj_set_style_pad_all(g_about, 0, 0);
    lv_obj_add_flag(g_about, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_about, about_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_about);
    lv_obj_set_width(panel, 180);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);           /* generous pad -> no glyph clipping */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ttl = lv_label_create(panel);
    lv_label_set_text(ttl, "CYD Palm");
    lv_obj_set_style_text_font(ttl, &lv_font_palm_bold, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 160);                       /* panel(180) - 2*pad(10) */
    lv_label_set_text(body, "A PalmOS-style PDA that syncs to iCloud.\n\nv0.1 - tap to close");
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);
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
/* Find engine is ready + host-tested (bridge/find.c); its UI still needs a
 * Graffiti query field, so the silkscreen Find is deferred for now. */
static void find_cb(lv_event_t *e){ (void)e; /* TODO Find UI -> find_in_pdb() over all 4 PDBs */ }

/* ------------------------- Calculator (silkscreen accessory) -------------
 * Full-screen modal keypad feeding the host-tested evaluator (bridge/calc.c).
 * Self-contained: it covers the silkscreen too, so it's dismissed with its own
 * Done button. calc_expr holds the entered expression; '=' evaluates in place
 * so the result can seed the next calculation (Palm behaviour). */
static lv_obj_t *g_calc, *g_calc_disp;
static char calc_expr[48];
static int  calc_isresult;   /* last press showed a result -> next digit restarts */

static void calc_close(void){ if(g_calc){ lv_obj_del(g_calc); g_calc=NULL; g_calc_disp=NULL; } }
static void calc_done_cb(lv_event_t *e){ (void)e; calc_close(); }
static void calc_refresh(void){
    if(g_calc_disp) lv_label_set_text(g_calc_disp, calc_expr[0] ? calc_expr : "0");
}
static void calc_apply(char k){
    size_t n = strlen(calc_expr);
    if(k=='C'){ calc_expr[0]=0; calc_isresult=0; }
    else if(k=='<'){ if(n) calc_expr[n-1]=0; calc_isresult=0; }
    else if(k=='='){
        double v; int rc = calc_eval(calc_expr, &v);
        if(rc==CALC_OK) snprintf(calc_expr,sizeof calc_expr,"%.10g", v);
        else snprintf(calc_expr,sizeof calc_expr,"%s", rc==CALC_ERR_DIVZERO?"Div by 0":"Error");
        calc_isresult=1;
    } else {
        /* a digit/paren after a result starts fresh; an operator continues it */
        if(calc_isresult){
            if((k>='0'&&k<='9')||k=='.'||k=='('){ calc_expr[0]=0; n=0; }
            calc_isresult=0;
        }
        if(n < sizeof calc_expr - 1){ calc_expr[n]=k; calc_expr[n+1]=0; }
    }
    calc_refresh();
}
/* One button matrix drives the whole keypad (a single LVGL object -- far lighter
 * than 20 buttons and, crucially, it sizes its own cells so there's no grid-FR
 * auto-size layout recursion). "<-" is the backspace key. */
static void calc_bm_cb(lv_event_t *e){
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *txt = lv_buttonmatrix_get_button_text(bm, id);
    if(!txt) return;
    calc_apply(txt[0]=='<' ? '<' : txt[0]);
}
static void calc_open(void){
    if(g_calc) return;
    calc_expr[0]=0; calc_isresult=0;

    g_calc = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_calc, LCD_W, LCD_H);
    lv_obj_set_pos(g_calc, 0, 0);
    lv_obj_set_style_bg_color(g_calc, COL_BODY, 0);
    lv_obj_set_style_border_width(g_calc, 0, 0);
    lv_obj_set_style_radius(g_calc, 0, 0);
    lv_obj_set_style_pad_all(g_calc, 4, 0);
    lv_obj_clear_flag(g_calc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_calc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_calc, 4, 0);

    /* header: title + Done */
    lv_obj_t *hdr = lv_obj_create(g_calc);
    lv_obj_set_size(hdr, lv_pct(100), 24);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "Calculator");
    lv_obj_set_style_text_font(ttl, &lv_font_palm_bold, 0);
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *done = lv_button_create(hdr);
    lv_obj_set_size(done, 56, 24);
    lv_obj_set_style_radius(done, 0, 0);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done");
    lv_obj_center(dl);
    lv_obj_add_event_cb(done, calc_done_cb, LV_EVENT_CLICKED, NULL);

    /* display */
    lv_obj_t *disp = lv_obj_create(g_calc);
    lv_obj_set_size(disp, lv_pct(100), 44);
    lv_obj_set_style_bg_color(disp, COL_BODY, 0);
    lv_obj_set_style_border_width(disp, 1, 0);
    lv_obj_set_style_border_color(disp, COL_LINE, 0);
    lv_obj_set_style_radius(disp, 0, 0);
    lv_obj_set_style_pad_all(disp, 6, 0);
    lv_obj_clear_flag(disp, LV_OBJ_FLAG_SCROLLABLE);
    g_calc_disp = lv_label_create(disp);
    lv_label_set_long_mode(g_calc_disp, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(g_calc_disp, lv_pct(100));
    lv_obj_set_style_text_font(g_calc_disp, &lv_font_palm_bold, 0);
    lv_obj_set_style_text_align(g_calc_disp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_calc_disp, LV_ALIGN_RIGHT_MID, 0, 0);
    calc_refresh();

    /* keypad: one button matrix (map is static -- LVGL keeps the pointer) */
    static const char *km[] = {
        "C","(",")","<-","\n",
        "7","8","9","/","\n",
        "4","5","6","*","\n",
        "1","2","3","-","\n",
        "0",".","=","+","" };
    lv_obj_t *bm = lv_buttonmatrix_create(g_calc);
    lv_obj_set_width(bm, lv_pct(100));
    lv_obj_set_flex_grow(bm, 1);                 /* fills remaining height */
    lv_buttonmatrix_set_map(bm, km);
    lv_obj_set_style_text_font(bm, &lv_font_palm_bold, 0);
    lv_obj_set_style_radius(bm, 0, 0);
    lv_obj_set_style_radius(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, calc_bm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
static void calc_cb(lv_event_t *e){ (void)e; calc_open(); }

/* ------------------------- F2: category picker ------------------------- */
static lv_obj_t *cat_trigger, *cat_label, *g_catpop;

static void catpop_close(void){ if(g_catpop){ lv_obj_del(g_catpop); g_catpop=NULL; } }
static void catpop_backdrop_cb(lv_event_t *e){ (void)e; catpop_close(); }

static void update_cat_trigger(void){
    if(!cat_trigger) return;
    if(!cur_app){ lv_obj_add_flag(cat_trigger, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(cat_trigger, LV_OBJ_FLAG_HIDDEN);
    int f = data_get_category();
    if(f < 0){ lv_label_set_text(cat_label, "All"); return; }
    CatTable t;
    if(data_get_categories(cur_app->app, &t) && t.name[f][0]) lv_label_set_text(cat_label, t.name[f]);
    else lv_label_set_text(cat_label, "All");
}

static void cat_pick_cb(lv_event_t *e){
    int cat = (int)(intptr_t)lv_event_get_user_data(e);
    const AppDef *a = cur_app;
    catpop_close();
    data_set_category(cat);
    if(a) list_view(a);   /* refresh filtered + updates the trigger */
}

static void cat_item(lv_obj_t *par, const char *txt, int cat){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(b, cat_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cat);
}

/* Palm category pop-up: All + the app's categories, top-right under the trigger */
static void cat_trigger_cb(lv_event_t *e){
    (void)e;
    if(!cur_app || g_catpop) return;
    CatTable t; int have = data_get_categories(cur_app->app, &t);

    g_catpop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_catpop, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(g_catpop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_catpop, 0, 0);
    lv_obj_set_style_pad_all(g_catpop, 0, 0);
    lv_obj_add_flag(g_catpop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_catpop, catpop_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_catpop);
    lv_obj_set_width(panel, 110);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LCD_H - 30, 0);
    lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, -2, TITLE_H);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 3, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    cat_item(panel, "All", -1);
    if(have) for(int c=0;c<CAT_COUNT;c++) if(t.name[c][0]) cat_item(panel, t.name[c], c);
}

/* ------------------------- F4: Details (category) ------------------------- */
static lv_obj_t *g_details;
static void details_close(void){ if(g_details){ lv_obj_del(g_details); g_details=NULL; } }
static void details_backdrop_cb(lv_event_t *e){ (void)e; details_close(); }
static void details_pick_cb(lv_event_t *e){ edit_cat = (int)(intptr_t)lv_event_get_user_data(e); details_close(); set_editcat_label(); }

/* Details dialog: choose the record's category (Palm's per-record Details). */
static void details_open(void){
    if(!cur_app || g_details) return;
    CatTable t; int have = data_get_categories(cur_app->app, &t);

    g_details = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_details, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_details, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_details, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_details, 0, 0);
    lv_obj_set_style_pad_all(g_details, 0, 0);
    lv_obj_add_flag(g_details, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_details, details_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_details);
    lv_obj_set_width(panel, 150);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LCD_H - 40, 0);
    lv_obj_center(panel);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_label_set_text(hdr, "Category:");
    lv_obj_set_style_text_font(hdr, &lv_font_palm_bold, 0);

    if(have){
        for(int c=0;c<CAT_COUNT;c++){
            if(!t.name[c][0]) continue;
            lv_obj_t *b = lv_button_create(panel);
            lv_obj_set_width(b, lv_pct(100));
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_style_pad_ver(b, 4, 0);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text_fmt(l, "%s%s", c==edit_cat?"* ":"  ", t.name[c]);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
            lv_obj_add_event_cb(b, details_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
        }
    }
}

/* ------------------------- U6: Graffiti stroke capture ------------------------- */
static void graf_down_cb(lv_event_t *e){ (void)e; graffiti_clear(); }
static void graf_move_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    graffiti_add_point(p.x, p.y);
}
/* case state armed by the shift upstroke: none -> shift (one letter) -> caps lock
 * -> none, cycling on each upstroke (Palm's single/double/single shift). */
enum { CASE_NONE, CASE_SHIFT, CASE_LOCK };
static int graf_case;
static lv_obj_t *graf_abc_lbl;    /* case hint on the letter pad */
static void show_case(void){
    if(graf_abc_lbl)
        lv_label_set_text(graf_abc_lbl,
            graf_case==CASE_NONE ? "abc" : graf_case==CASE_SHIFT ? "Abc" : "ABC");
}
/* user_data: 0 = letters (abc pad), 1 = digits (123 pad) */
static void graf_up_cb(lv_event_t *e){
    int digits = (int)(intptr_t)lv_event_get_user_data(e);
    char c = graffiti_recognize(digits);
    if(!c) return;
    if(c == GRAF_SHIFT){ graf_case = (graf_case + 1) % 3; show_case(); return; }
    if(!active_ta){ graf_case = CASE_NONE; show_case(); return; }
    if(c == '\b'){                                     /* backspace: keep caps lock */
        lv_textarea_delete_char(active_ta);
        if(graf_case == CASE_SHIFT){ graf_case = CASE_NONE; show_case(); }
        return;
    }
    if(graf_case != CASE_NONE && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    lv_textarea_add_char(active_ta, c);                /* letter, digit, space, or '\n' */
    if(graf_case == CASE_SHIFT){ graf_case = CASE_NONE; show_case(); }
}

/* one Graffiti writing pad (letters or digits) inside the strip */
static void graf_pad(lv_obj_t *parent, int x, int w, int digits){
    lv_obj_t *surf = lv_obj_create(parent);
    lv_obj_set_size(surf, w, GRAFFITI_H - 6);
    lv_obj_set_pos(surf, x, 3);
    lv_obj_set_style_bg_opa(surf, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(surf, 0, 0);
    lv_obj_set_style_pad_all(surf, 0, 0);
    lv_obj_add_flag(surf, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(surf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(surf, graf_down_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(surf, graf_move_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(surf, graf_up_cb,   LV_EVENT_RELEASED, (void *)(intptr_t)digits);
}

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

    /* F2: category pop-up trigger (top-right, Palm convention) */
    cat_trigger = lv_button_create(bar);
    lv_obj_set_height(cat_trigger, TITLE_H - 4);
    lv_obj_align(cat_trigger, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_radius(cat_trigger, 0, 0);
    lv_obj_set_style_pad_hor(cat_trigger, 4, 0);
    lv_obj_add_event_cb(cat_trigger, cat_trigger_cb, LV_EVENT_CLICKED, NULL);
    cat_label = lv_label_create(cat_trigger);
    lv_label_set_text(cat_label, "All");
    lv_obj_center(cat_label);
    lv_obj_add_flag(cat_trigger, LV_OBJ_FLAG_HIDDEN);   /* only shown in data apps */

    /* content area (swappable views) */
    content = panel(scr, 0, TITLE_H, LCD_W, PDA_H - TITLE_H, COL_BODY);

    /* Graffiti strip: silkscreen buttons flank the writing area, Palm-style:
     * [Home][Menu] ... abc | 123 ... [Find][Calc] */
    lv_obj_t *graf = panel(scr, 0, PDA_H, LCD_W, GRAFFITI_H, COL_GRAF);
    mk_silk(graf, &silk_home, LV_ALIGN_TOP_LEFT,     3,  3, home_cb);
    mk_silk(graf, &silk_menu, LV_ALIGN_BOTTOM_LEFT,  3, -3, menu_cb);
    mk_silk(graf, &silk_find, LV_ALIGN_TOP_RIGHT,   -3,  3, find_cb);
    mk_silk(graf, &silk_calc, LV_ALIGN_BOTTOM_RIGHT,-3, -3, calc_cb);

    /* U6: two Graffiti writing pads between the silkscreen buttons -- abc (left)
     * writes letters, 123 (right) writes digits; strokes -> $1 -> active field.
     * Swipe L->R = space, R->L = backspace. There is no on-screen keyboard. */
    int gx0 = 36, gx1 = LCD_W - 36;          /* clear of the 30px silk buttons */
    int gw = gx1 - gx0, half = gw / 2;
    graf_pad(graf, gx0,        half, 0);      /* letters */
    graf_pad(graf, gx0 + half, gw - half, 1);/* digits */

    lv_obj_t *sep = panel(graf, LCD_W/2, 6, 2, GRAFFITI_H-12, COL_LINE);
    (void)sep;
    graf_abc_lbl = lv_label_create(graf);
    lv_label_set_text(graf_abc_lbl, "abc");
    lv_obj_align(graf_abc_lbl, LV_ALIGN_CENTER, -28, 0);
    lv_obj_t *gr = lv_label_create(graf);
    lv_label_set_text(gr, "123");
    lv_obj_align(gr, LV_ALIGN_CENTER, 28, 0);

    show_launcher();
}
