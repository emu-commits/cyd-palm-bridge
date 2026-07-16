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
#include "find.h"         /* global search engine (bridge/find.c) */
#include "appcfg.h"
#include "power.h"        /* live backlight brightness for the Preferences slider */
#include "clock.h"        /* timezone picker + DST-aware zone list */
#include "lvgl.h"
#include <string.h>
#include <strings.h>      /* strncasecmp for the Address Look Up filter */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TITLE_H     24
#define COL_TITLE   lv_color_hex(0x000000)   /* black title bar (Palm: white-on-black) */
#define COL_TITLE_FG lv_color_hex(0xFFFFFF)  /* title text/glyphs on the black bar */
#define COL_BODY    lv_color_hex(0xFFFFFF)   /* white app body   */
#define COL_GRAF    lv_color_hex(0xD6D6D6)   /* graffiti strip   */
#define COL_LINE    lv_color_hex(0x000000)   /* black rules      */

static lv_obj_t *content;      /* the swappable view area */
static lv_obj_t *title_lbl;
static lv_obj_t *clock_lbl;    /* live clock in the title bar (Palm) */

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
static lv_obj_t *g_fields[12];         /* edit-form textareas (also the Preferences form) */
static int g_nfields;
static lv_obj_t *active_ta;            /* last-focused textarea (Graffiti target) */

/* To Do due-date picker state: edited via the due popup, written on Save. */
static int g_due_has, g_due_y, g_due_m, g_due_d;
static lv_obj_t *g_due_lbl;            /* label on the edit-form Due trigger */

static void list_view(const AppDef *ad);
static void show_detail(uint32_t uid);
static void show_edit(uint32_t uid);
static void show_prefs(void);
static void show_discover(void);
static void update_cat_trigger(void);
static void cat_trigger_cb(lv_event_t *e);
static void details_open(void);
static void due_open(void);
static void due_btn_cb(lv_event_t *e);
static void due_set_label(void);
static void br_open(void);

/* Date Book uses PalmOS's date-centric views (Day + Month) instead of a flat list
 * of every event -- see show_datebook_day / show_datebook_month. g_cal_* holds the
 * currently-viewed day so navigation + "return from a record" land back on it. */
static void show_datebook_day(int y, int m, int d);
static void show_datebook_month(int y, int m);
static void app_reopen(const AppDef *a);   /* back to an app's main view (Day view for Date Book) */
static int  g_cal_y, g_cal_m, g_cal_d;

/* HotSync screen state (status label + polling timer live in `content`) */
static lv_obj_t *hs_status;
static lv_timer_t *hs_timer;
static void kill_hs(void){ if(hs_timer){ lv_timer_delete(hs_timer); hs_timer=NULL; } hs_status=NULL; }

/* Discovery screen state (a status label + polling timer, like HotSync) */
static lv_obj_t *disc_status;
static lv_timer_t *disc_timer;
static int disc_built;

/* tear down per-form / per-screen state when navigating away. Text entry is
 * Graffiti-only (no on-screen keyboard), so there's no overlay to drop here. */
static void free_rowuids(void);
static void free_finds(void);
static lv_obj_t *g_listtbl;           /* current record table (partial rebuild) */
static lv_obj_t *g_findtbl;           /* Find results table                     */
static void kill_kb(void){
    g_form=NULL; active_ta=NULL; edit_cat_lbl=NULL; g_listtbl=NULL; g_findtbl=NULL;
    free_rowuids();
    free_finds();
    kill_hs();
    if(disc_timer){ lv_timer_delete(disc_timer); disc_timer=NULL; }
    disc_status=NULL;
}

/* The record list is one virtualized `lv_table` (a SINGLE LVGL object) instead of
 * an `lv_list` of N button objects. This is the fix for the no-PSRAM pool crash:
 * `lv_list` materializes a full button+label per row from the fixed 24 KB LVGL
 * object pool AND needs a draw task per row from that same pool -- ~20-35 rows
 * exhausted it (StoreProhibited on the failed alloc, or a Task WDT when the draw
 * timer spun). That forced a hard LIST_MAX=12 cap, so records past 12 were
 * unreachable. `lv_table` holds only compact per-cell text (not objects) and
 * draws only the visible rows, so row count is bounded by free heap for the cell
 * strings (KBs for hundreds of records), not by the object pool. The cap is gone.
 *
 * Row->record identity: the table has no per-row user_data, so we keep a parallel
 * uid array (g_rowuids) indexed by row and resolve it in the click handler. It's
 * malloc'd to the record count (two-pass: count, then fill) and freed on nav. */
static uint32_t *g_rowuids;
static int       g_rowuid_n;
static void free_rowuids(void){ free(g_rowuids); g_rowuids=NULL; g_rowuid_n=0; }

/* per-app "lens" state (parallels the Date Book's date-centric navigation):
 *  - Address: a Graffiti "Look Up" prefix filter on the name (Palm's signature
 *    Address navigation aid) + a Name | Phone two-column layout.
 *  - To Do: a checkbox column (tap col 0 = toggle done) + a Show Completed
 *    toggle in the Options menu.
 * g_listtbl is the live record table, kept so the Address filter can rebuild
 * just the table without tearing down the Look Up field. */
static char      g_lookup[24];        /* Address Look Up prefix (Graffiti) */
static int       g_todo_show_done = 1;/* To Do: include completed items      */
static int       g_todo_sort_due  = 0;/* To Do: 0 = by priority, 1 = by due  */

/* used by the Date Book day view, whose list is bounded to one day's events */
static void row_cb(lv_event_t *e){
    show_detail((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

/* keep predicate shared by the count + fill passes (reads cur_app). Must be
 * applied identically in both passes so g_rowuids is sized to what's shown. */
static int row_keep(const char *primary){
    if(!cur_app) return 1;
    if(cur_app->app==APP_ADDR && g_lookup[0]){
        /* Palm Look Up matches by last OR first name. primary is "Last, First"
         * (or just a name/company), so test the whole string and, if present,
         * the first-name token after ", ". */
        size_t n = strlen(g_lookup);
        int hit = (strncasecmp(primary, g_lookup, n) == 0);
        if(!hit){
            const char *comma = strstr(primary, ", ");
            if(comma) hit = (strncasecmp(comma + 2, g_lookup, n) == 0);
        }
        if(!hit) return 0;
    }
    if(cur_app->app==APP_TODO && !g_todo_show_done)
        if(primary[0]=='[' && primary[1]=='x') return 0;   /* completed = "[x] ..." */
    return 1;
}

static void tbl_count_cb(uint32_t uid,const char*p,const char*s,void*ctx){
    (void)uid;(void)s; if(row_keep(p)) (*(int*)ctx)++;
}

/* Collected row for sorting. Palm sorts these lists (Address by name, To Do by
 * priority, Memo alphabetically) rather than showing raw PDB order, so we buffer
 * the display text + a sort key, qsort, then fill the table in order. The buffer
 * is transient (freed after fill) and only lives in interactive mode (no TLS), so
 * the RAM is available. */
typedef struct {
    uint32_t uid;
    int      done;        /* To Do completed (sorts incomplete first) */
    int      pri;         /* To Do priority 1..5 (1 = highest)         */
    int      due;         /* To Do due date YYYYMMDD (0 = none)         */
    char     c0[8];       /* col-0 text (To Do checkbox), else empty    */
    char     c1[96];      /* main display text                          */
    char     sort[48];    /* case-folded sort key                       */
} SRow;
typedef struct { SRow *rows; int n; int cap; } Collect;

static void collect_cb(uint32_t uid,const char*primary,const char*secondary,void*ctx){
    Collect *co = (Collect*)ctx;
    if(!row_keep(primary) || co->n >= co->cap) return;
    SRow *r = &co->rows[co->n++];
    r->uid=uid; r->done=0; r->pri=99; r->due=0; r->c0[0]=0;
    if(cur_app && cur_app->app==APP_TODO){
        r->done = (primary[0]=='[' && primary[1]=='x');
        const char *txt = (primary[0]=='[') ? primary+4 : primary;
        snprintf(r->c0, sizeof r->c0, "%s", r->done ? "[x]" : "[ ]");
        if(!secondary || sscanf(secondary,"pri %d due %d",&r->pri,&r->due)<1){ r->pri=99; r->due=0; }
        /* Palm To Do row: "<pri> description        <due>". Show the due date
         * (M/D) at the right when set; priority as a leading digit. */
        if(r->due){
            int dm=(r->due/100)%100, dd=r->due%100;
            snprintf(r->c1,sizeof r->c1,"%d %.72s   %d/%d",r->pri,txt,dm,dd);
        } else {
            snprintf(r->c1,sizeof r->c1,"%d %.80s",r->pri,txt);
        }
        snprintf(r->sort, sizeof r->sort, "%s", txt);
    } else {
        if(secondary && secondary[0]) snprintf(r->c1,sizeof r->c1,"%s  (%s)",primary,secondary);
        else                          snprintf(r->c1,sizeof r->c1,"%s",primary);
        snprintf(r->sort, sizeof r->sort, "%s", primary);
    }
}
static int cmp_name(const void *a,const void *b){        /* Address, Memo */
    return strcasecmp(((const SRow*)a)->sort, ((const SRow*)b)->sort);
}
static int cmp_todo(const void *a,const void *b){        /* incomplete, then priority, then text */
    const SRow *x=a, *y=b;
    if(x->done != y->done) return x->done - y->done;
    if(x->pri  != y->pri ) return x->pri  - y->pri;
    return strcasecmp(x->sort, y->sort);
}
/* due-date sort: incomplete first, then earliest due (undated last), then priority. */
static int cmp_todo_due(const void *a,const void *b){
    const SRow *x=a, *y=b;
    if(x->done != y->done) return x->done - y->done;
    int xd = x->due ? x->due : 99999999;     /* undated sinks to the bottom */
    int yd = y->due ? y->due : 99999999;
    if(xd != yd) return xd - yd;
    if(x->pri != y->pri) return x->pri - y->pri;
    return strcasecmp(x->sort, y->sort);
}
static void tbl_click_cb(lv_event_t *e){
    lv_obj_t *t = lv_event_get_target(e);
    uint32_t r=LV_TABLE_CELL_NONE, c=LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &r, &c);
    if(r==LV_TABLE_CELL_NONE || !g_rowuids || (int)r >= g_rowuid_n) return;
    uint32_t uid = g_rowuids[r];
    if(cur_app && cur_app->app==APP_TODO && c==0){   /* tap the checkbox = toggle */
        data_toggle_todo(uid);
        if(cur_app) app_reopen(cur_app);
    } else {
        show_detail(uid);
    }
}
/* NOTE: lv_table selection must be read on LV_EVENT_VALUE_CHANGED, NOT
 * LV_EVENT_CLICKED: on RELEASED the table sends VALUE_CHANGED and then resets the
 * selected cell to CELL_NONE, and CLICKED is delivered afterward -- so a CLICKED
 * handler always reads CELL_NONE and does nothing. VALUE_CHANGED is sent only on
 * a genuine tap (not a scroll drag), so it behaves like a click for our purpose. */

/* (re)build just the record table for cur_app. Callers set the title / any
 * filter bar first; the Address Look Up field calls this on each keystroke. */
static void build_record_table(void){
    free_rowuids();
    if(g_listtbl){ lv_obj_del(g_listtbl); g_listtbl=NULL; }
    int todo = (cur_app && cur_app->app==APP_TODO);
    int addr = (cur_app && cur_app->app==APP_ADDR);

    int n = 0;
    cur_app->iter(tbl_count_cb, &n);          /* pass 1: count (filtered) */

    lv_obj_t *t = lv_table_create(content);
    g_listtbl = t;
    lv_obj_set_style_radius(t, 0, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 4, LV_PART_ITEMS);
    if(todo){ lv_table_set_column_width(t, 0, 34); lv_table_set_column_width(t, 1, LCD_W-46); }
    else    { lv_table_set_column_width(t, 0, LCD_W-8); }
    /* Address reserves the top strip for the Look Up field; others fill content */
    if(addr){ lv_obj_set_size(t, lv_pct(100), lv_pct(84)); lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, 0); }
    else    { lv_obj_set_size(t, lv_pct(100), lv_pct(100)); }
    lv_obj_add_event_cb(t, tbl_click_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if(n <= 0){ lv_table_set_cell_value(t, 0, 0, "(no records)"); return; }

    SRow *rows = calloc(n, sizeof *rows);
    g_rowuids  = calloc(n, sizeof *g_rowuids);
    if(!rows || !g_rowuids){                    /* out of RAM -> degrade, don't crash */
        free(rows); free_rowuids();
        lv_table_set_cell_value(t, 0, 0, "(low memory)");
        return;
    }
    Collect co = { rows, 0, n };
    cur_app->iter(collect_cb, &co);             /* pass 2: collect rows */
    qsort(rows, co.n, sizeof *rows,
          todo ? (g_todo_sort_due ? cmp_todo_due : cmp_todo) : cmp_name);

    g_rowuid_n = co.n;
    for(int i=0;i<co.n;i++){
        if(todo){ lv_table_set_cell_value(t, i, 0, rows[i].c0);
                  lv_table_set_cell_value(t, i, 1, rows[i].c1); }
        else      lv_table_set_cell_value(t, i, 0, rows[i].c1);
        g_rowuids[i] = rows[i].uid;
    }
    free(rows);
}

/* Address Look Up: mirror the field text into g_lookup and refilter the table */
static void lookup_ta_cb(lv_event_t *e){
    lv_obj_t *ta = lv_event_get_target(e);
    snprintf(g_lookup, sizeof g_lookup, "%s", lv_textarea_get_text(ta));
    build_record_table();
}

/* scrolling list of records for one app (virtualized lv_table + per-app lens) */
static void list_view(const AppDef *ad){
    kill_kb();
    cur_app = ad;
    cur_uid = 0;
    lv_obj_clean(content);
    g_listtbl = NULL;
    lv_label_set_text(title_lbl, ad->name);

    if(ad->app == APP_ADDR){
        lv_obj_t *lb = lv_label_create(content);
        lv_label_set_text(lb, "Look Up:"); lv_obj_set_pos(lb, 4, 8);
        lv_obj_t *ta = lv_textarea_create(content);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, sizeof g_lookup - 1);
        lv_textarea_set_text(ta, g_lookup);           /* set BEFORE the cb so it doesn't fire */
        lv_obj_set_width(ta, LCD_W - 72);
        lv_obj_set_pos(ta, 66, 2);
        lv_obj_add_event_cb(ta, lookup_ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
        active_ta = ta;                                /* Graffiti types into Look Up */
    } else {
        g_lookup[0] = 0;                               /* filter only applies to Address */
    }

    build_record_table();
    update_cat_trigger();
}

static void done_cb(lv_event_t *e){ (void)e; if(cur_app) app_reopen(cur_app); }

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
    if(a && u){ data_delete(a->app, u); cur_uid = 0; app_reopen(a); }
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
    static char buf[1280];   /* fits a full-length memo (mtext is 1200) without truncation */
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
    /* a new event defaults to the day the user is looking at (Palm "New" on a day) */
    if(g_cal_y >= 2024){ a->year=g_cal_y; a->month=g_cal_m; a->day=g_cal_d; }
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
        t.hasDue = g_due_has;
        if(g_due_has){ t.dueY=g_due_y; t.dueM=g_due_m; t.dueD=g_due_d; }
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
    app_reopen(cur_app);
}
static void cancel_cb(lv_event_t *e){ (void)e; app_reopen(cur_app); }

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

    /* C4: Palm form contract -- the action row lives across the BOTTOM of the
     * form (Palm's Done/Details convention), Done leftmost. Done saves (Palm
     * edits committed on Done); Details is the category trigger; Cancel
     * discards. The fields fill the space above. */
    lv_obj_t *done = lv_button_create(content);
    lv_obj_set_size(done, 64, 30); lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 4, -3);
    lv_obj_t *dl=lv_label_create(done); lv_label_set_text(dl,"Done"); lv_obj_center(dl);
    lv_obj_add_event_cb(done, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *det = lv_button_create(content);
    lv_obj_set_size(det, 92, 30); lv_obj_align(det, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_pad_hor(det, 2, 0);
    edit_cat_lbl = lv_label_create(det); lv_obj_center(edit_cat_lbl);
    lv_obj_add_event_cb(det, details_btn_cb, LV_EVENT_CLICKED, NULL);
    set_editcat_label();
    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 64, 30); lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

    g_form = lv_obj_create(content);
    lv_obj_t *form = g_form;
    lv_obj_set_size(form, LCD_W, (PDA_H - TITLE_H) - 38);   /* fields above the bottom bar */
    lv_obj_set_pos(form, 0, 0);
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
        g_due_has=t.hasDue; g_due_y=t.dueY; g_due_m=t.dueM; g_due_d=t.dueD;
        form_field(form,"Description",t.description,255,&y);
        form_field(form,"Note",t.note,500,&y);
        /* Due-date trigger (Palm's To Do due popup). A button, not a text field,
         * so it isn't in g_fields; the picked date lives in g_due_* until Save. */
        lv_obj_t *dlab = lv_label_create(form);
        lv_label_set_text(dlab, "Due"); lv_obj_set_pos(dlab, 2, y);
        lv_obj_t *db = lv_button_create(form);
        lv_obj_set_size(db, LCD_W - 16, 30);
        lv_obj_set_pos(db, 2, y + 15);
        lv_obj_set_style_radius(db, 0, 0);
        g_due_lbl = lv_label_create(db);
        lv_obj_align(g_due_lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_event_cb(db, due_btn_cb, LV_EVENT_CLICKED, NULL);
        due_set_label();
        y += 52;
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
        lv_obj_set_size(ta, LCD_W - 16, (PDA_H - TITLE_H) - 46);
        lv_obj_set_pos(ta, 2, 2);
        lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
        g_fields[g_nfields++] = ta;
    }

    /* focus the first field so Graffiti has a target immediately */
    if(g_nfields > 0){ active_ta = g_fields[0]; lv_obj_add_state(g_fields[0], LV_STATE_FOCUSED); }
}

/* U7: HotSync screen (Sync Now + a status line polled from the background task) */
/* Progress is TEXT, not an lv_bar. On this no-PSRAM device the heap is badly
 * fragmented during a sync (Wi-Fi + TLS hold the big blocks), and an lv_bar
 * forces LVGL to allocate a draw-LAYER buffer to composite its indicator -- that
 * allocation fails mid-sync and LVGL spins retrying the draw every refresh,
 * starving IDLE0 -> Task WDT -> frozen screen (seen freezing at 66%). A label
 * never allocates a layer, so the percentage is shown as text instead. */
static void hs_tick(lv_timer_t *t){
    (void)t;
    if(!hs_status) return;
    int p = hotsync_progress();              /* -1 idle, else 0..100 */
    if(p >= 0 && p < 100)
        lv_label_set_text_fmt(hs_status, "%s\n%d%%", hotsync_status(), p);
    else
        lv_label_set_text(hs_status, hotsync_status());
}
static void hs_sync_cb(lv_event_t *e){ (void)e; hotsync_start(); }

static void show_hotsync(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "HotSync");
    update_cat_trigger();

    /* C2: the classic HotSync moment -- the logo front and centre, the status
     * ("Synchronizing <app>... N%") beneath it. Progress stays TEXT (never an
     * lv_bar: its draw-layer alloc fails mid-sync on the fragmented no-PSRAM
     * heap and live-locks LVGL -- see the note at hs_tick). The icon is drawn
     * once, before any sync starts, so it costs nothing during the window. */
    lv_obj_t *img = lv_image_create(content);
    lv_image_set_src(img, &icon_hotsync);
    lv_obj_set_style_image_recolor(img, COL_LINE, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 12);

    hs_status = lv_label_create(content);
    lv_label_set_long_mode(hs_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hs_status, LCD_W - 12);
    lv_obj_align(hs_status, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_text_align(hs_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hs_status, hotsync_status());

    lv_obj_t *btn = lv_button_create(content);
    lv_obj_set_size(btn, 130, 38);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &lv_font_palm_bold, 0);
    lv_label_set_text(bl, "Sync Now");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, hs_sync_cb, LV_EVENT_CLICKED, NULL);

    hs_timer = lv_timer_create(hs_tick, 400, NULL);
}

/* ===================== Date Book: PalmOS Day + Month views =====================
 * A flat list of every event doesn't scale (and isn't how Palm works). Instead the
 * Date Book opens on a Day view -- one day's time-sorted agenda, naturally bounded
 * -- with prev/next-day nav and a Month view (a calendar grid, days-with-events
 * dotted) to jump anywhere. This mirrors DateBook's Day/Week/Month views. */
static const char *CAL_MON[] = {"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *CAL_WD[]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void cal_today(int *y,int *m,int *d){
    time_t now=0; time(&now); struct tm t; localtime_r(&now,&t);
    if(t.tm_year+1900 < 2024){ *y=2026; *m=1; *d=1; return; }
    *y=t.tm_year+1900; *m=t.tm_mon+1; *d=t.tm_mday;
}
static int cal_wday(int y,int m,int d){
    struct tm t={0}; t.tm_year=y-1900; t.tm_mon=m-1; t.tm_mday=d; t.tm_hour=12;
    mktime(&t); return t.tm_wday;
}
static void cal_add_days(int *y,int *m,int *d,int delta){
    struct tm t={0}; t.tm_year=*y-1900; t.tm_mon=*m-1; t.tm_mday=*d+delta; t.tm_hour=12;
    time_t tt=mktime(&t); struct tm n; localtime_r(&tt,&n);
    *y=n.tm_year+1900; *m=n.tm_mon+1; *d=n.tm_mday;
}

/* --- Day view --- */
#define DAY_MAX 24                              /* events/day materialized (bounded) */
typedef struct { uint32_t uid; char txt[92]; } DayRow;
static DayRow g_dayrows[DAY_MAX];
static int    g_ndayrows;
static void day_collect(uint32_t uid,const char *primary,const char *secondary,void *ctx){
    (void)secondary;(void)ctx;
    if(g_ndayrows>=DAY_MAX) return;
    g_dayrows[g_ndayrows].uid=uid;
    snprintf(g_dayrows[g_ndayrows].txt,sizeof g_dayrows[0].txt,"%s",primary);
    g_ndayrows++;
}
static int day_cmp(const void *a,const void *b){
    return strcmp(((const DayRow*)a)->txt,((const DayRow*)b)->txt);   /* "HH:MM " prefix => chrono */
}
static void day_prev_cb(lv_event_t *e){ (void)e; cal_add_days(&g_cal_y,&g_cal_m,&g_cal_d,-1); show_datebook_day(g_cal_y,g_cal_m,g_cal_d); }
static void day_next_cb(lv_event_t *e){ (void)e; cal_add_days(&g_cal_y,&g_cal_m,&g_cal_d, 1); show_datebook_day(g_cal_y,g_cal_m,g_cal_d); }
static void day_month_cb(lv_event_t *e){ (void)e; show_datebook_month(g_cal_y,g_cal_m); }

static void show_datebook_day(int y,int m,int d){
    kill_kb();
    g_cal_y=y; g_cal_m=m; g_cal_d=d;
    cur_app=&APPDEFS[0]; cur_uid=0;     /* Date Book context (menu New/Categories) */
    lv_obj_clean(content);
    char t[36]; snprintf(t,sizeof t,"%s %d/%d",CAL_WD[cal_wday(y,m,d)],m,d);
    lv_label_set_text(title_lbl,t);
    update_cat_trigger();

    lv_obj_t *prev=lv_button_create(content); lv_obj_set_size(prev,40,28); lv_obj_align(prev,LV_ALIGN_TOP_LEFT,2,2);
    lv_obj_t *pl=lv_label_create(prev); lv_label_set_text(pl,"<"); lv_obj_center(pl);
    lv_obj_add_event_cb(prev,day_prev_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *next=lv_button_create(content); lv_obj_set_size(next,40,28); lv_obj_align(next,LV_ALIGN_TOP_RIGHT,-2,2);
    lv_obj_t *nl=lv_label_create(next); lv_label_set_text(nl,">"); lv_obj_center(nl);
    lv_obj_add_event_cb(next,day_next_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *mon=lv_button_create(content); lv_obj_set_size(mon,130,28); lv_obj_align(mon,LV_ALIGN_TOP_MID,0,2);
    lv_obj_t *ml=lv_label_create(mon); lv_label_set_text_fmt(ml,"%s %d, %d",CAL_MON[m],d,y); lv_obj_center(ml);
    lv_obj_add_event_cb(mon,day_month_cb,LV_EVENT_CLICKED,NULL);   /* tap the date -> Month view */

    g_ndayrows=0;
    data_cal_day(y,m,d,day_collect,NULL);
    qsort(g_dayrows,g_ndayrows,sizeof g_dayrows[0],day_cmp);

    lv_obj_t *list=lv_list_create(content);
    lv_obj_set_size(list,LCD_W,FORM_FULL);
    lv_obj_set_pos(list,0,34);
    lv_obj_set_style_radius(list,0,0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,0,0);
    if(g_ndayrows==0){
        lv_obj_t *b=lv_list_add_button(list,NULL,"(no events)"); lv_obj_set_style_radius(b,0,0);
    } else for(int i=0;i<g_ndayrows;i++){
        lv_obj_t *b=lv_list_add_button(list,NULL,g_dayrows[i].txt);
        lv_obj_set_style_radius(b,0,0);
        lv_obj_add_event_cb(b,row_cb,LV_EVENT_CLICKED,(void*)(uintptr_t)g_dayrows[i].uid);
    }
}

/* --- Month view (lv_calendar: one light widget, days-with-events highlighted) --- */
static lv_calendar_date_t g_cal_hl[31];   /* persists: LVGL keeps the pointer */
static void month_pick_cb(lv_event_t *e){
    lv_obj_t *cal=(lv_obj_t*)lv_event_get_current_target(e);
    lv_calendar_date_t dd;
    if(lv_calendar_get_pressed_date(cal,&dd)==LV_RESULT_OK)
        show_datebook_day(dd.year,dd.month,dd.day);
}
static void month_prev_cb(lv_event_t *e){ (void)e; if(--g_cal_m<1){ g_cal_m=12; g_cal_y--; } show_datebook_month(g_cal_y,g_cal_m); }
static void month_next_cb(lv_event_t *e){ (void)e; if(++g_cal_m>12){ g_cal_m=1;  g_cal_y++; } show_datebook_month(g_cal_y,g_cal_m); }
static void month_today_cb(lv_event_t *e){ (void)e; int y,m,d; cal_today(&y,&m,&d); show_datebook_day(y,m,d); }

static void show_datebook_month(int y,int m){
    kill_kb();
    g_cal_y=y; g_cal_m=m;
    cur_app=&APPDEFS[0]; cur_uid=0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl,"Date Book");
    update_cat_trigger();

    lv_obj_t *prev=lv_button_create(content); lv_obj_set_size(prev,38,26); lv_obj_align(prev,LV_ALIGN_TOP_LEFT,2,2);
    lv_obj_t *pl=lv_label_create(prev); lv_label_set_text(pl,"<"); lv_obj_center(pl);
    lv_obj_add_event_cb(prev,month_prev_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *next=lv_button_create(content); lv_obj_set_size(next,38,26); lv_obj_align(next,LV_ALIGN_TOP_RIGHT,-2,2);
    lv_obj_t *nl=lv_label_create(next); lv_label_set_text(nl,">"); lv_obj_center(nl);
    lv_obj_add_event_cb(next,month_next_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *lbl=lv_label_create(content); lv_label_set_text_fmt(lbl,"%s %d",CAL_MON[m],y);
    lv_obj_set_style_text_font(lbl,&lv_font_palm_bold,0); lv_obj_align(lbl,LV_ALIGN_TOP_MID,0,8);

    lv_obj_t *cal=lv_calendar_create(content);
    lv_obj_set_size(cal, LCD_W-6, FORM_FULL-30);
    lv_obj_align(cal, LV_ALIGN_TOP_MID, 0, 32);
    int ty,tm,td; cal_today(&ty,&tm,&td);
    lv_calendar_set_today_date(cal,ty,tm,td);
    lv_calendar_set_showed_date(cal,y,m);
    uint8_t marks[32]; data_cal_month_marks(y,m,marks);
    int n=0; for(int dd=1; dd<=31 && n<31; dd++) if(marks[dd]){ g_cal_hl[n].year=y; g_cal_hl[n].month=m; g_cal_hl[n].day=dd; n++; }
    lv_calendar_set_highlighted_dates(cal, g_cal_hl, n);
    lv_obj_add_event_cb(cal, month_pick_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tb=lv_button_create(content); lv_obj_set_size(tb,70,26); lv_obj_align(tb,LV_ALIGN_BOTTOM_MID,0,-2);
    lv_obj_t *tl=lv_label_create(tb); lv_label_set_text(tl,"Today"); lv_obj_center(tl);
    lv_obj_add_event_cb(tb,month_today_cb,LV_EVENT_CLICKED,NULL);
}

/* return to an app's main view after a record action (Day view for Date Book). */
static void app_reopen(const AppDef *a){
    if(a && a->app==APP_CAL) show_datebook_day(g_cal_y,g_cal_m,g_cal_d);
    else if(a)               list_view(a);
    else                     show_launcher();
}

static void show_app(const char *name){
    if(!strcmp(name, "Date Book")){                 /* PalmOS Day view, not a flat list */
        data_set_category(-1);
        int y,m,d; cal_today(&y,&m,&d);
        show_datebook_day(y,m,d);
        return;
    }
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

/* ============ P1.5: Preferences + collection discovery ============ */

/* A dismissable one-shot alert (Save feedback, discovery errors). Tapping
 * anywhere closes it. */
static lv_obj_t *g_alert;
static void alert_close(void){ if(g_alert){ lv_obj_del(g_alert); g_alert=NULL; } }
static void alert_ok_cb(lv_event_t *e){ (void)e; alert_close(); }
static void alert_show(const char *msg){
    if(g_alert) return;
    g_alert = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_alert, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_alert, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_alert, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_alert, 0, 0);
    lv_obj_set_style_pad_all(g_alert, 0, 0);
    lv_obj_add_flag(g_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_alert, alert_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_alert);
    lv_obj_set_width(panel, 200);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, alert_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(panel);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 180);
    lv_label_set_text(l, msg);
}

/* ---- Preferences: edit the config.ini fields on-device (Graffiti entry) ----
 * Rendered as an lv_list (the app-list pattern, which is rock-solid here) rather
 * than one big scrollable form: a form holding 10 textareas + buttons both
 * strained LVGL's 24 KB pool AND drove an infinite scroll-relayout loop (Task
 * WDT) once the content grew past the viewport. Each field is a list row
 * "Label: value"; tapping opens a single-field editor with ONE textarea (light,
 * and the proven-safe widget). Edits land in the in-memory config immediately;
 * the "Save to config.ini" row persists them (also picks up Discover's writes). */
enum { PF_SSID, PF_WPASS, PF_USER, PF_PASS, PF_CALB, PF_CARDB,
       PF_CAL, PF_TODO, PF_CARD, PF_TZ, PF_N };
static const char *PF_LABELS[PF_N] = {
    "Wi-Fi SSID", "Wi-Fi pass", "Apple ID", "App pass", "CalDAV host",
    "CardDAV host", "Calendar coll", "Reminders coll", "Address coll", "Time zone",
};
static const char *pol_name(int p){
    return p==CFG_POL_LOCAL ? "device wins"
         : p==CFG_POL_BOTH  ? "keep both"
         :                    "iCloud wins";
}
/* the config buffer + capacity for field i (both read and write go through this) */
static char *pf_buf(Config *c, int i, int *cap){
    switch(i){
        case PF_SSID:  *cap=sizeof c->wifi_ssid;     return c->wifi_ssid;
        case PF_WPASS: *cap=sizeof c->wifi_pass;     return c->wifi_pass;
        case PF_USER:  *cap=sizeof c->dav_user;      return c->dav_user;
        case PF_PASS:  *cap=sizeof c->dav_pass;      return c->dav_pass;
        case PF_CALB:  *cap=sizeof c->dav_base;      return c->dav_base;
        case PF_CARDB: *cap=sizeof c->dav_card_base; return c->dav_card_base;
        case PF_CAL:   *cap=sizeof c->cal_coll;      return c->cal_coll;
        case PF_TODO:  *cap=sizeof c->todo_coll;     return c->todo_coll;
        case PF_CARD:  *cap=sizeof c->card_coll;     return c->card_coll;
        case PF_TZ:    *cap=sizeof c->timezone;      return c->timezone;
    }
    *cap=0; return NULL;
}

/* ---- single-field editor (one textarea at a time) ---- */
static int pf_edit_idx;
static void pf_edit_cancel_cb(lv_event_t *e){ (void)e; show_prefs(); }
static void pf_edit_save_cb(lv_event_t *e){ (void)e;
    int cap=0; char *dst = pf_buf(appcfg_mut(), pf_edit_idx, &cap);
    if(dst && cap) snprintf(dst, cap, "%s", lv_textarea_get_text(g_fields[0]));
    appcfg_save();            /* persist to SD now -> survives reboot */
    show_prefs();
}

/* I1.2: on-screen keyboard for the Preferences fields. Entering a 19-character
 * app-specific password stroke-by-stroke through an untuned recognizer was the
 * single biggest setup blocker, so config fields get a tap keyboard: ONE
 * lv_buttonmatrix (the calculator's proven pattern -- a single object, sizes
 * its own cells, safe in the 24 KB pool). Graffiti still works in parallel;
 * record editing everywhere else remains Graffiti-only (the signature input). */
static const char *KB_LOWER[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","@","\n",
    "ABC","z","x","c","v","b","n","m","<-","\n",
    "123",".","-","_","space",":","/","" };
static const char *KB_UPPER[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","@","\n",
    "abc","Z","X","C","V","B","N","M","<-","\n",
    "123",".","-","_","space",":","/","" };
static const char *KB_DIGIT[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "!","#","$","%","&","*","(",")","+","=","\n",
    "abc",",",";","'","\"","?","~","^","<-","\n",
    "ABC",".","-","_","space",":","/","" };
static void prefkb_cb(lv_event_t *e){
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *t = lv_buttonmatrix_get_button_text(bm, id);
    if(!t || !active_ta) return;
    if(!strcmp(t, "ABC")){ lv_buttonmatrix_set_map(bm, KB_UPPER); return; }
    if(!strcmp(t, "abc")){ lv_buttonmatrix_set_map(bm, KB_LOWER); return; }
    if(!strcmp(t, "123")){ lv_buttonmatrix_set_map(bm, KB_DIGIT); return; }
    if(!strcmp(t, "<-")) { lv_textarea_delete_char(active_ta); return; }
    if(!strcmp(t, "space")){ lv_textarea_add_char(active_ta, ' '); return; }
    lv_textarea_add_char(active_ta, (uint32_t)t[0]);
}

static void show_pref_edit(int i){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0; pf_edit_idx = i;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, PF_LABELS[i]);
    update_cat_trigger();

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 60, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, pf_edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = lv_button_create(content);
    lv_obj_set_size(save, 60, 28); lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *sl=lv_label_create(save); lv_label_set_text(sl,"Save"); lv_obj_center(sl);
    lv_obj_add_event_cb(save, pf_edit_save_cb, LV_EVENT_CLICKED, NULL);

    int cap=0; const char *val = pf_buf(appcfg_mut(), i, &cap);
    lv_obj_t *lb = lv_label_create(content);
    lv_label_set_text(lb, PF_LABELS[i]);
    lv_obj_set_pos(lb, 4, 38);
    lv_obj_t *ta = lv_textarea_create(content);       /* ONE textarea -> light + safe */
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, cap>0 ? cap-1 : 63);
    lv_textarea_set_text(ta, val ? val : "");
    lv_obj_set_width(ta, LCD_W - 16);
    lv_obj_set_pos(ta, 4, 54);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    g_fields[0] = ta; g_nfields = 1; active_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    /* the tap keyboard fills the rest of the screen below the field */
    lv_obj_t *bm = lv_buttonmatrix_create(content);
    lv_obj_set_size(bm, LCD_W - 4, (PDA_H - TITLE_H) - 92);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_buttonmatrix_set_map(bm, KB_LOWER);
    lv_obj_set_style_radius(bm, 0, 0);
    lv_obj_set_style_radius(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, prefkb_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- timezone picker (replaces the free-text TZ editor) ----
 * Picks from clock.c's built-in DST-aware zone list, so the chosen zone always
 * resolves to a POSIX rule string and DST fires automatically. The header shows
 * the resulting wall-clock offset + whether DST is active right now.
 * Built on lv_table (virtualized), NOT an lv_list of ~24 buttons -- that many
 * buttons exhausted the 24 KB LVGL pool and froze the device (draw-task WDT),
 * the same failure class the record list hit. The table row index == zone index. */
static void tz_cancel_cb(lv_event_t *e){ (void)e; show_prefs(); }
static void tz_tbl_click_cb(lv_event_t *e){
    lv_obj_t *t = lv_event_get_target(e);
    uint32_t r=LV_TABLE_CELL_NONE, c=LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &r, &c);
    if(r==LV_TABLE_CELL_NONE || (int)r >= clock_zone_count()) return;
    const char *z = clock_zone_name((int)r);
    Config *cfg = appcfg_mut();
    snprintf(cfg->timezone, sizeof cfg->timezone, "%s", z);
    clock_set_tz(z);          /* apply now so the clock/desc update immediately */
    appcfg_save();            /* persist to SD now -> survives reboot */
    show_prefs();
}
static void show_tz_picker(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Time Zone");
    update_cat_trigger();

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 60, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, tz_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* header: current zone + live offset/DST status */
    const Config *c = appcfg();
    char desc[40]; clock_now_desc(desc, sizeof desc);
    char hdr[128];
    snprintf(hdr, sizeof hdr, "%s\n%s",
             (c->timezone[0]) ? c->timezone : "(unset)", desc);
    lv_obj_t *hl = lv_label_create(content);
    lv_label_set_text(hl, hdr);
    lv_obj_set_pos(hl, 68, 6);

    lv_obj_t *t = lv_table_create(content);
    lv_obj_set_size(t, lv_pct(100), lv_pct(78));
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(t, 0, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 4, LV_PART_ITEMS);
    lv_table_set_column_width(t, 0, LCD_W - 8);
    int n = clock_zone_count();
    for(int i=0;i<n;i++) lv_table_set_cell_value(t, i, 0, clock_zone_name(i));
    lv_obj_add_event_cb(t, tz_tbl_click_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- the Preferences list ---- */
static lv_obj_t *g_pf_bright_btn;   /* the "Brightness: NN%" row, refreshed on stepper close */
static void pf_row_open_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i == PF_TZ){ show_tz_picker(); return; }   /* zone -> picker, not text entry */
    show_pref_edit(i);
}
static void pf_pol_row_cb(lv_event_t *e){ (void)e;
    Config *c = appcfg_mut(); c->policy = (c->policy + 1) % 3; appcfg_save(); show_prefs();
}
static void pf_disc_row_cb(lv_event_t *e){ (void)e;
    if(hotsync_busy()){ alert_show("A sync is in progress; try again in a moment."); return; }
    show_discover();
}
static void pf_bright_row_cb(lv_event_t *e){ (void)e; br_open(); }
static void pf_saverow_cb(lv_event_t *e){ (void)e;
    int rc = appcfg_save();
    alert_show(rc==0 ? "Saved to config.ini" : "Could not write config.ini (SD card?)");
}
static lv_obj_t *pf_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, int ud){
    lv_obj_t *b = lv_list_add_button(list, NULL, text);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);
    return b;
}
static void show_prefs(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Preferences");
    update_cat_trigger();   /* hides the category picker (no data app) */

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    Config *c = appcfg_mut();
    char row[80];
    for(int i=0;i<PF_N;i++){
        int cap=0; const char *v = pf_buf(c, i, &cap);
        char shown[28];
        if(i==PF_WPASS || i==PF_PASS)
            snprintf(shown, sizeof shown, "%s", (v && v[0]) ? "********" : "(unset)");
        else if(v && v[0])
            snprintf(shown, sizeof shown, "%.20s%s", v, strlen(v)>20 ? "..." : "");
        else
            snprintf(shown, sizeof shown, "(unset)");
        snprintf(row, sizeof row, "%s: %s", PF_LABELS[i], shown);
        pf_add(list, row, pf_row_open_cb, i);
    }
    snprintf(row, sizeof row, "Conflicts: %s", pol_name(c->policy));
    pf_add(list, row, pf_pol_row_cb, 0);
    snprintf(row, sizeof row, "Brightness: %d%%", c->brightness);
    g_pf_bright_btn = pf_add(list, row, pf_bright_row_cb, 0);
    pf_add(list, "Discover collections...", pf_disc_row_cb, 0);
    pf_add(list, "Save to config.ini", pf_saverow_cb, 0);
}

/* ---- collection discovery screen (chunk 3) ----
 * Kicks off hotsync_discover_start() and polls hotsync_status(); when the run
 * finishes it lists the found collections. Tapping one assigns it to a role
 * (Calendar / Reminders / Address) in the in-memory config; the assignment is
 * persisted when the user taps Save back on the Preferences form. */
static lv_obj_t *g_rolepop;
static void rolepop_close(void){ if(g_rolepop){ lv_obj_del(g_rolepop); g_rolepop=NULL; } }
static void rolepop_backdrop_cb(lv_event_t *e){ (void)e; rolepop_close(); }
static void disc_show_results(void);

/* user_data packs (index<<8)|role, role in {'c'=cal,'t'=todo,'a'=addr} */
static void role_pick_cb(lv_event_t *e){
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int idx = (int)(v>>8); char role = (char)(v & 0xff);
    const DiscColl *dc = hotsync_discover_get(idx);
    rolepop_close();
    if(!dc) return;
    Config *c = appcfg_mut();
    if(role=='c')      snprintf(c->cal_coll,  sizeof c->cal_coll,  "%s", dc->href);
    else if(role=='t') snprintf(c->todo_coll, sizeof c->todo_coll, "%s", dc->href);
    else if(role=='a') snprintf(c->card_coll, sizeof c->card_coll, "%s", dc->href);
    appcfg_save();         /* persist to SD now -> survives reboot */
    disc_show_results();   /* redraw so the new [role] tag shows */
}
static void role_btn(lv_obj_t *par, const char *txt, int idx, char role){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l,txt); lv_obj_align(l,LV_ALIGN_LEFT_MID,2,0);
    lv_obj_add_event_cb(b, role_pick_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)((idx<<8)|(unsigned char)role));
}
static void disc_row_cb(lv_event_t *e){
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const DiscColl *dc = hotsync_discover_get(idx);
    if(!dc || g_rolepop) return;

    g_rolepop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_rolepop, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_rolepop, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_rolepop, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_rolepop, 0, 0);
    lv_obj_set_style_pad_all(g_rolepop, 0, 0);
    lv_obj_add_flag(g_rolepop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_rolepop, rolepop_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_rolepop);
    lv_obj_set_width(panel, 180);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
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
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hdr, 172);
    lv_label_set_text_fmt(hdr, "Use \"%s\" as:", dc->name);
    lv_obj_set_style_text_font(hdr, &lv_font_palm_bold, 0);

    if(dc->kind=='c'){
        role_btn(panel, "Calendar (Date Book)", idx, 'c');
        role_btn(panel, "Reminders (To Do)",    idx, 't');
    } else {
        role_btn(panel, "Address book",         idx, 'a');
    }
}
static void disc_back_cb(lv_event_t *e){ (void)e; show_prefs(); }

static void disc_show_results(void){
    disc_built = 1;
    if(disc_timer){ lv_timer_delete(disc_timer); disc_timer=NULL; }
    lv_obj_clean(content);
    disc_status = NULL;
    int n = hotsync_discover_count();

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, 60, 28); lv_obj_align(back, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *bl=lv_label_create(back); lv_label_set_text(bl,"Back"); lv_obj_center(bl);
    lv_obj_add_event_cb(back, disc_back_cb, LV_EVENT_CLICKED, NULL);

    if(n==0){
        lv_obj_t *l = lv_label_create(content);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LCD_W - 16);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, hotsync_status());
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_t *hint = lv_label_create(content);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -6, 8);
    lv_label_set_text(hint, "tap to assign");

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, LCD_W, FORM_FULL);
    lv_obj_set_pos(list, 0, 34);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    const Config *c = appcfg();
    for(int i=0;i<n;i++){
        const DiscColl *dc = hotsync_discover_get(i);
        const char *tag = "";
        if(dc->href[0] && !strcmp(dc->href, c->cal_coll))       tag = "  [Calendar]";
        else if(dc->href[0] && !strcmp(dc->href, c->todo_coll)) tag = "  [Reminders]";
        else if(dc->href[0] && !strcmp(dc->href, c->card_coll)) tag = "  [Address]";
        char buf[128];
        snprintf(buf,sizeof buf, "%s %s%s", dc->kind=='a'?"(A)":"(C)", dc->name, tag);
        lv_obj_t *b = lv_list_add_button(list, NULL, buf);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_add_event_cb(b, disc_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

static void disc_tick(lv_timer_t *t){ (void)t;
    if(hotsync_discover_busy()){
        if(disc_status) lv_label_set_text(disc_status, hotsync_status());
        return;
    }
    if(!disc_built) disc_show_results();   /* run finished -> show the list */
}

static void show_discover(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; disc_built = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Discover");
    update_cat_trigger();

    hotsync_discover_start();

    disc_status = lv_label_create(content);
    lv_label_set_long_mode(disc_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(disc_status, LCD_W - 16);
    lv_obj_set_style_text_align(disc_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(disc_status, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(disc_status, "Discovering...");
    disc_timer = lv_timer_create(disc_tick, 400, NULL);
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
static void act_prefs(lv_event_t *e){ (void)e; menu_close(); show_prefs(); }
static void act_toggle_done(lv_event_t *e){ (void)e; menu_close();
    g_todo_show_done = !g_todo_show_done; if(cur_app) app_reopen(cur_app); }
static void act_toggle_sort(lv_event_t *e){ (void)e; menu_close();
    g_todo_sort_due = !g_todo_sort_due; if(cur_app) app_reopen(cur_app); }

/* debug: seed 30 test appointments into the Date Book so a >24-record collection
 * can be pushed to iCloud to exercise the streaming reconcile. Each is a new
 * record (uid 0 => data layer assigns a fresh uniqueID); the next HotSync pushes
 * all of them up. C5: dev scaffolding -- only present when UI_DEVTOOLS is
 * defined (sim builds + dev firmware builds; strip the define for release). */
#ifdef UI_DEVTOOLS
static void act_gentest(lv_event_t *e){ (void)e; menu_close();
    time_t now=0; time(&now);
    struct tm base; localtime_r(&now,&base);
    if(base.tm_year+1900 < 2024){ base.tm_year=2026-1900; base.tm_mon=0; base.tm_mday=1; }
    base.tm_hour=9; base.tm_min=0; base.tm_sec=0;
    for(int i=0;i<30;i++){
        struct tm t=base; t.tm_mday += i;
        time_t tt=mktime(&t); struct tm nt; localtime_r(&tt,&nt);
        Appt a; memset(&a,0,sizeof a);
        a.year=nt.tm_year+1900; a.month=nt.tm_mon+1; a.day=nt.tm_mday;
        a.hasTime=1; a.sH=9; a.sM=0; a.eH=10; a.eM=0;
        snprintf(a.description,sizeof a.description,"Test event %d",i+1);
        snprintf(a.note,sizeof a.note,"generated for streaming-sync test");
        data_save_cal(0, 0, &a);
    }
    if(cur_app && cur_app->app==APP_CAL) list_view(cur_app);
    alert_show("Added 30 test events to Date Book.\nHotSync to push them to iCloud.");
}
#endif /* UI_DEVTOOLS */

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
    lv_label_set_text(body, "A pocket PDA that syncs to iCloud.\n"
                            "Offline by default. HotSync when\n"
                            "you want to. No feed. No ads.\n\n"
                            "v0.2 - tap to close");
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
    if(cur_app) menu_item(panel, "Categories", act_categories);
    if(cur_app && cur_app->app==APP_TODO){
        menu_item(panel, g_todo_show_done ? "Hide Completed" : "Show Completed", act_toggle_done);
        menu_item(panel, g_todo_sort_due ? "Sort by Priority" : "Sort by Due Date", act_toggle_sort);
    }
    menu_item(panel, "Preferences", act_prefs);
#ifdef UI_DEVTOOLS
    menu_item(panel, "Add test events", act_gentest);
#endif
    menu_item(panel, "About", act_about);
}

/* ------------------------- Find (global search, silkscreen) -------------
 * Palm's Find scans every app for a substring. A Graffiti query field drives
 * bridge/find.c's streaming search over all four PDBs; results list as
 * "<app>: <snippet>", and tapping one opens that record in its app. Re-runs
 * live per keystroke (small PDBs, interactive mode -> no TLS competing). */
static FindHit *g_finds;
static int      g_finds_n, g_finds_cap;
static char     g_findq[40];
static void free_finds(void){ free(g_finds); g_finds=NULL; g_finds_n=0; g_finds_cap=0; }

/* pair each app's PDB with its FIND_* code (enum orders differ) */
static const struct { int app, findapp; } FIND_DBS[] = {
    { APP_CAL, FIND_CAL }, { APP_ADDR, FIND_ADDR },
    { APP_TODO, FIND_TODO }, { APP_MEMO, FIND_MEMO },
};
static const char *find_app_label(int findapp){
    return findapp==FIND_CAL ? "Date" : findapp==FIND_TODO ? "ToDo"
         : findapp==FIND_ADDR ? "Addr" : "Memo";
}
static const AppDef *find_appdef(int findapp){
    int a = findapp==FIND_CAL ? APP_CAL : findapp==FIND_TODO ? APP_TODO
          : findapp==FIND_ADDR ? APP_ADDR : APP_MEMO;
    for(int i=0;i<NAPPDEFS;i++) if(APPDEFS[i].app==a) return &APPDEFS[i];
    return NULL;
}
static void find_collect_cb(const FindHit *h, void *ctx){
    (void)ctx;
    if(g_finds_n < g_finds_cap) g_finds[g_finds_n++] = *h;
}
static void find_click_cb(lv_event_t *e){
    lv_obj_t *t = lv_event_get_target(e);
    uint32_t r=LV_TABLE_CELL_NONE, c=LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &r, &c);
    if(r==LV_TABLE_CELL_NONE || (int)r >= g_finds_n) return;
    const AppDef *ad = find_appdef(g_finds[r].app);
    uint32_t uid = g_finds[r].uid;         /* capture before kill_kb frees g_finds */
    if(ad){ cur_app = ad; show_detail(uid); }
}
static void build_find_results(void){
    free_finds();
    if(g_findtbl){ lv_obj_del(g_findtbl); g_findtbl=NULL; }
    lv_obj_t *t = lv_table_create(content);
    g_findtbl = t;
    lv_obj_set_style_radius(t, 0, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 4, LV_PART_ITEMS);
    lv_table_set_column_width(t, 0, LCD_W - 8);
    lv_obj_set_size(t, lv_pct(100), lv_pct(84));
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(t, find_click_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if(!g_findq[0]){ lv_table_set_cell_value(t, 0, 0, "Type to search all apps"); return; }
    g_finds_cap = 60;
    g_finds = calloc(g_finds_cap, sizeof *g_finds);
    if(!g_finds){ lv_table_set_cell_value(t, 0, 0, "(low memory)"); return; }
    for(int i=0;i<(int)(sizeof FIND_DBS/sizeof FIND_DBS[0]) && g_finds_n<g_finds_cap;i++)
        find_in_pdb(data_db_path(FIND_DBS[i].app), FIND_DBS[i].findapp,
                    g_findq, find_collect_cb, NULL);
    if(g_finds_n==0){ lv_table_set_cell_value(t, 0, 0, "(no matches)"); return; }
    for(int i=0;i<g_finds_n;i++){
        char row[128];
        snprintf(row, sizeof row, "%s: %s", find_app_label(g_finds[i].app), g_finds[i].snippet);
        lv_table_set_cell_value(t, i, 0, row);
    }
}
static void findq_ta_cb(lv_event_t *e){
    lv_obj_t *ta = lv_event_get_target(e);
    snprintf(g_findq, sizeof g_findq, "%s", lv_textarea_get_text(ta));
    build_find_results();
}
static void show_find(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_findtbl = NULL; g_findq[0] = 0;
    lv_obj_clean(content);
    lv_label_set_text(title_lbl, "Find");
    update_cat_trigger();

    lv_obj_t *lb = lv_label_create(content);
    lv_label_set_text(lb, "Find:"); lv_obj_set_pos(lb, 4, 8);
    lv_obj_t *ta = lv_textarea_create(content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, sizeof g_findq - 1);
    lv_textarea_set_text(ta, "");                  /* set before cb so it doesn't fire */
    lv_obj_set_width(ta, LCD_W - 52);
    lv_obj_set_pos(ta, 46, 2);
    lv_obj_add_event_cb(ta, findq_ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    active_ta = ta;                                /* Graffiti types the query */

    build_find_results();
}

/* silkscreen buttons */
static void menu_cb(lv_event_t *e){ (void)e; menu_open(); }
static void find_cb(lv_event_t *e){ (void)e; show_find(); }

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

/* ------------------------- To Do due-date picker ------------------------- */
/* Palm's To Do due popup: quick options (Today / Tomorrow / One week / No Date)
 * plus a calendar for an arbitrary day. Modal overlay over the edit form (like
 * Details) so the typed Description/Note underneath survive the pick. */
static lv_obj_t *g_duepop;
static void due_close(void){ if(g_duepop){ lv_obj_del(g_duepop); g_duepop=NULL; } }
static void due_backdrop_cb(lv_event_t *e){ (void)e; due_close(); }

void due_set_label(void){
    if(!g_due_lbl) return;
    if(g_due_has) lv_label_set_text_fmt(g_due_lbl, "%d/%d/%d", g_due_m, g_due_d, g_due_y);
    else          lv_label_set_text(g_due_lbl, "No Date");
}

/* quick options: 0=today 1=tomorrow 2=one week 3=no date */
static void due_quick_cb(lv_event_t *e){
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if(which == 3){ g_due_has = 0; }
    else {
        time_t now = 0; time(&now);
        struct tm tmv; localtime_r(&now, &tmv);
        if(tmv.tm_year + 1900 < 2024){          /* clock unset: anchor to a sane date */
            tmv.tm_year = 2026 - 1900; tmv.tm_mon = 0; tmv.tm_mday = 1;
        }
        now = mktime(&tmv);
        now += (time_t)(which == 1 ? 1 : which == 2 ? 7 : 0) * 86400;
        localtime_r(&now, &tmv);
        g_due_has = 1;
        g_due_y = tmv.tm_year + 1900; g_due_m = tmv.tm_mon + 1; g_due_d = tmv.tm_mday;
    }
    due_set_label(); due_close();
}

static void due_cal_cb(lv_event_t *e){
    lv_obj_t *cal = (lv_obj_t *)lv_event_get_target(e);
    lv_calendar_date_t d;
    if(lv_calendar_get_pressed_date(cal, &d) == LV_RESULT_OK){
        g_due_has = 1; g_due_y = d.year; g_due_m = d.month; g_due_d = d.day;
        due_set_label(); due_close();
    }
}

static void due_quick_btn(lv_obj_t *par, const char *txt, int which){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(48));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_center(l);
    lv_obj_add_event_cb(b, due_quick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
}

void due_open(void){
    if(g_duepop) return;
    g_duepop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_duepop, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_duepop, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_duepop, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_duepop, 0, 0);
    lv_obj_set_style_pad_all(g_duepop, 0, 0);
    lv_obj_add_flag(g_duepop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_duepop, due_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_duepop);
    lv_obj_set_width(panel, LCD_W - 20);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LCD_H - 16, 0);
    lv_obj_center(panel);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_flex_main_place(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 3, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_label_set_text(hdr, "Due Date:");
    lv_obj_set_style_text_font(hdr, &lv_font_palm_bold, 0);

    due_quick_btn(panel, "Today",    0);
    due_quick_btn(panel, "Tomorrow", 1);
    due_quick_btn(panel, "1 Week",   2);
    due_quick_btn(panel, "No Date",  3);

    /* calendar for an arbitrary day, seeded to the current due (or today) */
    lv_obj_t *cal = lv_calendar_create(panel);
    lv_obj_set_width(cal, lv_pct(100));
    lv_obj_set_height(cal, 180);
    int sy = g_due_has ? g_due_y : 2026, sm = g_due_has ? g_due_m : 1, sd = g_due_has ? g_due_d : 1;
    time_t now = 0; time(&now); struct tm tmv;
    localtime_r(&now, &tmv);
    if(tmv.tm_year + 1900 >= 2024){
        lv_calendar_set_today_date(cal, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        if(!g_due_has){ sy = tmv.tm_year + 1900; sm = tmv.tm_mon + 1; sd = tmv.tm_mday; }
    }
    lv_calendar_set_showed_date(cal, sy, sm);
    lv_calendar_header_arrow_create(cal);
    lv_obj_add_event_cb(cal, due_cal_cb, LV_EVENT_VALUE_CHANGED, NULL);
    (void)sd;
}
static void due_btn_cb(lv_event_t *e){ (void)e; due_open(); }

/* ------------------------- Preferences: brightness stepper ------------------------- */
/* A [ - ]  75%  [ + ] popup that live-adjusts the backlight and persists on close.
 *
 * NOT an lv_slider: lv_slider derives from lv_bar (.base_class = &lv_bar_class),
 * and a bar's indicator forces LVGL to allocate a draw-LAYER buffer from the fixed
 * 24 KB object pool. On the no-PSRAM device (and in the 24 KB-pool wasm sim) that
 * allocation can fail with the pool already full from the Preferences list, and
 * LVGL then spins retrying the draw every refresh -> IDLE0 starves -> Task WDT ->
 * frozen screen. This is the exact failure documented at hs_tick (why HotSync
 * progress is text, not an lv_bar). Plain buttons never allocate a layer -- the
 * same reason the Calculator and the on-screen keyboard use a button matrix -- so
 * the stepper is pool-safe. Brightness floors at 10% so it can't go fully dark. */
static lv_obj_t *g_brpop, *g_br_val;
static int g_br_val_cur, g_br_val_orig;
static void br_close(void){
    if(!g_brpop) return;
    if(g_br_val_cur != g_br_val_orig){            /* persist once, on close */
        appcfg_mut()->brightness = g_br_val_cur; appcfg_save();
        /* refresh the underlying "Brightness: NN%" row (the popup is a layer_top
         * overlay, so the Preferences list beneath it is still the live screen) */
        if(g_pf_bright_btn){
            lv_obj_t *lbl = lv_obj_get_child_by_type(g_pf_bright_btn, 0, &lv_label_class);
            if(lbl) lv_label_set_text_fmt(lbl, "Brightness: %d%%", g_br_val_cur);
        }
    }
    lv_obj_del(g_brpop); g_brpop=NULL; g_br_val=NULL;
}
static void br_backdrop_cb(lv_event_t *e){ (void)e; br_close(); }
static void br_set(int v){
    if(v < 10) v = 10;
    if(v > 100) v = 100;
    g_br_val_cur = v;
    power_set_brightness(v);                          /* live preview */
    if(g_br_val) lv_label_set_text_fmt(g_br_val, "%d%%", v);
}
static void br_minus_cb(lv_event_t *e){ (void)e; br_set(g_br_val_cur - 10); }
static void br_plus_cb (lv_event_t *e){ (void)e; br_set(g_br_val_cur + 10); }
static lv_obj_t *br_step_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 46);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, &lv_font_palm_bold, 0);
    lv_obj_center(l);
    return b;
}
void br_open(void){
    if(g_brpop) return;
    int cur = appcfg()->brightness;
    if(cur < 10) cur = 10;
    g_br_val_orig = g_br_val_cur = cur;

    g_brpop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_brpop, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(g_brpop, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_brpop, LV_OPA_30, 0);
    lv_obj_set_style_border_width(g_brpop, 0, 0);
    lv_obj_set_style_pad_all(g_brpop, 0, 0);
    lv_obj_add_flag(g_brpop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_brpop, br_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(g_brpop);
    lv_obj_set_width(panel, LCD_W - 30);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(panel, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_LINE, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_label_set_text(hdr, "Brightness");
    lv_obj_set_style_text_font(hdr, &lv_font_palm_bold, 0);

    /* [ - ]  NN%  [ + ] -- a transparent flex row of plain buttons + the value */
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

    br_step_btn(row, "-", br_minus_cb);
    g_br_val = lv_label_create(row);
    lv_obj_set_width(g_br_val, 52);
    lv_obj_set_style_text_align(g_br_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(g_br_val, "%d%%", cur);
    br_step_btn(row, "+", br_plus_cb);
}

/* ------------------------- U6: Graffiti stroke capture ------------------------- */

/* C1: the ink trail. Real Graffiti showed your stroke; without it the user
 * can't tell whether a stroke registered or where it went wrong. An I1 canvas
 * (2 colors) sits BEHIND the writing pads: 168x106 @ 1bpp is ~2.3 KB in BSS --
 * off the LVGL pool, cheap enough for the no-PSRAM device. Ink is drawn with
 * direct set_px (Bresenham, 2px pen) -- no draw-layer allocation, so it can
 * never hit the mid-sync layer-alloc failure documented at show_hotsync. The
 * ink fades (clears) shortly after pen-up, like the real thing. */
#define INK_X0 36                        /* clear of the 30px silkscreen buttons */
#define INK_Y0 3
#define INK_W  (LCD_W - 2*INK_X0)
#define INK_H  (GRAFFITI_H - 6)
static lv_obj_t  *ink_canvas;
static uint8_t    ink_buf[LV_CANVAS_BUF_SIZE(INK_W, INK_H, 1, 1) + 16]; /* +palette */
static int        ink_lx = -1, ink_ly = -1;   /* last canvas-local point (-1 = pen up) */
static lv_timer_t *ink_fade;

static void ink_clear(void){
    if(!ink_canvas) return;
    lv_color_t bg = { .blue = 0 };       /* indexed canvas: palette index in .blue */
    lv_canvas_fill_bg(ink_canvas, bg, LV_OPA_COVER);
}
static void ink_fade_cb(lv_timer_t *t){ (void)t; ink_clear(); ink_fade = NULL; }
static void ink_fade_start(void){
    if(ink_fade){ lv_timer_delete(ink_fade); }
    ink_fade = lv_timer_create(ink_fade_cb, 450, NULL);
    lv_timer_set_repeat_count(ink_fade, 1);
}
static void ink_point(int sx, int sy){
    if(!ink_canvas) return;
    int x = sx - INK_X0, y = sy - (PDA_H + INK_Y0);
    if(x < 0 || y < 0 || x >= INK_W || y >= INK_H){ ink_lx = -1; return; }
    if(ink_lx < 0){ ink_lx = x; ink_ly = y; }
    int x0 = ink_lx, y0 = ink_ly;
    int dx = x > x0 ? x - x0 : x0 - x, sx_ = x0 < x ? 1 : -1;
    int dy = y > y0 ? y0 - y : y - y0, sy_ = y0 < y ? 1 : -1;   /* dy <= 0 */
    int err = dx + dy;
    lv_color_t ink = { .blue = 1 };
    for(;;){
        lv_canvas_set_px(ink_canvas, x0, y0, ink, LV_OPA_COVER);
        if(x0 + 1 < INK_W) lv_canvas_set_px(ink_canvas, x0 + 1, y0, ink, LV_OPA_COVER);
        if(y0 + 1 < INK_H) lv_canvas_set_px(ink_canvas, x0, y0 + 1, ink, LV_OPA_COVER);
        if(x0 == x && y0 == y) break;
        int e2 = 2 * err;
        if(e2 >= dy){ err += dy; x0 += sx_; }
        if(e2 <= dx){ err += dx; y0 += sy_; }
    }
    ink_lx = x; ink_ly = y;
}

/* echo the recognized character in the strip for a moment (Palm-style feedback) */
static lv_obj_t  *graf_echo_lbl;
static lv_timer_t *echo_timer;
static void echo_clear_cb(lv_timer_t *t){ (void)t;
    if(graf_echo_lbl) lv_label_set_text(graf_echo_lbl, "");
    echo_timer = NULL;
}
static void graf_echo(char c){
    if(!graf_echo_lbl || c < ' ' || c > 126) return;
    char s[2] = { c, 0 };
    lv_label_set_text(graf_echo_lbl, s);
    if(echo_timer) lv_timer_delete(echo_timer);
    echo_timer = lv_timer_create(echo_clear_cb, 600, NULL);
    lv_timer_set_repeat_count(echo_timer, 1);
}

static void graf_down_cb(lv_event_t *e){ (void)e;
    graffiti_clear();
    if(ink_fade){ lv_timer_delete(ink_fade); ink_fade = NULL; }
    ink_clear();
    ink_lx = -1;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    ink_point(p.x, p.y);
}
static void graf_move_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    graffiti_add_point(p.x, p.y);
    ink_point(p.x, p.y);
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
/* punctuation-shift indicator: a tap arms "the next stroke is punctuation", shown
 * here so the user knows the mode is active (like PalmOS's shift dot). */
static lv_obj_t *graf_punct_lbl;
static void show_punct(int on){
    if(graf_punct_lbl) lv_label_set_text(graf_punct_lbl, on ? "PUNC" : "");
}
/* user_data: 0 = letters (abc pad), 1 = digits (123 pad) */
static void graf_up_cb(lv_event_t *e){
    int digits = (int)(intptr_t)lv_event_get_user_data(e);
    ink_lx = -1;
    ink_fade_start();                                  /* ink lingers, then clears */
    char c = graffiti_recognize(digits);
    if(c) graf_echo(c);                                /* flash what was recognized */
    if(!c){ show_punct(0); return; }                   /* nothing / punct rejected */
    if(c == GRAF_SHIFT){ graf_case = (graf_case + 1) % 3; show_case(); return; }
    if(c == GRAF_PUNCT){ show_punct(1); return; }       /* tap: arm punctuation */
    show_punct(0);                                      /* any real char clears it */
    if(!active_ta){ graf_case = CASE_NONE; show_case(); return; }
    if(c == '\b'){                                     /* backspace: keep caps lock */
        lv_textarea_delete_char(active_ta);
        if(graf_case == CASE_SHIFT){ graf_case = CASE_NONE; show_case(); }
        return;
    }
    if(graf_case != CASE_NONE && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    lv_textarea_add_char(active_ta, c);                /* letter, digit, punct, space, '\n' */
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

/* refresh the title-bar clock: 12h time + date to its right ("12:34p  Jul 10").
 * Persists across screen swaps since it lives on the title bar, not content. */
static void clock_tick(lv_timer_t *t){
    (void)t;
    if(!clock_lbl) return;
    time_t now=0; time(&now);
    struct tm ti; localtime_r(&now, &ti);
    int h = ti.tm_hour % 12; if(h==0) h = 12;
    char b[24];
    snprintf(b, sizeof b, "%d:%02d%s  %s %d",
             h, ti.tm_min, ti.tm_hour < 12 ? "a" : "p",
             CAL_MON[ti.tm_mon + 1], ti.tm_mday);
    lv_label_set_text(clock_lbl, b);
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
    lv_obj_set_style_text_color(title_lbl, COL_TITLE_FG, 0);   /* white on black (Palm) */
    lv_obj_set_style_text_font(title_lbl, &lv_font_palm_bold, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 6, 0);

    /* live clock, centered in the title bar (Palm shows the time up top). Titles
     * are left-aligned + short and the category trigger is far right, so center
     * stays clear. Refreshed every 15 s by an lv_timer. */
    clock_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(clock_lbl, COL_TITLE_FG, 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, 0);
    clock_tick(NULL);
    lv_timer_create(clock_tick, 15000, NULL);

    /* F2: category pop-up trigger (top-right, Palm convention) */
    cat_trigger = lv_button_create(bar);
    lv_obj_set_height(cat_trigger, TITLE_H - 4);
    lv_obj_align(cat_trigger, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_radius(cat_trigger, 0, 0);
    lv_obj_set_style_pad_hor(cat_trigger, 4, 0);
    lv_obj_set_style_bg_color(cat_trigger, COL_TITLE, 0);      /* blend into the black bar */
    lv_obj_set_style_border_width(cat_trigger, 1, 0);
    lv_obj_set_style_border_color(cat_trigger, COL_TITLE_FG, 0);
    lv_obj_add_event_cb(cat_trigger, cat_trigger_cb, LV_EVENT_CLICKED, NULL);
    cat_label = lv_label_create(cat_trigger);
    lv_obj_set_style_text_color(cat_label, COL_TITLE_FG, 0);   /* white on black */
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

    /* C1: the ink canvas sits UNDER the pads (created first = behind); the pads
     * stay the clickable surfaces and feed both the recognizer and the ink. */
    ink_canvas = lv_canvas_create(graf);
    lv_canvas_set_buffer(ink_canvas, ink_buf, INK_W, INK_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(ink_canvas, 0, lv_color_to_32(COL_GRAF, 0xFF));
    lv_canvas_set_palette(ink_canvas, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_set_pos(ink_canvas, INK_X0, INK_Y0);
    lv_obj_clear_flag(ink_canvas, LV_OBJ_FLAG_CLICKABLE);
    ink_clear();

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

    /* punctuation-shift indicator (top-centre of the strip; empty until armed) */
    graf_punct_lbl = lv_label_create(graf);
    lv_label_set_text(graf_punct_lbl, "");
    lv_obj_set_style_text_font(graf_punct_lbl, &lv_font_palm_bold, 0);
    lv_obj_align(graf_punct_lbl, LV_ALIGN_TOP_MID, 0, 1);

    /* recognized-character echo (bottom-centre; flashes for ~600 ms per stroke) */
    graf_echo_lbl = lv_label_create(graf);
    lv_label_set_text(graf_echo_lbl, "");
    lv_obj_set_style_text_font(graf_echo_lbl, &lv_font_palm_bold, 0);
    lv_obj_align(graf_echo_lbl, LV_ALIGN_BOTTOM_MID, 0, -1);

    show_launcher();
}
