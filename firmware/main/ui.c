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
#include "news.h"         /* RSS reader's on-SD article store */
#include "feeds.h"        /* RSS feed list (Preferences manager + HotSync fetch) */
#include "lv_font_kana.h" /* hiragana+katakana bitmap subset (Kana trainer) */
#include "kana_data.h"    /* ordered gojuon table (Kana trainer, roadmap #3) */
#include "kana_strokes.h" /* per-kana stroke polylines (Tier 2 writing challenge) */
#include "kana_write.h"   /* per-stroke $1 matcher (Tier 2) */
#include "appcfg.h"
#include "power.h"        /* live backlight brightness + battery gauge for the dashboard */
#include "clock.h"        /* timezone picker + DST-aware zone list + world-time helper */
#include "dash.h"         /* lock-screen dashboard: weather cache + moon/sun math */
#include "minesweeper.h"  /* Games: Minesweeper board logic */
#include "wordie.h"       /* Games: Wordie word-game logic */
#include "sudoku.h"       /* Games: Sudoku board logic */
#include "zip.h"          /* Games: Zip path-puzzle logic */
#include "playclock.h"    /* Games: pausable play timer (shared by Mines/Sudoku/Zip) */
#include "coach.h"        /* Coach: ritual focus timer (pure logic + rule engine) */
#include "lvgl.h"
#include <string.h>
#include <strings.h>      /* strncasecmp for the Address Look Up filter */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>         /* sqrt() for the dashboard moon disc */

#define TITLE_H     24
#define FEEDS_PATH  "/sdcard/feeds.txt"       /* News reader's RSS source list */
#define COL_TITLE   lv_color_hex(0x000000)   /* black title bar (Palm: white-on-black) */
#define COL_TITLE_FG lv_color_hex(0xFFFFFF)  /* title text/glyphs on the black bar */
#define COL_BODY    lv_color_hex(0xFFFFFF)   /* white app body   */
#define COL_GRAF    lv_color_hex(0xD6D6D6)   /* graffiti strip   */
#define COL_LINE    lv_color_hex(0x000000)   /* black rules      */

static lv_obj_t *content;      /* the swappable view area */
/* Tear down the content area. ALWAYS use this instead of lv_obj_clean(content):
 * it nulls every global that points into it, in the same breath that frees them.
 * Defined at the bottom of the file, where all of those globals are in scope. */
static void content_clear(void);
static lv_obj_t *title_lbl;
static lv_obj_t *clock_lbl;    /* live clock in the title bar (Palm) */

/* Kana is NOT a top-level app -- it lives inside Graffiti (a handwriting sibling of
 * the Latin drill), reached by the "あ" button there. Keeps the launcher focused. */
static const char *APPS[] = { "Date Book", "Address", "To Do List", "Memo Pad", "HotSync", "Graffiti", "News", "Games", "Coach" };
/* authentic Palm app launcher icons (from PumpkinOS) */
static const lv_image_dsc_t *APP_ICONS[] = { &icon_datebook, &icon_address,
                                             &icon_todo, &icon_memo, &icon_hotsync,
                                             &icon_graffiti, &icon_news, &icon_games,
                                             &icon_coach };
#define NAPPS ((int)(sizeof(APPS)/sizeof(APPS[0])))

static void show_launcher(void);
static void show_trainer(void);
static void show_kana(void);
static void show_news(void);
static void show_games(void);
static void show_coach(void);
static void co_save(void);        /* persist Coach state (defined with the app) */
static void show_coach_marks(void);
static void show_coach_report(void);
static void co_show_sigil(void);
static void show_minesweeper(void);
static void show_wordie(void);
static void show_sudoku(void);
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

/* ---- shared I1 canvas direct-write helpers --------------------------------
 * Every canvas in this file is I1 (1 bpp indexed: palette 0 = background,
 * 1 = ink). Painting them through lv_canvas_set_px is pathological on this
 * board: set_px ends EVERY pixel with lv_obj_invalidate(), which walks the
 * object tree and rescans the display's invalid-area list, and for indexed
 * formats lv_canvas_fill_bg is itself just a set_px loop. Measured on device
 * with the 168x106 Graffiti pad: 80 ms for one clear during ui_init and
 * 1.006 SECONDS once the full UI was live. Because that runs inside
 * lv_timer_handler -- the same loop that polls touch -- it also swallows the
 * input that triggered the repaint.
 *
 * These reproduce LVGL's own I1 addressing (bit 7-(x&7) of the byte at
 * goto_xy) and deliberately do NOT invalidate; each paint routine issues
 * exactly one lv_obj_invalidate() when it is done. */
static void i1_px(lv_draw_buf_t *db, int x, int y, int v){
    if(!db || x < 0 || y < 0 ||
       x >= (int)db->header.w || y >= (int)db->header.h) return;
    uint8_t *p = lv_draw_buf_goto_xy(db, (uint32_t)x, (uint32_t)y);
    if(!p) return;
    uint8_t m = (uint8_t)(1u << (7 - (x & 7)));
    if(v) *p |= m; else *p &= (uint8_t)~m;
}
static void i1_clear(lv_draw_buf_t *db){          /* fill with palette index 0 */
    if(!db) return;
    uint8_t *p0 = lv_draw_buf_goto_xy(db, 0, 0);
    if(p0) lv_memset(p0, 0x00, (size_t)db->header.stride * db->header.h);
}
/* object-taking wrappers, for the paint code that only has the canvas widget */
static void i1_obj_px(lv_obj_t *cv, int x, int y, int v){
    if(cv) i1_px(lv_canvas_get_draw_buf(cv), x, y, v);
}
static void i1_obj_clear(lv_obj_t *cv){
    if(cv) i1_clear(lv_canvas_get_draw_buf(cv));
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

/* Date Book Details: alarm on/off + repeat type, edited in the Details sheet and
 * applied to the Appt on Save (the codec already round-trips VALARM/RRULE). */
static int g_ev_alarm, g_ev_repeat;
static const char *repeat_name(int rt){
    switch(rt){
        case repeatDaily:  return "Daily";
        case repeatWeekly: return "Weekly";
        case repeatMonthlyByDay: case repeatMonthlyByDate: return "Monthly";
        case repeatYearly: return "Yearly";
        default:           return "None";
    }
}
static int repeat_next(int rt){   /* cycle None->Daily->Weekly->Monthly->Yearly->None */
    switch(rt){
        case repeatNone:   return repeatDaily;
        case repeatDaily:  return repeatWeekly;
        case repeatWeekly: return repeatMonthlyByDate;
        case repeatMonthlyByDate: case repeatMonthlyByDay: return repeatYearly;
        default:           return repeatNone;
    }
}

static void list_view(const AppDef *ad);
static void show_detail(uint32_t uid);
static void show_edit(uint32_t uid);
static void show_prefs(void);
static void show_dash_settings(void);                          /* Lock Screen settings sub-screen */
static void world_tag(const char *zone, char *out, int cap);   /* 3-letter world-clock tag */
static lv_obj_t *pf_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, int ud);
static void show_discover(void);
static void show_feeds(void);
static void show_feed_edit(int idx);
static void update_cat_trigger(void);
static void cat_trigger_cb(lv_event_t *e);
static void details_open(void);
static void toast_show(const char *msg);   /* I4: transient save/delete feedback */
static void due_open(void);
static void due_btn_cb(lv_event_t *e);
static void due_set_label(void);
static void br_open(void);

/* Date Book uses PalmOS's date-centric views (Day/Week/Month) instead of a flat
 * list of every event -- see show_datebook_day / show_datebook_week /
 * show_datebook_month. g_cal_* holds the currently-viewed day so navigation +
 * "return from a record" land back on it. Tapping the centre label of a view
 * zooms out one level (Day -> Week -> Month); a day tap zooms back in. */
static void show_datebook_day(int y, int m, int d);
static void show_datebook_week(int y, int m, int d);
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
/* Graffiti input hooks (the trainer). graf_char_hook: a recognized character goes
 * here instead of the active textarea (drill mode). graf_capture_hook: runs on
 * pen-up BEFORE recognition, so the raw stroke can be captured as a user template
 * (train mode); returns 1 if it consumed the stroke. Both cleared on every screen
 * teardown so they can't outlive the trainer. */
static void (*graf_char_hook)(char c);
static int  (*graf_capture_hook)(void);
static int  g_trainer_open;      /* the Graffiti trainer is the live screen (menu Reset) */
static int  g_kana_open;         /* the Kana trainer is the live screen (menu Reset) */
static int  g_ms_active;         /* the Mines screen is live (1 Hz timer tick guard) */
static int  g_sd_active;         /* the Sudoku screen is live (1 Hz timer tick guard) */
static int  g_zp_active;         /* the Zip screen is live (1 Hz timer tick guard)    */
/* ---- Coach (see the implementation block above ui_init) -------------------
 * These live up here, with the other cross-screen state, because content_clear()
 * is defined further down and has to be able to null every one of them. */
static CoachState g_co;              /* durable state; mirrors /sdcard/coach.sav */
static CoachSigil g_co_sig;          /* the live session's mark                  */
static int  g_co_loaded;             /* coach.sav has been read this boot        */
static int  g_co_open;               /* a Coach screen is the live view (menu)   */
static int  g_co_step;               /* ritual step 0..2                         */
static int  g_co_pick_en, g_co_pick_dom, g_co_pick_int;
static int  g_co_res, g_co_blk;      /* the reflect answers                      */
static int  g_co_last_fill;          /* last painted fill %, so we repaint 1/min */
static uint32_t g_co_last_start;     /* the just-finished session, for the exports */
static uint16_t g_co_last_min;
static uint32_t g_co_hold_start;     /* give-up press-and-hold start (lv ticks)  */
static lv_obj_t *g_co_cv, *g_co_time, *g_co_sub, *g_co_status, *g_co_hold_lbl;
static lv_obj_t *g_co_len_lbl, *g_co_goal_lbl;   /* the two live rows on the menu */
enum { CO_VIEW_HOME, CO_VIEW_MARKS, CO_VIEW_WEEK, CO_VIEW_OTHER };
static int  g_co_view;               /* which Coach screen `content` is showing  */
static lv_obj_t *g_co_seal;          /* the sealed takeover root, or NULL        */
static int  g_co_reflect;            /* the reflect -> blocker -> note flow is up */

/* 1 while Coach owns the screen and nothing may cover it: a sealed session, or the
 * reflect flow that follows one. The lock screen is suppressed for both -- someone
 * who put the device down mid-session and picked it up again has to land on "how
 * did it go", not on a dashboard that has already thrown that screen away. */
static int co_owns_screen(void){
    return g_co_seal != NULL || g_co.phase == CO_PH_REFLECT || g_co_reflect;
}
/* Bank + persist the open game's play clock. Every screen teardown goes through
 * kill_kb(), which makes it the one place that reliably means "you left the game"
 * -- so the timers stop there and restart in the matching show_*(). */
static void games_pause_clocks(void);
static void kill_kb(void){
    games_pause_clocks();        /* BEFORE the active flags are cleared below */
    g_form=NULL; active_ta=NULL; edit_cat_lbl=NULL; g_listtbl=NULL; g_findtbl=NULL;
    graf_char_hook=NULL; graf_capture_hook=NULL;
    g_trainer_open=0; g_kana_open=0; g_ms_active=0; g_sd_active=0; g_zp_active=0;
    g_co_open=0; g_co_view=CO_VIEW_OTHER; g_co_reflect=0;
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
    content_clear();
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
    if(a && u){ data_delete(a->app, u); cur_uid = 0; app_reopen(a); toast_show("Deleted"); }
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

    content_clear();
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
        a.hasAlarm = g_ev_alarm;                          /* Details sheet: alarm + repeat */
        if(g_ev_alarm && a.alarmAdv <= 0){ a.alarmAdv = 5; a.alarmUnit = 0; }  /* default 5 min */
        if(g_ev_repeat == repeatNone){ a.hasRepeat = 0; a.repeatType = repeatNone; }
        else { a.hasRepeat = 1; a.repeatType = g_ev_repeat; if(a.repeatFreq <= 0) a.repeatFreq = 1; }
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
        a.fields[F_title]=iv(&a,fv(2));
        a.fields[F_company]=iv(&a,fv(3));
        a.fields[F_phone1]=iv(&a,fv(4)); a.phoneLabel[0]=have?old.phoneLabel[0]:workLabel;
        a.fields[F_address]=iv(&a,fv(5));
        a.fields[F_city]=iv(&a,fv(6));
        a.fields[F_state]=iv(&a,fv(7));
        a.fields[F_zip]=iv(&a,fv(8));
        a.fields[F_note]=iv(&a,fv(9));
        if(have){   /* preserve fields the form still doesn't expose */
            for(int k=1;k<5;k++) if(old.fields[F_phone1+k]){ a.fields[F_phone1+k]=iv(&a,old.fields[F_phone1+k]); a.phoneLabel[k]=old.phoneLabel[k]; }
            static const int keep[]={F_country,F_custom1,F_custom2,F_custom3,F_custom4};
            for(unsigned k=0;k<sizeof keep/sizeof keep[0];k++) if(old.fields[keep[k]]) a.fields[keep[k]]=iv(&a,old.fields[keep[k]]);
            a.displayPhone=old.displayPhone;
        }
        data_save_addr(edit_uid,edit_cat,&a);
    } else if(cur_app->app == APP_MEMO){
        data_save_memo(edit_uid,edit_cat,fv(0));
    }
    toast_show("Saved");
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
    content_clear();
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
        g_ev_alarm  = a.hasAlarm;                              /* Details sheet state */
        g_ev_repeat = a.hasRepeat ? a.repeatType : repeatNone;
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
        /* the scrollable form now exposes the common Palm Address fields (was just
         * 5); fv() indices below must stay in lock-step with save_cb's APP_ADDR arm */
        form_field(form,"Last",a.fields[F_name],40,&y);       /* fv0 */
        form_field(form,"First",a.fields[F_firstName],40,&y); /* fv1 */
        form_field(form,"Title",a.fields[F_title],40,&y);     /* fv2 */
        form_field(form,"Company",a.fields[F_company],60,&y); /* fv3 */
        form_field(form,"Phone",a.fields[F_phone1],40,&y);    /* fv4 */
        form_field(form,"Address",a.fields[F_address],60,&y); /* fv5 */
        form_field(form,"City",a.fields[F_city],40,&y);       /* fv6 */
        form_field(form,"State",a.fields[F_state],20,&y);     /* fv7 */
        form_field(form,"Zip",a.fields[F_zip],20,&y);         /* fv8 */
        form_field(form,"Note",a.fields[F_note],200,&y);      /* fv9 */
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
    content_clear();
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
static void day_week_cb(lv_event_t *e){ (void)e; show_datebook_week(g_cal_y,g_cal_m,g_cal_d); }  /* zoom out to the week */

static void show_datebook_day(int y,int m,int d){
    kill_kb();
    g_cal_y=y; g_cal_m=m; g_cal_d=d;
    cur_app=&APPDEFS[0]; cur_uid=0;     /* Date Book context (menu New/Categories) */
    content_clear();
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
    lv_obj_add_event_cb(mon,day_week_cb,LV_EVENT_CLICKED,NULL);   /* tap the date -> Week view */

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

/* --- Week view (7-day agenda: one scrollable list, a count per day, today tinted).
 * Sits between Day and Month in the zoom hierarchy: tap a day row to zoom in to
 * that Day, tap the centre range label to zoom out to the Month. Pool-safe --
 * plain list buttons + labels, no draw-layer widgets. g_wk_* is the week start
 * (its Sunday) so prev/next page whole weeks. */
static int g_wk_y, g_wk_m, g_wk_d;
static struct { int y,m,d; } g_wkdays[7];
static int g_wk_count;
static void wk_count_cb(uint32_t uid,const char *p,const char *s,void *ctx){
    (void)uid;(void)p;(void)s;(void)ctx; g_wk_count++;
}
static void wk_day_cb(lv_event_t *e){
    int i=(int)(uintptr_t)lv_event_get_user_data(e);
    show_datebook_day(g_wkdays[i].y,g_wkdays[i].m,g_wkdays[i].d);
}
static void week_prev_cb(lv_event_t *e){ (void)e; cal_add_days(&g_wk_y,&g_wk_m,&g_wk_d,-7); show_datebook_week(g_wk_y,g_wk_m,g_wk_d); }
static void week_next_cb(lv_event_t *e){ (void)e; cal_add_days(&g_wk_y,&g_wk_m,&g_wk_d, 7); show_datebook_week(g_wk_y,g_wk_m,g_wk_d); }
static void week_month_cb(lv_event_t *e){ (void)e; show_datebook_month(g_wk_y,g_wk_m); }

static void show_datebook_week(int y,int m,int d){
    kill_kb();
    cur_app=&APPDEFS[0]; cur_uid=0;
    /* snap (y,m,d) back to the Sunday that starts its week */
    g_wk_y=y; g_wk_m=m; g_wk_d=d;
    cal_add_days(&g_wk_y,&g_wk_m,&g_wk_d, -cal_wday(y,m,d));
    content_clear();
    lv_label_set_text(title_lbl,"Week");
    update_cat_trigger();

    /* materialise the 7 days up front (used by the range label and each row) */
    for(int i=0;i<7;i++){
        g_wkdays[i].y=g_wk_y; g_wkdays[i].m=g_wk_m; g_wkdays[i].d=g_wk_d;
        cal_add_days(&g_wkdays[i].y,&g_wkdays[i].m,&g_wkdays[i].d, i);
    }

    lv_obj_t *prev=lv_button_create(content); lv_obj_set_size(prev,38,28); lv_obj_align(prev,LV_ALIGN_TOP_LEFT,2,2);
    lv_obj_t *pl=lv_label_create(prev); lv_label_set_text(pl,"<"); lv_obj_center(pl);
    lv_obj_add_event_cb(prev,week_prev_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *next=lv_button_create(content); lv_obj_set_size(next,38,28); lv_obj_align(next,LV_ALIGN_TOP_RIGHT,-2,2);
    lv_obj_t *nl=lv_label_create(next); lv_label_set_text(nl,">"); lv_obj_center(nl);
    lv_obj_add_event_cb(next,week_next_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *rng=lv_button_create(content); lv_obj_set_size(rng,150,28); lv_obj_align(rng,LV_ALIGN_TOP_MID,0,2);
    lv_obj_t *rl=lv_label_create(rng); lv_obj_center(rl);
    if(g_wkdays[0].m==g_wkdays[6].m)
        lv_label_set_text_fmt(rl,"%s %d-%d",CAL_MON[g_wkdays[0].m],g_wkdays[0].d,g_wkdays[6].d);
    else
        lv_label_set_text_fmt(rl,"%s %d - %s %d",CAL_MON[g_wkdays[0].m],g_wkdays[0].d,CAL_MON[g_wkdays[6].m],g_wkdays[6].d);
    lv_obj_add_event_cb(rng,week_month_cb,LV_EVENT_CLICKED,NULL);   /* tap the range -> Month view */

    int ty,tm,td; cal_today(&ty,&tm,&td);
    lv_obj_t *list=lv_list_create(content);
    lv_obj_set_size(list,LCD_W,FORM_FULL);
    lv_obj_set_pos(list,0,34);
    lv_obj_set_style_radius(list,0,0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,0,0);
    for(int i=0;i<7;i++){
        g_wk_count=0;
        data_cal_day(g_wkdays[i].y,g_wkdays[i].m,g_wkdays[i].d,wk_count_cb,NULL);
        char row[48];
        if(g_wk_count==0)
            snprintf(row,sizeof row,"%s %d/%d",CAL_WD[cal_wday(g_wkdays[i].y,g_wkdays[i].m,g_wkdays[i].d)],g_wkdays[i].m,g_wkdays[i].d);
        else
            snprintf(row,sizeof row,"%s %d/%d   %d event%s",
                     CAL_WD[cal_wday(g_wkdays[i].y,g_wkdays[i].m,g_wkdays[i].d)],g_wkdays[i].m,g_wkdays[i].d,
                     g_wk_count, g_wk_count==1?"":"s");
        lv_obj_t *b=lv_list_add_button(list,NULL,row);
        lv_obj_set_style_radius(b,0,0);
        if(g_wkdays[i].y==ty && g_wkdays[i].m==tm && g_wkdays[i].d==td)
            lv_obj_set_style_bg_color(b,COL_GRAF,0);   /* tint today's row (bg fill only -> pool-safe) */
        lv_obj_add_event_cb(b,wk_day_cb,LV_EVENT_CLICKED,(void*)(uintptr_t)i);
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
    content_clear();
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
    if(!strcmp(name, "Graffiti")){ show_trainer(); return; }
    if(!strcmp(name, "News")){ show_news(); return; }
    if(!strcmp(name, "Games")){ show_games(); return; }
    if(!strcmp(name, "Coach")){ show_coach(); return; }
    cur_app = NULL;
    content_clear();
    lv_label_set_text(title_lbl, name);
    lv_obj_t *l = lv_label_create(content);
    lv_label_set_text_fmt(l, "%s\n\n(coming soon)", name);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

/* ===================== Graffiti Trainer (roadmap #2) =========================
 * A learn-to-write drill: shows a target glyph + its stroke guide (drawn from the
 * recognizer's own template), you draw it in the Graffiti strip, and it scores the
 * stroke and schedules the next with a DETERMINISTIC spaced-repetition system --
 * never random. Each glyph has a level 1..5 and a due "tick"; a correct stroke
 * promotes it a level (longer interval -> shows up less often) and, past level 5,
 * BURNS it (retired until Menu > Reset progress). A wrong stroke demotes a level
 * and reschedules it immediately (shows up more often). The next glyph is always
 * the non-burned one with the smallest due tick (ties by order) -- fully
 * reproducible. The set is letters + digits + punctuation. Progress persists to
 * SD. Pool-safe: labels + one I1 canvas for the guide, no layer-alloc widgets.
 * Input arrives through graf_char_hook (set on entry, cleared by kill_kb). */
#define TR_GW 96
#define TR_GH 96
#define TR_USER "/sdcard/graf_user.dat"
static uint8_t   tr_guide_buf[LV_CANVAS_BUF_SIZE(TR_GW, TR_GH, 1, 1) + 16];
static lv_obj_t *tr_guide, *tr_prompt, *tr_score, *tr_feedback, *tr_mode_lbl;

/* the trainable glyph set. Letters lead (indices 0..25) so Train mode -- which
 * captures per-device templates and is letters-only -- can index them directly. */
static const char TR_G[] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '@',',','/','-','\'','(',')','?','.'
};
#define TR_NG     ((int)(sizeof TR_G))
#define TR_BURNED 6                 /* level 6 = burned (retired until reset) */

static uint8_t  tr_lvl[TR_NG];      /* 1..5 active, 6 = burned */
static uint32_t tr_due[TR_NG];      /* scheduler tick at which the glyph is next due */
static uint32_t tr_tick;            /* global answered-trial counter (drives scheduling) */
static int      tr_target;          /* index into TR_G, -1 = none left (all burned) */
static int      tr_correct, tr_total, tr_streak, tr_best;
static int      tr_mode;            /* 0 = Drill (quiz), 1 = Train (record my strokes) */
static int      tr_train_idx;       /* Train mode walks a..z in order */

/* review interval (in scheduler ticks) once a CORRECT answer promotes a glyph to
 * level L: higher level -> longer interval -> resurfaces less often. */
static const uint32_t TR_INTV[TR_BURNED+1] = { 0, 0, 3, 6, 10, 16, 0 };

#define TR_SAVE  "/sdcard/graf_train.dat"
#define TR_MAGIC 0x47543032u        /* 'GT02' (supersedes the old 26-byte box file) */
static void tr_reset_mem(void){
    for(int i=0;i<TR_NG;i++){ tr_lvl[i]=1; tr_due[i]=0; }
    tr_tick=0;
}
static void tr_load(void){
    tr_reset_mem();
    FILE *f = fopen(TR_SAVE, "rb"); if(!f) return;
    uint32_t magic=0, tick=0;
    if(fread(&magic,4,1,f)==1 && magic==TR_MAGIC &&
       fread(&tick,4,1,f)==1 &&
       fread(tr_lvl,1,TR_NG,f)==(size_t)TR_NG &&
       fread(tr_due,4,TR_NG,f)==(size_t)TR_NG){
        tr_tick = tick;
        for(int i=0;i<TR_NG;i++){ if(tr_lvl[i]<1) tr_lvl[i]=1; if(tr_lvl[i]>TR_BURNED) tr_lvl[i]=TR_BURNED; }
    } else tr_reset_mem();          /* old/short/foreign file: start fresh */
    fclose(f);
}
static void tr_save(void){
    FILE *f=fopen(TR_SAVE,"wb"); if(!f) return;
    uint32_t magic=TR_MAGIC;
    fwrite(&magic,4,1,f); fwrite(&tr_tick,4,1,f);
    fwrite(tr_lvl,1,TR_NG,f); fwrite(tr_due,4,TR_NG,f);
    fclose(f);
}

/* deterministic pick: the non-burned glyph with the smallest due tick (ties resolve
 * to the lowest index). Returns -1 when every glyph is burned. */
static int tr_pick(void){
    int best=-1; uint32_t bestdue=0;
    for(int i=0;i<TR_NG;i++){
        if(tr_lvl[i]>=TR_BURNED) continue;
        if(best<0 || tr_due[i]<bestdue){ best=i; bestdue=tr_due[i]; }
    }
    return best;
}

static void tr_plot(int x, int y){
    i1_obj_px(tr_guide, x, y, 1);
}
static void tr_line(int x0,int y0,int x1,int y1){    /* Bresenham, 2px weight */
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
    for(;;){
        tr_plot(x0,y0); tr_plot(x0+1,y0); tr_plot(x0,y0+1);
        if(x0==x1&&y0==y1) break;
        int e2=2*err;
        if(e2>=dy){ err+=dy; x0+=sx; }
        if(e2<=dx){ err+=dx; y0+=sy; }
    }
}
static void tr_draw_guide_body(int gi){
    i1_obj_clear(tr_guide);
    if(gi<0) return;
    int np=0; const float *p = graffiti_glyph_template(TR_G[gi], &np);
    if(!p || np<1){                               /* no drawn stroke (e.g. '.'): a dot */
        int cx=TR_GW/2, cy=TR_GH/2;
        for(int a=-2;a<=2;a++) for(int b=-2;b<=2;b++) if(a*a+b*b<=4) tr_plot(cx+a,cy+b);
        return;
    }
    const int pad=14, span=TR_GW-2*pad;
    #define GX(i) (pad + (int)(p[2*(i)]  /10.0f*span))
    #define GY(i) (pad + (int)(p[2*(i)+1]/10.0f*span))
    for(int i=0;i<np-1;i++) tr_line(GX(i),GY(i),GX(i+1),GY(i+1));
    int sx=GX(0), sy=GY(0);                       /* filled start dot: shows direction */
    for(int a=-2;a<=2;a++) for(int b=-2;b<=2;b++) if(a*a+b*b<=4) tr_plot(sx+a,sy+b);
    #undef GX
    #undef GY
}
/* one invalidate for the whole guide, however the body returned */
static void tr_draw_guide(int gi){
    if(!tr_guide) return;
    tr_draw_guide_body(gi);
    lv_obj_invalidate(tr_guide);
}

/* how the current target is entered, so the prompt can nudge the user to the right
 * pad / punctuation shift (letters need no hint). */
static const char *tr_hint(char c){
    if(c>='0' && c<='9')       return "  (123 pad)";
    if(c=='.')                 return "  (tap twice)";
    if(!(c>='a' && c<='z'))    return "  (tap, then draw)";   /* punctuation shift */
    return "";
}

static void tr_render(void){
    tr_draw_guide(tr_target);
    if(tr_mode){                                  /* Train (record my strokes), letters only */
        if(tr_prompt)   lv_label_set_text_fmt(tr_prompt, "Trace:  %c", TR_G[tr_target]);
        if(tr_mode_lbl) lv_label_set_text(tr_mode_lbl, "Drill");
        if(tr_score)    lv_label_set_text_fmt(tr_score, "recorded %d/26 letters", graffiti_user_count());
        return;
    }
    if(tr_mode_lbl) lv_label_set_text(tr_mode_lbl, "Train");
    if(tr_target < 0){                            /* every glyph mastered/burned */
        if(tr_prompt) lv_label_set_text(tr_prompt, "All burned!");
        if(tr_score)  lv_label_set_text(tr_score, "every glyph mastered -- reset to replay");
        return;
    }
    char c = TR_G[tr_target];
    if(tr_prompt) lv_label_set_text_fmt(tr_prompt, "Write:  %c   Lv %d/5%s", c, tr_lvl[tr_target], tr_hint(c));
    if(tr_score){
        int burned=0; for(int i=0;i<TR_NG;i++) if(tr_lvl[i]>=TR_BURNED) burned++;
        lv_label_set_text_fmt(tr_score, "%d/%d  streak %d (best %d)  %d/%d burned",
                              tr_correct, tr_total, tr_streak, tr_best, burned, TR_NG);
    }
}

/* Drill mode: score the recognized glyph (quality %% from the $1 distance) and
 * apply the deterministic SRS -- correct promotes + reschedules later (or burns
 * past level 5), wrong demotes + reschedules immediately for a retry. */
static void trainer_input(char c){
    if(tr_target < 0) return;                                 /* nothing left to drill */
    if(c==' '||c=='\b'||c=='\n'||c==GRAF_SHIFT||c==GRAF_PUNCT) return;  /* gestures, not glyphs */
    char want = TR_G[tr_target];
    int pct = (int)(100.0f * (1.0f - graffiti_last_distance()/32.0f));
    if(pct<0) pct=0;
    if(pct>100) pct=100;
    tr_total++;
    if(c == want){
        tr_correct++; tr_streak++; if(tr_streak>tr_best) tr_best=tr_streak;
        int nl = tr_lvl[tr_target] + 1;
        if(nl >= TR_BURNED){
            tr_lvl[tr_target] = TR_BURNED;                    /* mastered -> retire it */
            if(tr_feedback) lv_label_set_text_fmt(tr_feedback, "Mastered '%c' -- burned!", want);
        } else {
            tr_lvl[tr_target] = (uint8_t)nl;
            tr_due[tr_target] = tr_tick + TR_INTV[nl];        /* longer interval */
            if(tr_feedback) lv_label_set_text_fmt(tr_feedback, "Nice!  %d%%  (Lv %d)", pct, nl);
        }
        tr_tick++; tr_save();
        tr_target = tr_pick();                                /* advance deterministically */
    } else {
        tr_streak = 0;
        int nl = tr_lvl[tr_target] - 1; if(nl<1) nl=1;
        tr_lvl[tr_target] = (uint8_t)nl;
        tr_due[tr_target] = tr_tick;                          /* soonest -> comes back more often */
        if(tr_feedback) lv_label_set_text_fmt(tr_feedback, "read '%c' (%d%%) - try again", c, pct);
        tr_tick++; tr_save();                                 /* keep target: immediate retry */
    }
    tr_render();
}

/* Train mode: capture the raw stroke as this letter's per-device template (runs on
 * pen-up, before recognition, so the buffer is intact), persist it, and walk a..z.
 * Returns 1 = consumed (no normal recognition/typing). */
static int trainer_capture(void){
    if(graffiti_capture_user((char)('a'+tr_target))){
        graffiti_user_save(TR_USER);
        if(tr_feedback) lv_label_set_text_fmt(tr_feedback, "saved your '%c'", 'a'+tr_target);
        tr_train_idx = (tr_train_idx + 1) % 26;   /* next letter to record */
        tr_target = tr_train_idx;
    } else if(tr_feedback) {
        lv_label_set_text(tr_feedback, "draw the whole letter");
    }
    tr_render();
    return 1;
}

static void tr_mode_toggle(lv_event_t *e){
    (void)e;
    tr_mode = !tr_mode;
    if(tr_mode){
        tr_train_idx = 0; tr_target = 0;
        graf_char_hook = NULL; graf_capture_hook = trainer_capture;
        if(tr_feedback) lv_label_set_text(tr_feedback, "trace each letter to teach it");
    } else {
        tr_target = tr_pick();
        graf_capture_hook = NULL; graf_char_hook = trainer_input;
        if(tr_feedback) lv_label_set_text(tr_feedback, "draw it in the strip below");
    }
    tr_render();
}

static void graffiti_to_kana_cb(lv_event_t *e){ (void)e; show_kana(); }

static void show_trainer(void){
    kill_kb();
    cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Graffiti");
    update_cat_trigger();
    tr_load();
    tr_correct=tr_total=tr_streak=tr_best=0;
    tr_mode=0; tr_train_idx=0;
    tr_target=tr_pick();

    tr_prompt = lv_label_create(content);
    lv_obj_set_style_text_font(tr_prompt, &lv_font_palm_bold, 0);
    lv_obj_align(tr_prompt, LV_ALIGN_TOP_LEFT, 6, 8);

    /* mode toggle: Drill (quiz) <-> Train (record my own strokes) */
    lv_obj_t *mb = lv_button_create(content);
    lv_obj_set_size(mb, 56, 26);
    lv_obj_align(mb, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_radius(mb, 0, 0);
    tr_mode_lbl = lv_label_create(mb);
    lv_obj_center(tr_mode_lbl);
    lv_obj_add_event_cb(mb, tr_mode_toggle, LV_EVENT_CLICKED, NULL);

    /* Kana lives here (handwriting sibling of the Latin drill): a compact "あ" button
     * to its left switches into the kana trainer. */
    lv_obj_t *kb = lv_button_create(content);
    lv_obj_set_size(kb, 30, 26);
    lv_obj_align(kb, LV_ALIGN_TOP_RIGHT, -64, 2);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_add_event_cb(kb, graffiti_to_kana_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kbl = lv_label_create(kb);
    lv_obj_set_style_text_font(kbl, &lv_font_kana, 0);
    lv_label_set_text(kbl, "\xe3\x81\x82");   /* U+3042 HIRAGANA A */
    lv_obj_center(kbl);

    tr_guide = lv_canvas_create(content);
    lv_canvas_set_buffer(tr_guide, tr_guide_buf, TR_GW, TR_GH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(tr_guide, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(tr_guide, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(tr_guide, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_border_width(tr_guide, 1, 0);
    lv_obj_set_style_border_color(tr_guide, COL_LINE, 0);

    tr_feedback = lv_label_create(content);
    lv_obj_set_style_text_font(tr_feedback, &lv_font_palm, 0);
    lv_obj_align(tr_feedback, LV_ALIGN_TOP_MID, 0, 32 + TR_GH + 6);
    lv_label_set_text(tr_feedback, "draw it in the strip below");

    tr_score = lv_label_create(content);
    lv_obj_set_style_text_font(tr_score, &lv_font_palm, 0);
    lv_obj_align(tr_score, LV_ALIGN_BOTTOM_MID, 0, -2);

    tr_render();
    graf_char_hook = trainer_input;    /* AFTER kill_kb cleared it: route strokes here */
    g_trainer_open = 1;                /* enables Menu > Reset progress */
}

/* wipe all trainer progress (Menu > Reset progress). */
static void tr_reset_progress(void){
    tr_reset_mem();
    tr_save();
}

/* ===================== Kana Trainer (roadmap #3, Tiers 1-2) ==================
 * A learn-the-syllabary app with TWO challenges per kana, each on the same
 * deterministic SRS (level 1..5, burn past 5, smallest-due pick):
 *
 *   SOUND (Tier 1): shows a kana (hiragana, then katakana); you answer its
 *     Hepburn romaji by drawing Latin letters in the Graffiti strip -- reusing
 *     the letter recognizer with ZERO changes. The answer is NOTED on a kana's
 *     first sight and on every miss.
 *   WRITE (Tier 2): shows the kana's stroke model with NUMBERED strokes; you
 *     redraw it stroke by stroke in the strip. Official stroke ORDER is enforced
 *     -- each stroke is matched (kana_write.c's separate $1) against the ONE
 *     expected next stroke, so a wrong shape/direction/order is rejected. This
 *     decomposes multi-stroke recognition into N single-stroke checks and never
 *     touches the Latin recognizer. Stroke data: KanjiVG (CC BY-SA).
 *
 * A top-right button toggles Sound <-> Write; each keeps its own SRS state.
 * Progress persists to SD. Pool-safe: labels + one I1 canvas for the model. */
#define KA_MAX    96              /* >= KANA_N (92); compile-time array bound */
#define KA_BURNED 6               /* level 6 = burned (retired until reset) */
#define KA_SAVE   "/sdcard/kana_train.dat"
#define KA_MAGIC  0x4B543032u     /* 'KT02' (adds the write-challenge state) */
#define KW_THRESH 26.0f           /* per-stroke accept distance (tune on-device) */
#define KW_GW 104
#define KW_GH 104
static uint8_t  ka_lvl[KA_MAX];   /* SOUND: 1..5 active, 6 = burned */
static uint32_t ka_due[KA_MAX];   /* SOUND: scheduler tick when next due */
static uint8_t  ka_intro[KA_MAX]; /* SOUND: 1 = answered correctly at least once */
static uint8_t  kw_lvl[KA_MAX];   /* WRITE: level */
static uint32_t kw_due[KA_MAX];   /* WRITE: due tick */
static uint32_t ka_tick;          /* global answered-trial counter (drives scheduling) */
static int      ka_wmode;         /* 0 = Sound challenge, 1 = Write challenge */
static int      ka_target;        /* index into KANA (current mode), -1 = none left */
static int      ka_correct, ka_total, ka_streak, ka_best;   /* per-session stats */
static int      ka_reveal;        /* SOUND: show the romaji note for the current target */
static char     ka_buf[8];        /* SOUND: romaji drawn so far */
static int      ka_len;
static int      kw_cur;           /* WRITE: index of the next stroke to draw (also how
                                   * many are "locked in" and shown solid so far) */
static const uint32_t KA_INTV[KA_BURNED+1] = { 0, 0, 3, 6, 10, 16, 0 };
static lv_obj_t *ka_kana, *ka_prompt, *ka_answer, *ka_typed, *ka_feedback, *ka_score;
static lv_obj_t *ka_strokes_lbl, *ka_model, *ka_modelbl;
static uint8_t   ka_model_buf[LV_CANVAS_BUF_SIZE(KW_GW, KW_GH, 1, 1) + 16];
static void kana_build(int mode);

static int ka_count(void){ int n = KANA_N; return n > KA_MAX ? KA_MAX : n; }

static void ka_reset_mem(void){
    for(int i=0;i<KA_MAX;i++){ ka_lvl[i]=1; ka_due[i]=0; ka_intro[i]=0; kw_lvl[i]=1; kw_due[i]=0; }
    ka_tick=0;
}
static void ka_load(void){
    ka_reset_mem();
    FILE *f=fopen(KA_SAVE,"rb"); if(!f) return;
    uint32_t magic=0, tick=0; int n=ka_count();
    if(fread(&magic,4,1,f)==1 && magic==KA_MAGIC &&
       fread(&tick,4,1,f)==1 &&
       fread(ka_lvl,1,n,f)==(size_t)n &&
       fread(ka_due,4,n,f)==(size_t)n &&
       fread(ka_intro,1,n,f)==(size_t)n &&
       fread(kw_lvl,1,n,f)==(size_t)n &&
       fread(kw_due,4,n,f)==(size_t)n){
        ka_tick=tick;
        for(int i=0;i<n;i++){
            if(ka_lvl[i]<1) ka_lvl[i]=1;
            if(ka_lvl[i]>KA_BURNED) ka_lvl[i]=KA_BURNED;
            if(kw_lvl[i]<1) kw_lvl[i]=1;
            if(kw_lvl[i]>KA_BURNED) kw_lvl[i]=KA_BURNED;
        }
    } else ka_reset_mem();
    fclose(f);
}
static void ka_save(void){
    FILE *f=fopen(KA_SAVE,"wb"); if(!f) return;
    uint32_t magic=KA_MAGIC; int n=ka_count();
    fwrite(&magic,4,1,f); fwrite(&ka_tick,4,1,f);
    fwrite(ka_lvl,1,n,f); fwrite(ka_due,4,n,f); fwrite(ka_intro,1,n,f);
    fwrite(kw_lvl,1,n,f); fwrite(kw_due,4,n,f);
    fclose(f);
}
/* deterministic pick within the ACTIVE mode: the non-burned kana with the
 * smallest due tick (ties by order). Returns -1 when every kana is burned. */
static int ka_pick(void){
    uint8_t *lvl = ka_wmode ? kw_lvl : ka_lvl;
    uint32_t *due = ka_wmode ? kw_due : ka_due;
    int best=-1; uint32_t bestdue=0; int n=ka_count();
    for(int i=0;i<n;i++){
        if(lvl[i]>=KA_BURNED) continue;
        if(best<0 || due[i]<bestdue){ best=i; bestdue=due[i]; }
    }
    return best;
}
static void ka_set_target(int t){
    ka_target=t; ka_len=0; ka_buf[0]=0; kw_cur=0;
    ka_reveal = (t>=0 && ka_wmode==0 && !ka_intro[t]) ? 1 : 0;   /* SOUND: note on first sight */
}

/* ---- WRITE mode: draw the numbered stroke model on the I1 canvas ---- */
static void ka_mplot(int x,int y){
    i1_obj_px(ka_model, x, y, 1);
}
/* style: 0 = dotted guide (a stroke not yet drawn), 1 = solid "locked in" (a stroke
 * the user has correctly drawn -- stays visible until the kana is finished or a
 * wrong stroke restarts it). */
static void ka_mline(int x0,int y0,int x1,int y1,int style){
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy, k=0;
    for(;;){
        if(style || (k++ & 1)==0){          /* solid: every pixel; dotted: every other */
            ka_mplot(x0,y0);
            if(style){ ka_mplot(x0+1,y0); ka_mplot(x0,y0+1); }   /* thicken the locked-in stroke */
        }
        if(x0==x1&&y0==y1) break;
        int e2=2*err;
        if(e2>=dy){ err+=dy; x0+=sx; }
        if(e2<=dx){ err+=dx; y0+=sy; }
    }
}
/* tiny 3x5 numerals 0-9 for stroke labels (low 3 bits per row, top->bottom). */
static const uint8_t KW_DIG[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7},
};
static void ka_mdigit(int cx,int cy,int d){
    if(d<0||d>9) return;
    for(int r=0;r<5;r++) for(int c=0;c<3;c++)
        if(KW_DIG[d][r] & (4>>c)) ka_mplot(cx+c, cy+r);
}
static void ka_draw_model_body(void){
    i1_obj_clear(ka_model);
    if(ka_target<0) return;
    const KanaStrokes *ks = &KANA_STROKES[ka_target];
    const int pad=10, span=KW_GW-2*pad;
    #define MX(v) (pad + (int)((v)*span/108))
    for(int si=0; si<ks->n; si++){
        const KStroke *st = &ks->s[si];
        int solid = (si < kw_cur);                 /* already drawn correctly -> locked in */
        for(int j=0;j+1<st->npts;j++)
            ka_mline(MX(st->pts[2*j]),   MX(st->pts[2*j+1]),
                     MX(st->pts[2*(j+1)]),MX(st->pts[2*(j+1)+1]), solid);
        int sx=MX(st->pts[0]), sy=MX(st->pts[1]);
        /* number the stroke at its start (offset up-left, clamped) */
        int nx=sx-5, ny=sy-6; if(nx<0)nx=0; if(ny<0)ny=0; if(nx>KW_GW-3)nx=KW_GW-3; if(ny>KW_GH-5)ny=KW_GH-5;
        ka_mdigit(nx, ny, si+1);
        if(si==kw_cur)                             /* start dot marks the stroke to draw next */
            for(int a=-1;a<=1;a++) for(int b=-1;b<=1;b++) ka_mplot(sx+a, sy+b);
    }
    #undef MX
}
/* one invalidate for the whole model, however the body returned */
static void ka_draw_model(void){
    if(!ka_model) return;
    ka_draw_model_body();
    lv_obj_invalidate(ka_model);
}

static void ka_render(void){
    int n=ka_count();
    uint8_t *lvl = ka_wmode ? kw_lvl : ka_lvl;
    if(ka_target<0){
        if(ka_prompt)      lv_label_set_text(ka_prompt, "All burned!");
        if(ka_kana)        lv_label_set_text(ka_kana, "");
        if(ka_answer)      lv_label_set_text(ka_answer, "every kana mastered -- reset to replay");
        if(ka_typed)       lv_label_set_text(ka_typed, "");
        if(ka_strokes_lbl) lv_label_set_text(ka_strokes_lbl, "");
        if(ka_score)       lv_label_set_text(ka_score, "");
        if(ka_model)       ka_draw_model();
        return;
    }
    const KanaEntry *k=&KANA[ka_target];
    if(ka_prompt) lv_label_set_text_fmt(ka_prompt, "%s   Lv %d/5",
                        k->script ? "Katakana" : "Hiragana", lvl[ka_target]);
    if(ka_wmode==0){                                   /* SOUND */
        if(ka_kana) lv_label_set_text(ka_kana, k->kana);
        if(ka_answer){
            if(ka_reveal) lv_label_set_text_fmt(ka_answer, "sound:  %s", k->romaji);
            else          lv_label_set_text(ka_answer, "draw the sound");
        }
        if(ka_typed) lv_label_set_text(ka_typed, ka_len ? ka_buf : "_");
    } else {                                           /* WRITE */
        int ns = KANA_STROKES[ka_target].n;
        if(ka_strokes_lbl) lv_label_set_text_fmt(ka_strokes_lbl, "stroke %d of %d",
                                (kw_cur<ns?kw_cur+1:ns), ns);
        if(ka_model) ka_draw_model();
    }
    if(ka_score){
        int burned=0; for(int i=0;i<n;i++) if(lvl[i]>=KA_BURNED) burned++;
        lv_label_set_text_fmt(ka_score, "%d/%d  streak %d (best %d)  %d/%d burned",
                              ka_correct, ka_total, ka_streak, ka_best, burned, n);
    }
}

/* SOUND: score the drawn romaji letter by letter against the Hepburn reading.
 * A correct prefix advances the echo; a full match promotes/burns; a divergence
 * demotes, re-reveals, and keeps the target for an immediate retry. */
static void ka_input(char c){
    if(ka_target<0) return;
    if(c==' '||c=='\n'||c==GRAF_SHIFT||c==GRAF_PUNCT) return;   /* gestures, not letters */
    if(c=='\b'){ if(ka_len>0){ ka_len--; ka_buf[ka_len]=0; } ka_render(); return; }
    if(c<'a'||c>'z') return;                                    /* romaji is a-z only */
    if(ka_len < (int)sizeof ka_buf - 1){ ka_buf[ka_len++]=c; ka_buf[ka_len]=0; }
    const char *want = KANA[ka_target].romaji;
    int wl = (int)strlen(want);
    if(strncmp(ka_buf, want, ka_len)==0){          /* still on track */
        if(ka_len==wl){                            /* full romaji drawn -> correct */
            ka_total++; ka_correct++; ka_streak++; if(ka_streak>ka_best) ka_best=ka_streak;
            ka_intro[ka_target]=1;
            int nl = ka_lvl[ka_target] + 1;
            if(nl>=KA_BURNED){
                ka_lvl[ka_target]=KA_BURNED;
                if(ka_feedback) lv_label_set_text_fmt(ka_feedback, "Mastered %s -- burned!", want);
            } else {
                ka_lvl[ka_target]=(uint8_t)nl; ka_due[ka_target]=ka_tick+KA_INTV[nl];
                if(ka_feedback) lv_label_set_text_fmt(ka_feedback, "Correct!  %s  (Lv %d)", want, nl);
            }
            ka_tick++; ka_save();
            ka_set_target(ka_pick());
        } else if(ka_feedback){
            lv_label_set_text(ka_feedback, "keep going...");
        }
    } else {                                       /* wrong letter -> miss */
        ka_total++; ka_streak=0;
        int nl = ka_lvl[ka_target] - 1; if(nl<1) nl=1;
        ka_lvl[ka_target]=(uint8_t)nl; ka_due[ka_target]=ka_tick;
        if(ka_feedback) lv_label_set_text_fmt(ka_feedback, "was:  %s  - try again", want);
        ka_tick++; ka_save();
        ka_len=0; ka_buf[0]=0; ka_reveal=1;        /* re-show the answer, keep target */
    }
    ka_render();
}

/* WRITE: on each pen-up, match the raw stroke against the expected NEXT stroke.
 * Runs as graf_capture_hook (before recognition, buffer intact) and consumes the
 * stroke. A correct stroke locks in (stays solid on the model) and advances; the
 * last stroke completing promotes/burns the kana. A WRONG stroke is a miss:
 * demote, reschedule soon, and START OVER from stroke 1 (the locked-in strokes
 * clear) -- enforcing the whole character in correct shape, direction and order. */
static int kw_capture(void){
    if(ka_target<0) return 1;
    static int16_t raw[512];
    int un = graffiti_raw_stroke(raw, 256);
    if(un < 2){ if(ka_feedback) lv_label_set_text(ka_feedback, "draw the stroke"); return 1; }
    const KanaStrokes *ks = &KANA_STROKES[ka_target];
    if(kw_cur >= ks->n) kw_cur = 0;
    float d = kana_stroke_dist(raw, un, &ks->s[kw_cur]);
    if(d < KW_THRESH){                             /* correct stroke -> lock it in */
        kw_cur++;
        if(kw_cur >= ks->n){                       /* whole kana drawn cleanly */
            ka_total++; ka_correct++; ka_streak++; if(ka_streak>ka_best) ka_best=ka_streak;
            int nl = kw_lvl[ka_target] + 1;
            if(nl>=KA_BURNED){ kw_lvl[ka_target]=KA_BURNED;
                if(ka_feedback) lv_label_set_text(ka_feedback, "Mastered -- burned!"); }
            else { kw_lvl[ka_target]=(uint8_t)nl; kw_due[ka_target]=ka_tick+KA_INTV[nl];
                if(ka_feedback) lv_label_set_text_fmt(ka_feedback, "Correct!  (Lv %d)", nl); }
            ka_tick++; ka_save();
            ka_set_target(ka_pick());
        } else if(ka_feedback){
            lv_label_set_text_fmt(ka_feedback, "stroke %d ok", kw_cur);   /* kw_cur = next now */
        }
    } else {                                       /* wrong stroke -> miss + start over */
        ka_total++; ka_streak=0;
        int nl = kw_lvl[ka_target] - 1; if(nl<1) nl=1;
        kw_lvl[ka_target]=(uint8_t)nl; kw_due[ka_target]=ka_tick;
        ka_tick++; ka_save();
        kw_cur = 0;                                /* clear progress: restart from stroke 1 */
        if(ka_feedback) lv_label_set_text(ka_feedback, "wrong stroke -- start from 1");
    }
    ka_render();
    return 1;                                      /* consume: no letter recognition */
}

static void kana_toggle_mode(lv_event_t *e){ (void)e;
    ka_wmode = !ka_wmode;
    ka_correct=ka_total=ka_streak=ka_best=0;
    ka_set_target(ka_pick());
    kana_build(ka_wmode);
}

static void kana_to_graffiti_cb(lv_event_t *e){ (void)e; show_trainer(); }

/* build the Kana screen for `mode` (0 Sound / 1 Write): clears content, lays out
 * the widgets that mode needs, wires the right Graffiti hook, and renders. */
static void kana_build(int mode){
    content_clear();
    ka_wmode = mode;

    /* mode toggle (top-right): label = the mode you'll switch TO */
    lv_obj_t *mb = lv_button_create(content);
    lv_obj_set_size(mb, 56, 26);
    lv_obj_align(mb, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_radius(mb, 0, 0);
    ka_modelbl = lv_label_create(mb);
    lv_label_set_text(ka_modelbl, mode==0 ? "Write" : "Sound");
    lv_obj_center(ka_modelbl);
    lv_obj_add_event_cb(mb, kana_toggle_mode, LV_EVENT_CLICKED, NULL);

    /* back to the Latin Graffiti drill (Kana is folded in under Graffiti) */
    lv_obj_t *bb = lv_button_create(content);
    lv_obj_set_size(bb, 34, 26);
    lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, -64, 2);
    lv_obj_set_style_radius(bb, 0, 0);
    lv_obj_add_event_cb(bb, kana_to_graffiti_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb); lv_label_set_text(bbl, "ABC"); lv_obj_center(bbl);

    ka_prompt = lv_label_create(content);
    lv_obj_set_style_text_font(ka_prompt, &lv_font_palm_bold, 0);
    lv_obj_align(ka_prompt, LV_ALIGN_TOP_LEFT, 6, 8);

    ka_feedback = lv_label_create(content);
    lv_obj_set_style_text_font(ka_feedback, &lv_font_palm, 0);

    ka_score = lv_label_create(content);
    lv_obj_set_style_text_font(ka_score, &lv_font_palm, 0);
    lv_obj_align(ka_score, LV_ALIGN_BOTTOM_MID, 0, -2);

    if(mode==0){                                   /* SOUND layout */
        ka_kana = lv_label_create(content);
        lv_obj_set_style_text_font(ka_kana, &lv_font_kana, 0);
        lv_obj_align(ka_kana, LV_ALIGN_TOP_MID, 0, 30);

        ka_answer = lv_label_create(content);
        lv_obj_set_style_text_font(ka_answer, &lv_font_palm, 0);
        lv_obj_align(ka_answer, LV_ALIGN_TOP_MID, 0, 86);

        ka_typed = lv_label_create(content);
        lv_obj_set_style_text_font(ka_typed, &lv_font_palm_bold, 0);
        lv_obj_align(ka_typed, LV_ALIGN_TOP_MID, 0, 106);

        lv_obj_align(ka_feedback, LV_ALIGN_TOP_MID, 0, 128);
        lv_label_set_text(ka_feedback, "draw the romaji in the strip below");

        graf_capture_hook = NULL; graf_char_hook = ka_input;
    } else {                                       /* WRITE layout */
        ka_strokes_lbl = lv_label_create(content);
        lv_obj_set_style_text_font(ka_strokes_lbl, &lv_font_palm, 0);
        lv_obj_align(ka_strokes_lbl, LV_ALIGN_TOP_RIGHT, -66, 10);

        ka_model = lv_canvas_create(content);
        lv_canvas_set_buffer(ka_model, ka_model_buf, KW_GW, KW_GH, LV_COLOR_FORMAT_I1);
        lv_canvas_set_palette(ka_model, 0, lv_color_to_32(COL_BODY, 0xFF));
        lv_canvas_set_palette(ka_model, 1, lv_color_to_32(COL_LINE, 0xFF));
        lv_obj_align(ka_model, LV_ALIGN_TOP_MID, 0, 32);
        lv_obj_set_style_border_width(ka_model, 1, 0);
        lv_obj_set_style_border_color(ka_model, COL_LINE, 0);

        lv_obj_align(ka_feedback, LV_ALIGN_TOP_MID, 0, 32 + KW_GH + 6);
        lv_label_set_text(ka_feedback, "trace each numbered stroke below");

        graf_char_hook = NULL; graf_capture_hook = kw_capture;
    }
    ka_render();
    g_kana_open = 1;                               /* enables Menu > Reset progress */
}

static void show_kana(void){
    kill_kb();
    cur_app=NULL; cur_uid=0;
    lv_label_set_text(title_lbl, "Kana");
    update_cat_trigger();
    ka_load();
    ka_wmode=0;
    ka_correct=ka_total=ka_streak=ka_best=0;
    ka_set_target(ka_pick());
    kana_build(0);
}

/* wipe all Kana trainer progress -- both challenges (Menu > Reset progress). */
static void ka_reset_progress(void){
    ka_reset_mem();
    ka_save();
}

/* ===================== News (RSS reader, roadmap #4) =========================
 * A one-item-per-view, vertical-swipe feed reader (headline + text, no images).
 * Articles are fetched during HotSync and stored on SD (bridge/news.c); the reader
 * holds only the current article in RAM (read from SD on each swipe), so it scales
 * to any store size. Pool-safe: labels only, content swapped in place on swipe --
 * no layer-compositing widget. Until a real fetch runs (or in the sim, which has
 * no network) the store is seeded with a few sample articles so it's browseable. */
static int      g_news_i;                       /* current article index */
/* Unread count, kept in step rather than recounted. Re-deriving it per render
 * meant re-reading the whole index on every swipe -- see flags_scan in news.c. */
static int      g_news_unread;
static lv_obj_t *g_news_hdr, *g_news_feed, *g_news_title, *g_news_body, *g_news_hint;
static char     g_news_buf[2048];

/* sample feed shown until HotSync fills the store (device) or always (sim). */
static void news_seed_if_empty(void){
    if(news_count() > 0) return;
    if(!news_begin()) return;
    struct { const char *feed,*title,*body; } S[] = {
      {"CYD News","Palm PDA lives again","A base ESP32 CYD now runs a PalmOS-style "
        "PDA that two-way syncs to iCloud -- and swipes through the news like this."},
      {"Tech","No PSRAM, no problem","The whole UI fits beside Wi-Fi and TLS in ~80 KB "
        "of heap by time-multiplexing: rich UI offline, a status line during HotSync."},
      {"Tech","Graffiti, recognised","A $1 unistroke recognizer turns strokes into "
        "letters; a built-in trainer even learns your own hand over time."},
      {"World","Offline-first, on purpose","Like the original Palm: use it offline, "
        "HotSync periodically. Your data lives on the SD card, always readable."},
      {"World","Swipe to continue","This reader is one article per screen. Swipe up "
        "for the next story, down for the previous one -- no thumbs required."},
      {"Fun","The charm of constraints","240x320, a resistive touch panel, and a "
        "24 KB object pool. Working within limits is half the fun."},
    };
    for(unsigned i=0;i<sizeof S/sizeof S[0];i++) news_add(S[i].feed,S[i].title,S[i].body,0);
    news_commit();
}

static void news_layout(void);
static void news_render(void){
    int n = news_count();
    if(g_news_i < 0) g_news_i = 0;
    if(g_news_i >= n) g_news_i = n>0 ? n-1 : 0;
    if(n <= 0){
        if(g_news_hdr)   lv_label_set_text(g_news_hdr, "");
        if(g_news_feed)  lv_label_set_text(g_news_feed, "");
        if(g_news_title) lv_label_set_text(g_news_title, "No news yet");
        if(g_news_body)  lv_label_set_text(g_news_body, "HotSync fetches your feeds.\n"
                                           "Add feed URLs in Preferences.");
        if(g_news_hint)  lv_label_set_text(g_news_hint, "");
        return;
    }
    NewsMeta m; news_meta(g_news_i, &m);
    news_read_text(g_news_i, g_news_buf, sizeof g_news_buf);
    /* Mark on display, not on leaving: the article is on screen and read, and a
     * four-byte write into the index is cheap enough to do per swipe. */
    if(!(m.flags & NEWS_F_READ)){
        news_mark_read(g_news_i);
        if(g_news_unread > 0) g_news_unread--;
    }
    if(g_news_unread > 0)
         lv_label_set_text_fmt(g_news_hdr, "%d/%d  %d new", g_news_i+1, n, g_news_unread);
    else lv_label_set_text_fmt(g_news_hdr, "%d/%d", g_news_i+1, n);
    lv_label_set_text(g_news_feed, m.feed);
    lv_label_set_text(g_news_title, m.title);
    lv_label_set_text(g_news_body, g_news_buf);
    lv_label_set_text(g_news_hint, g_news_i < n-1 ? "swipe up for next" :
                                   (n>1 ? "swipe down for previous" : ""));
    news_layout();
}

/* The headline WRAPS and the body did not know it: the title sat at y=16 and the
 * body was pinned at y=44, which is room for one line. A two-line headline --
 * most of them, at this width -- printed straight through the story text.
 * So place the body under whatever height the title actually took, and give it
 * the rest of the panel. lv_obj_update_layout() forces LVGL to compute that
 * height now rather than at the next refresh, which is what makes the number
 * usable in the same pass. */
#define NEWS_TITLE_Y   16
#define NEWS_GAP        6      /* headline to story */
#define NEWS_HINT_H    18      /* reserved for the swipe hint at the bottom */
static void news_layout(void){
    if(!g_news_title || !g_news_body) return;
    lv_obj_update_layout(g_news_title);
    int th = lv_obj_get_height(g_news_title);
    int y  = NEWS_TITLE_Y + th + NEWS_GAP;
    int avail = (PDA_H - TITLE_H) - y - NEWS_HINT_H - 12;   /* 12 = surf padding */
    if(avail < 24) avail = 24;                 /* a headline can never eat it all */
    lv_obj_align(g_news_body, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_height(g_news_body, avail);
}

/* Manual vertical-swipe detection (robust across the headless host and real
 * resistive touch, unlike LVGL's velocity-based gesture heuristic). The indev
 * point is already reset by the RELEASED event, so we track the last position
 * seen during PRESSING and compare it to the press Y. */
static int g_news_py, g_news_ly;
static void news_press_cb(lv_event_t *e){
    (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    g_news_py = g_news_ly = p.y;
}
static void news_pressing_cb(lv_event_t *e){
    (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    if(p.y > 0) g_news_ly = p.y;                                        /* last valid drag Y */
}
static void news_release_cb(lv_event_t *e){
    (void)e;
    int dy = g_news_ly - g_news_py, n = news_count();
    if(dy < -36 && g_news_i < n-1){ g_news_i++; news_render(); }        /* swipe up = next */
    else if(dy > 36 && g_news_i > 0){ g_news_i--; news_render(); }      /* swipe down = prev */
}

static void show_news(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0;
    news_seed_if_empty();
    /* Resume at the first story not yet read. Reopening used to snap back to
     * article 1, so every visit replayed what had already been flipped through.
     * Everything read -> stay at the end rather than restart from the top. */
    {
        int fu = news_first_unread();
        int n  = news_count();
        g_news_i = fu >= 0 ? fu : (n > 0 ? n-1 : 0);
        g_news_unread = news_unread();      /* once per open, not once per swipe */
    }
    content_clear();
    lv_label_set_text(title_lbl, "News");
    update_cat_trigger();

    /* a full-content gesture surface holds the labels; cleaned on navigation away
     * (so the gesture handler never accumulates on the persistent `content`). */
    lv_obj_t *surf = lv_obj_create(content);
    lv_obj_set_size(surf, LCD_W, PDA_H - TITLE_H);
    lv_obj_set_pos(surf, 0, 0);
    lv_obj_set_style_radius(surf, 0, 0);
    lv_obj_set_style_border_width(surf, 0, 0);
    lv_obj_set_style_bg_color(surf, COL_BODY, 0);
    lv_obj_set_style_pad_all(surf, 6, 0);
    lv_obj_clear_flag(surf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(surf, LV_OBJ_FLAG_CLICKABLE);         /* so it receives press/release */
    lv_obj_add_event_cb(surf, news_press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(surf, news_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(surf, news_release_cb,  LV_EVENT_RELEASED, NULL);

    g_news_feed = lv_label_create(surf);
    lv_obj_set_style_text_font(g_news_feed, &lv_font_palm, 0);
    lv_obj_align(g_news_feed, LV_ALIGN_TOP_LEFT, 0, 0);
    g_news_hdr = lv_label_create(surf);
    lv_obj_set_style_text_font(g_news_hdr, &lv_font_palm, 0);
    lv_obj_align(g_news_hdr, LV_ALIGN_TOP_RIGHT, 0, 0);

    g_news_title = lv_label_create(surf);
    lv_obj_set_style_text_font(g_news_title, &lv_font_palm_bold, 0);
    lv_label_set_long_mode(g_news_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_news_title, LCD_W - 12);
    lv_obj_align(g_news_title, LV_ALIGN_TOP_LEFT, 0, NEWS_TITLE_Y);

    g_news_body = lv_label_create(surf);
    lv_label_set_long_mode(g_news_body, LV_LABEL_LONG_DOT);   /* clip long bodies (feed card) */
    lv_obj_set_width(g_news_body, LCD_W - 12);
    /* height and y are set by news_layout(), from the headline's real height */

    g_news_hint = lv_label_create(surf);
    lv_obj_set_style_text_font(g_news_hint, &lv_font_palm, 0);
    lv_obj_set_style_text_color(g_news_hint, COL_GRAF, 0);
    lv_obj_align(g_news_hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    news_render();
}

static void show_launcher(void){
    kill_kb();
    cur_app = NULL;
    cur_uid = 0;
    content_clear();
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

    /* I1.1: onboarding hint. Until an iCloud account is configured, the records on
     * screen are demo data -- say so and point at setup. A full-width flex item at
     * the end of the grid (so it flows BELOW the icons instead of overlapping them
     * now that the app grid can be three rows); the grid scrolls to reveal it.
     * Disappears once dav_user (the Apple ID) is set. */
    if(appcfg()->dav_user[0] == '\0'){
        lv_obj_t *hint = lv_label_create(grid);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_pad_top(hint, 6, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hint, "Demo data shown. To sync your own:\n"
                                "edit config.ini on the card, or tap\n"
                                "Menu > Preferences.");
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

/* I4: a transient, auto-dismissing confirmation (Palm-style). Unlike alert_show
 * (a modal you tap to close), this is a non-clickable pill just above the Graffiti
 * strip that clears itself after ~900 ms -- for closing the loop on save/delete
 * without demanding a tap. A plain label with a solid fill allocates no draw
 * layer, so it's pool-safe (the lv_bar rule). */
static lv_obj_t  *g_toast;
static lv_timer_t *toast_timer;
static void toast_clear_cb(lv_timer_t *t){ (void)t;
    if(g_toast){ lv_obj_del(g_toast); g_toast=NULL; }
    toast_timer = NULL;
}
static void toast_show(const char *msg){
    if(g_toast){ lv_obj_del(g_toast); g_toast=NULL; }
    g_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(g_toast, COL_LINE, 0);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_toast, COL_TITLE_FG, 0);   /* white on black */
    lv_obj_set_style_pad_all(g_toast, 6, 0);
    lv_obj_set_style_radius(g_toast, 0, 0);
    lv_label_set_text(g_toast, msg);
    lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -(GRAFFITI_H + 8));
    if(toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(toast_clear_cb, 900, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);   /* one-shot: auto-deletes */
}

/* ---- Preferences: edit the config.ini fields on-device (Graffiti entry) ----
 * Rendered as an lv_list (the app-list pattern, which is rock-solid here) rather
 * than one big scrollable form: a form holding 10 textareas + buttons both
 * strained LVGL's 24 KB pool AND drove an infinite scroll-relayout loop (Task
 * WDT) once the content grew past the viewport. Each field is a list row
 * "Label: value"; tapping opens a single-field editor with ONE textarea (light,
 * and the proven-safe widget). Edits land in the in-memory config immediately;
 * the "Save to config.ini" row persists them (also picks up Discover's writes). */
/* PF_N bounds the Preferences LIST; the entries after it are editable fields
 * that live on other screens. Latitude/longitude are two of those: they belong
 * to the lock-screen dashboard, and three extra rows on the Preferences list
 * already once pushed it past LVGL's 24 KB pool (see show_prefs), so they are
 * reached from Lock Screen instead. Without them the only way to set a weather
 * location was to pull the SD card and edit config.ini on a computer. */
enum { PF_SSID, PF_WPASS, PF_USER, PF_PASS, PF_CALB, PF_CARDB,
       PF_CAL, PF_TODO, PF_CARD, PF_TZ, PF_N,
       PF_LAT = PF_N, PF_LON, PF_MAX };
static const char *PF_LABELS[PF_MAX] = {
    "Wi-Fi SSID", "Wi-Fi pass", "Apple ID", "App pass", "CalDAV host",
    "CardDAV host", "Calendar coll", "Reminders coll", "Address coll", "Time zone",
    "Latitude", "Longitude",
};
/* which screen an edit returns to */
static int pf_is_dash_field(int i){ return i==PF_LAT || i==PF_LON; }
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
        case PF_LAT:   *cap=sizeof c->latitude;      return c->latitude;
        case PF_LON:   *cap=sizeof c->longitude;     return c->longitude;
    }
    *cap=0; return NULL;
}

/* ---- single-field editor (one textarea at a time) ---- */
static int pf_edit_idx;
static void show_dash_settings(void);
static void pf_edit_back(void){
    if(pf_is_dash_field(pf_edit_idx)) show_dash_settings(); else show_prefs();
}
static void pf_edit_cancel_cb(lv_event_t *e){ (void)e; pf_edit_back(); }
static void pf_edit_save_cb(lv_event_t *e){ (void)e;
    int cap=0; char *dst = pf_buf(appcfg_mut(), pf_edit_idx, &cap);
    if(dst && cap) snprintf(dst, cap, "%s", lv_textarea_get_text(g_fields[0]));
    appcfg_save();            /* persist to SD now -> survives reboot */
    pf_edit_back();
    toast_show("Saved");      /* I4: same transient feedback as record save/delete */
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
    content_clear();
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

/* ---- News feeds manager (Preferences -> "News feeds...") --------------------
 * The RSS reader's sources: an lv_table (the pool-safe record-list widget) with a
 * checkbox column (tap col 0 = enable/disable, like To Do) and a name column (tap
 * = edit). "Add" opens the same editor for a new feed. The URL is typed on the tap
 * keyboard (the Preferences field pattern); the name is auto-derived from the host.
 * The list persists to feeds.txt on every change, and HotSync fetches the enabled
 * feeds. See bridge/feeds.c. */
static void feeds_back_cb(lv_event_t *e){ (void)e; show_prefs(); }
static void feeds_add_cb(lv_event_t *e){ (void)e; show_feed_edit(-1); }
static void feeds_tbl_click_cb(lv_event_t *e){
    lv_obj_t *t = lv_event_get_target(e);
    uint32_t r=LV_TABLE_CELL_NONE, c=LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &r, &c);
    if(r==LV_TABLE_CELL_NONE || (int)r >= feeds_count()) return;
    if(c==0){ feeds_toggle((int)r); feeds_save(FEEDS_PATH); show_feeds(); }   /* checkbox */
    else      show_feed_edit((int)r);                                        /* name -> edit */
}
static void show_feeds(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    content_clear();
    lv_label_set_text(title_lbl, "News Feeds");
    update_cat_trigger();

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, 58, 26); lv_obj_align(back, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *bl=lv_label_create(back); lv_label_set_text(bl,"Prefs"); lv_obj_center(bl);
    lv_obj_add_event_cb(back, feeds_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add = lv_button_create(content);
    lv_obj_set_size(add, 54, 26); lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *al=lv_label_create(add); lv_label_set_text(al,"Add"); lv_obj_center(al);
    lv_obj_add_event_cb(add, feeds_add_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_table_create(content);
    lv_obj_set_size(t, LCD_W, PDA_H - TITLE_H - 32);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_radius(t, 0, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 4, LV_PART_ITEMS);
    lv_table_set_column_width(t, 0, 34);
    lv_table_set_column_width(t, 1, LCD_W - 34 - 4);
    int n = feeds_count();
    if(n==0){
        lv_table_set_cell_value(t, 0, 1, "No feeds -- tap Add");
    } else {
        for(int i=0;i<n;i++){
            const Feed *f = feeds_get(i);
            lv_table_set_cell_value(t, i, 0, f->enabled ? "[x]" : "[ ]");
            char host[FEED_NAME_CAP]; feeds_host_label(f->url, host, sizeof host);
            lv_table_set_cell_value(t, i, 1, f->name[0] ? f->name : host);
        }
    }
    lv_obj_add_event_cb(t, feeds_tbl_click_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- add / edit one feed (URL on the tap keyboard; name auto-derived) ---- */
static int fe_edit_idx;                     /* -1 = adding a new feed */
static void fe_cancel_cb(lv_event_t *e){ (void)e; show_feeds(); }
static void fe_save_cb(lv_event_t *e){ (void)e;
    const char *url = lv_textarea_get_text(g_fields[0]);
    if(url && url[0]){
        if(fe_edit_idx < 0) feeds_add(url, "");                 /* name from host */
        else { const Feed *f = feeds_get(fe_edit_idx);
               char nm[FEED_NAME_CAP]; snprintf(nm, sizeof nm, "%s", f ? f->name : "");
               feeds_set(fe_edit_idx, url, nm); }               /* keep the existing name */
        feeds_save(FEEDS_PATH);
    }
    show_feeds();
    toast_show("Saved");
}
static void fe_delete_cb(lv_event_t *e){ (void)e;
    if(fe_edit_idx >= 0){ feeds_remove(fe_edit_idx); feeds_save(FEEDS_PATH); }
    show_feeds();
    toast_show("Removed");
}
static void show_feed_edit(int idx){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0; fe_edit_idx = idx;
    content_clear();
    lv_label_set_text(title_lbl, idx<0 ? "Add Feed" : "Edit Feed");
    update_cat_trigger();

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 58, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, fe_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = lv_button_create(content);
    lv_obj_set_size(save, 54, 28); lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *sl=lv_label_create(save); lv_label_set_text(sl,"Save"); lv_obj_center(sl);
    lv_obj_add_event_cb(save, fe_save_cb, LV_EVENT_CLICKED, NULL);
    if(idx >= 0){
        lv_obj_t *del = lv_button_create(content);
        lv_obj_set_size(del, 56, 28); lv_obj_align(del, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_t *dl=lv_label_create(del); lv_label_set_text(dl,"Delete"); lv_obj_center(dl);
        lv_obj_add_event_cb(del, fe_delete_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *lb = lv_label_create(content);
    lv_label_set_text(lb, "Feed URL (RSS/Atom)");
    lv_obj_set_pos(lb, 4, 36);
    const Feed *f = (idx>=0) ? feeds_get(idx) : NULL;
    lv_obj_t *ta = lv_textarea_create(content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, FEED_URL_CAP-1);
    lv_textarea_set_text(ta, f ? f->url : "https://");
    lv_obj_set_width(ta, LCD_W - 16);
    lv_obj_set_pos(ta, 4, 52);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    g_fields[0] = ta; g_nfields = 1; active_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

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
/* The same picker serves the system time zone AND the two lock-screen world
 * clocks -- g_zone_target selects which config field a tap writes. World targets
 * get a leading "(off)" row so a slot can be cleared; the system zone has none. */
enum { ZTGT_TZ = 0, ZTGT_W1 = 1, ZTGT_W2 = 2 };
static int g_zone_target;
static char *zone_target_buf(Config *c, int *cap){
    switch(g_zone_target){
        case ZTGT_W1: *cap=sizeof c->world1; return c->world1;
        case ZTGT_W2: *cap=sizeof c->world2; return c->world2;
        default:      *cap=sizeof c->timezone; return c->timezone;
    }
}
/* the picker returns to Preferences for the system zone, or to the Lock Screen
 * sub-screen for a world clock. */
static void zone_picker_return(void){
    if(g_zone_target==ZTGT_TZ) show_prefs(); else show_dash_settings();
}
static void tz_cancel_cb(lv_event_t *e){ (void)e; zone_picker_return(); }
static void tz_tbl_click_cb(lv_event_t *e){
    lv_obj_t *t = lv_event_get_target(e);
    uint32_t r=LV_TABLE_CELL_NONE, c=LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &r, &c);
    if(r==LV_TABLE_CELL_NONE) return;
    int off = (g_zone_target==ZTGT_TZ) ? 0 : 1;   /* world pickers have a "(off)" row 0 */
    Config *cfg = appcfg_mut();
    int cap=0; char *dst = zone_target_buf(cfg, &cap);
    if(off && (int)r == 0){ dst[0] = 0; }         /* "(off)" -> clear the slot */
    else {
        int zi = (int)r - off;
        if(zi < 0 || zi >= clock_zone_count()) return;
        const char *z = clock_zone_name(zi);
        snprintf(dst, cap, "%s", z);
        if(g_zone_target==ZTGT_TZ) clock_set_tz(z);   /* apply the system zone immediately */
    }
    appcfg_save();            /* persist to SD now -> survives reboot */
    zone_picker_return();
}
static void show_zone_picker(int target){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    g_zone_target = target;
    content_clear();
    lv_label_set_text(title_lbl, target==ZTGT_TZ ? "Time Zone" : "World Clock");
    update_cat_trigger();

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 60, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, tz_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* header: the field's current value (+ live offset/DST for the system zone) */
    int cap=0; const char *cur = zone_target_buf(appcfg_mut(), &cap);
    char hdr[128];
    if(target==ZTGT_TZ){
        char desc[40]; clock_now_desc(desc, sizeof desc);
        snprintf(hdr, sizeof hdr, "%s\n%s", cur[0]?cur:"(unset)", desc);
    } else {
        snprintf(hdr, sizeof hdr, "Clock %d\n%s", target, cur[0]?cur:"(off)");
    }
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
    int off = (target==ZTGT_TZ) ? 0 : 1, n = clock_zone_count();
    if(off) lv_table_set_cell_value(t, 0, 0, "(off)");
    for(int i=0;i<n;i++) lv_table_set_cell_value(t, i+off, 0, clock_zone_name(i));
    lv_obj_add_event_cb(t, tz_tbl_click_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- Lock Screen sub-screen: the dashboard's two world clocks + 12/24h format.
 * A light lv_list (3 rows + a Back button), so it adds nothing to the Preferences
 * list's pool footprint. World-clock rows open the shared zone picker; the format
 * row toggles in place. */
static void ds_back_cb(lv_event_t *e){ (void)e; show_prefs(); }
static void show_pref_edit(int i);
static void ds_lat_cb(lv_event_t *e){ (void)e; show_pref_edit(PF_LAT); }
static void ds_lon_cb(lv_event_t *e){ (void)e; show_pref_edit(PF_LON); }
static void ds_world1_cb(lv_event_t *e){ (void)e; show_zone_picker(ZTGT_W1); }
static void ds_world2_cb(lv_event_t *e){ (void)e; show_zone_picker(ZTGT_W2); }
static void ds_fmt_cb(lv_event_t *e){ (void)e;
    Config *c = appcfg_mut(); c->clock24 = !c->clock24; appcfg_save(); show_dash_settings();
}
static void show_dash_settings(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    content_clear();
    lv_label_set_text(title_lbl, "Lock Screen");
    update_cat_trigger();

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, 58, 26); lv_obj_align(back, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *bl=lv_label_create(back); lv_label_set_text(bl,"Prefs"); lv_obj_center(bl);
    lv_obj_add_event_cb(back, ds_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, LCD_W, PDA_H - TITLE_H - 32);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    const Config *c = appcfg();
    char row[80], tag[8];
    if(c->world1[0]){ world_tag(c->world1, tag, sizeof tag); snprintf(row, sizeof row, "World clock 1: %s (%s)", tag, c->world1); }
    else snprintf(row, sizeof row, "World clock 1: (off)");
    pf_add(list, row, ds_world1_cb, 0);
    if(c->world2[0]){ world_tag(c->world2, tag, sizeof tag); snprintf(row, sizeof row, "World clock 2: %s (%s)", tag, c->world2); }
    else snprintf(row, sizeof row, "World clock 2: (off)");
    pf_add(list, row, ds_world2_cb, 0);
    snprintf(row, sizeof row, "Clock format: %s", c->clock24 ? "24-hour" : "12-hour");
    pf_add(list, row, ds_fmt_cb, 0);
    /* Weather needs a location and had no way in but the SD card. Decimal
     * degrees, east/north positive -- the same thing config.ini takes. */
    snprintf(row, sizeof row, "Latitude: %s",  c->latitude[0]  ? c->latitude  : "(unset)");
    pf_add(list, row, ds_lat_cb, 0);
    snprintf(row, sizeof row, "Longitude: %s", c->longitude[0] ? c->longitude : "(unset)");
    pf_add(list, row, ds_lon_cb, 0);
}

/* ---- the Preferences list ---- */
static lv_obj_t *g_pf_bright_btn;   /* the "Brightness: NN%" row, refreshed on stepper close */
static void pf_row_open_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i == PF_TZ){ show_zone_picker(ZTGT_TZ); return; }   /* zone -> picker, not text entry */
    show_pref_edit(i);
}
static void pf_pol_row_cb(lv_event_t *e){ (void)e;
    Config *c = appcfg_mut(); c->policy = (c->policy + 1) % 3; appcfg_save(); show_prefs();
}
static void pf_dash_row_cb(lv_event_t *e){ (void)e; show_dash_settings(); }
static void pf_disc_row_cb(lv_event_t *e){ (void)e;
    if(hotsync_busy()){ alert_show("A sync is in progress; try again in a moment."); return; }
    show_discover();
}
static void pf_bright_row_cb(lv_event_t *e){ (void)e; br_open(); }
static void pf_feeds_row_cb(lv_event_t *e){ (void)e; show_feeds(); }
static void pf_saverow_cb(lv_event_t *e){ (void)e;
    int rc = appcfg_save();
    /* I4: success is a transient toast (like record save); a write FAILURE stays a
     * modal alert -- the user must notice the card didn't take their settings. */
    if(rc==0) toast_show("Saved to config.ini");
    else      alert_show("Could not write config.ini (SD card?)");
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
    content_clear();
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
    snprintf(row, sizeof row, "News feeds... (%d on)", feeds_enabled_count());
    pf_add(list, row, pf_feeds_row_cb, 0);
    /* the lock-screen dashboard settings live behind ONE sub-screen row (like News
     * feeds) rather than three inline rows -- three more lv_list buttons pushed the
     * Preferences list past the 24 KB object pool (the brightness popup then failed
     * to allocate its draw buffer and crashed on the device-sized 32-bit build). */
    pf_add(list, "Lock screen...", pf_dash_row_cb, 0);
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
    content_clear();
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
    content_clear();
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
static void menu_close(void){
    if(g_menu){ lv_obj_del(g_menu); g_menu=NULL; }
    g_co_len_lbl = g_co_goal_lbl = NULL;      /* they lived on the sheet */
}
static void menu_backdrop_cb(lv_event_t *e){ (void)e; menu_close(); }

static void act_new(lv_event_t *e){ (void)e; const AppDef *a=cur_app; menu_close(); if(a){ cur_app=a; show_edit(0); } }
static void act_delete(lv_event_t *e){ (void)e;
    uint32_t u=cur_uid; menu_close();
    if(u) ask_delete(u);   /* shared confirm dialog */
}
static void act_categories(lv_event_t *e){ (void)e; menu_close(); cat_trigger_cb(NULL); }
static void act_prefs(lv_event_t *e){ (void)e; menu_close(); show_prefs(); }
static void act_tr_reset(lv_event_t *e){ (void)e; menu_close(); tr_reset_progress(); show_trainer(); }
/* Coach: the session length cycles through the four lengths people actually use,
 * so setting it costs one tap and needs no picker screen.
 *
 * These two rows deliberately do NOT close the sheet. They are the only menu
 * entries that mutate a value rather than going somewhere, so dismissing on tap
 * hid the very thing the tap changed -- you had to reopen the menu to find out
 * what you had set. They restate themselves in place and repaint the home screen
 * underneath, and the user closes the sheet when they are done. */
static void co_menu_restate(void){
    if(g_co_len_lbl)  lv_label_set_text_fmt(g_co_len_lbl,  "Length: %d min",
                                            (int)g_co.pref_min);
    if(g_co_goal_lbl) lv_label_set_text_fmt(g_co_goal_lbl, "Day goal: %d",
                                            (int)g_co.day_goal);
    /* repaint what is behind the sheet, but only when it is the home screen --
     * the sheet lives on lv_layer_top(), so rebuilding `content` leaves it up. */
    if(g_co_view == CO_VIEW_HOME) show_coach();
}
static void act_co_len(lv_event_t *e){ (void)e;
    static const uint16_t LENS[] = { 15, 25, 45, 50 };
    int i = 0;
    for(int k = 0; k < 4; k++) if(g_co.pref_min == LENS[k]) i = k + 1;
    g_co.pref_min = LENS[i % 4];
    co_save();
    co_menu_restate();
    toast_show("Session length set");
    show_coach();
}
static void act_co_goal(lv_event_t *e){ (void)e;
    g_co.day_goal = (uint16_t)(g_co.day_goal >= 10 ? 2 : g_co.day_goal + 2);
    co_save();
    co_menu_restate();
}
static void act_co_marks(lv_event_t *e){ (void)e; menu_close(); show_coach_marks(); }
static void act_co_week(lv_event_t *e){ (void)e; menu_close(); show_coach_report(); }
static void act_co_reset(lv_event_t *e){ (void)e; menu_close();
    /* the counters go, the marks stay -- coach.log/coach.sig are the record of
     * what actually happened and are never rewritten from the UI. */
    uint16_t keep_len = g_co.pref_min, keep_goal = g_co.day_goal;
    coach_state_init(&g_co);
    g_co.pref_min = keep_len; g_co.day_goal = keep_goal;
    co_save();
    toast_show("Counters reset");
    show_coach();
}
static void act_ka_reset(lv_event_t *e){ (void)e; menu_close(); ka_reset_progress(); show_kana(); }
static void act_toggle_done(lv_event_t *e){ (void)e; menu_close();
    g_todo_show_done = !g_todo_show_done; if(cur_app) app_reopen(cur_app); }
static void act_toggle_sort(lv_event_t *e){ (void)e; menu_close();
    g_todo_sort_due = !g_todo_sort_due; if(cur_app) app_reopen(cur_app); }

/* debug: seed 30 test appointments into the Date Book so a >24-record collection
 * can be pushed to iCloud to exercise the streaming reconcile. Each is a new
 * record (uid 0 => data layer assigns a fresh uniqueID); the next HotSync pushes
 * all of them up. C5: dev scaffolding.
 *
 * Guarded by UI_SEED_TESTEVENTS rather than UI_DEVTOOLS, and that define is set
 * for SIM BUILDS ONLY. It was harmless while the sync could not complete; now
 * that two-way sync is on, one tap would push 30 fabricated appointments into
 * the real iCloud account, and the records it leaves behind are ordinary ones
 * that "Remove demo data" does not track. The sim UI tour still exercises it. */
#ifdef UI_SEED_TESTEVENTS
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
#endif /* UI_SEED_TESTEVENTS */

/* I2: remove the demo seed before the first HotSync, so Johnny Appleseed and the
 * fake meetings never get pushed into the user's real iCloud. Deletes only the
 * seeded records (the manifest tracks them); user edits/additions are kept. */
static void act_remove_demo(lv_event_t *e){ (void)e; menu_close();
    int n = data_remove_demo();
    if(cur_app) app_reopen(cur_app); else show_launcher();
    char msg[40]; snprintf(msg, sizeof msg, "Removed %d demo record%s", n, n==1?"":"s");
    toast_show(msg);
}

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
                            "Memos stay on this device.\n"
                            "To Dos sync as CalDAV tasks,\n"
                            "not the Reminders app.\n\n"
                            "v0.3 - tap to close");
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);
}

static lv_obj_t *menu_item_lbl(lv_obj_t *par, const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return l;
}
static void menu_item(lv_obj_t *par, const char *txt, lv_event_cb_t cb){
    (void)menu_item_lbl(par, txt, cb);
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
    if(g_trainer_open)
        menu_item(panel, "Reset progress", act_tr_reset);   /* Graffiti trainer only */
    if(g_kana_open)
        menu_item(panel, "Reset progress", act_ka_reset);   /* Kana trainer only */
    if(g_co_open){                                          /* Coach only */
        menu_header(panel, "Coach");
        static char lenbuf[24], goalbuf[24];
        snprintf(lenbuf,  sizeof lenbuf,  "Length: %d min", (int)g_co.pref_min);
        snprintf(goalbuf, sizeof goalbuf, "Day goal: %d",   (int)g_co.day_goal);
        g_co_len_lbl  = menu_item_lbl(panel, lenbuf,  act_co_len);
        g_co_goal_lbl = menu_item_lbl(panel, goalbuf, act_co_goal);
        menu_item(panel, "Marks",       act_co_marks);
        menu_item(panel, "This week",   act_co_week);
        menu_item(panel, "Reset counters", act_co_reset);
    }
    if(data_demo_present())
        menu_item(panel, "Remove demo data", act_remove_demo);
#ifdef UI_SEED_TESTEVENTS
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
    content_clear();
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
static void ce_edit_item(lv_obj_t *par);   /* "Edit Categories" tail item (C4) */

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
    ce_edit_item(panel);   /* Palm: the picker's last row edits the categories */
}

/* ---------------- C4: Edit Categories (rename existing / add new) ----------------
 * Palm's "Edit Categories" from the tail of the category picker. Renames land in
 * the app's PDB AppInfo immediately (data_set_categories preserves records; a
 * record's category nibble is unchanged, so a rename retags everything in that
 * category). "Unfiled" (slot 0) is reserved and not listed. Delete is out of
 * scope here -- it would need to recategorise the affected records to Unfiled.
 * All widgets are the pool-safe kind: a list of buttons, and for naming, one
 * textarea + one button-matrix keyboard (the Preferences I1.2 pattern). */
static int g_ce_app;    /* app whose categories are being edited */
static int g_ce_slot;   /* category slot (1..15) being named */
static void show_cat_edit(void);
static void show_cat_name_edit(int slot);

static void cat_name_cancel_cb(lv_event_t *e){ (void)e; show_cat_edit(); }
static void cat_name_save_cb(lv_event_t *e){ (void)e;
    CatTable t;
    if(data_get_categories(g_ce_app, &t)){
        snprintf(t.name[g_ce_slot], sizeof t.name[0], "%s", lv_textarea_get_text(g_fields[0]));
        if(data_set_categories(g_ce_app, &t)) toast_show("Saved");
    }
    show_cat_edit();
}

static void show_cat_name_edit(int slot){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0; g_ce_slot = slot;
    content_clear();
    lv_label_set_text(title_lbl, "Category");
    update_cat_trigger();

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, 60, 28); lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *cl=lv_label_create(cancel); lv_label_set_text(cl,"Cancel"); lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, cat_name_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save = lv_button_create(content);
    lv_obj_set_size(save, 60, 28); lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *sl=lv_label_create(save); lv_label_set_text(sl,"Save"); lv_obj_center(sl);
    lv_obj_add_event_cb(save, cat_name_save_cb, LV_EVENT_CLICKED, NULL);

    CatTable t; const char *cur = "";
    if(data_get_categories(g_ce_app, &t)) cur = t.name[slot];
    lv_obj_t *lb = lv_label_create(content);
    lv_label_set_text(lb, "Name:");
    lv_obj_set_pos(lb, 4, 38);
    lv_obj_t *ta = lv_textarea_create(content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (int)sizeof t.name[0] - 1);   /* 15 chars */
    lv_textarea_set_text(ta, cur ? cur : "");
    lv_obj_set_width(ta, LCD_W - 16);
    lv_obj_set_pos(ta, 4, 54);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    g_fields[0] = ta; g_nfields = 1; active_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    lv_obj_t *bm = lv_buttonmatrix_create(content);
    lv_obj_set_size(bm, LCD_W - 4, (PDA_H - TITLE_H) - 92);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_buttonmatrix_set_map(bm, KB_LOWER);
    lv_obj_set_style_radius(bm, 0, 0);
    lv_obj_set_style_radius(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, prefkb_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void ce_row_cb(lv_event_t *e){ show_cat_name_edit((int)(intptr_t)lv_event_get_user_data(e)); }
static void ce_done_cb(lv_event_t *e){ (void)e; app_reopen(&APPDEFS[g_ce_app]); }
static void ce_new_cb(lv_event_t *e){ (void)e;
    CatTable t; if(!data_get_categories(g_ce_app, &t)) return;
    for(int c=1;c<CAT_COUNT;c++) if(!t.name[c][0]){ show_cat_name_edit(c); return; }
    toast_show("Categories full");
}

static void show_cat_edit(void){
    kill_kb();
    cur_app = NULL; cur_uid = 0; g_nfields = 0;
    content_clear();
    lv_label_set_text(title_lbl, "Edit Categories");
    update_cat_trigger();

    lv_obj_t *done = lv_button_create(content);
    lv_obj_set_size(done, 60, 28); lv_obj_align(done, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_t *dl=lv_label_create(done); lv_label_set_text(dl,"Done"); lv_obj_center(dl);
    lv_obj_add_event_cb(done, ce_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nw = lv_button_create(content);
    lv_obj_set_size(nw, 60, 28); lv_obj_align(nw, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_t *nl=lv_label_create(nw); lv_label_set_text(nl,"New"); lv_obj_center(nl);
    lv_obj_add_event_cb(nw, ce_new_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, LCD_W, FORM_FULL);
    lv_obj_set_pos(list, 0, 34);
    lv_obj_set_style_radius(list,0,0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_all(list,0,0);
    CatTable t; int have = data_get_categories(g_ce_app, &t);
    int shown = 0;
    if(have) for(int c=1;c<CAT_COUNT;c++) if(t.name[c][0]){
        lv_obj_t *b=lv_list_add_button(list,NULL,t.name[c]);
        lv_obj_set_style_radius(b,0,0);
        lv_obj_add_event_cb(b,ce_row_cb,LV_EVENT_CLICKED,(void*)(intptr_t)c);
        shown++;
    }
    if(!shown){
        lv_obj_t *b=lv_list_add_button(list,NULL,"(tap New to add one)");
        lv_obj_set_style_radius(b,0,0);
    }
}

static void ce_open_cb(lv_event_t *e){ (void)e;
    if(!cur_app) return;
    g_ce_app = cur_app->app;
    catpop_close();
    show_cat_edit();
}
static void ce_edit_item(lv_obj_t *par){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(b, COL_LINE, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "Edit Categories");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(b, ce_open_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------- F4: Details (category) ------------------------- */
static lv_obj_t *g_details;
static void details_close(void){ if(g_details){ lv_obj_del(g_details); g_details=NULL; } }
static void details_backdrop_cb(lv_event_t *e){ (void)e; details_close(); }
static void details_pick_cb(lv_event_t *e){ edit_cat = (int)(intptr_t)lv_event_get_user_data(e); details_close(); set_editcat_label(); }

/* Date Book Details: Alarm on/off + Repeat cycle. Plain buttons that relabel in
 * place (pool-safe); the picked state lives in g_ev_* until Save. */
static lv_obj_t *g_ev_alarm_lbl, *g_ev_repeat_lbl;
static void ev_alarm_cb(lv_event_t *e){ (void)e;
    g_ev_alarm = !g_ev_alarm;
    if(g_ev_alarm_lbl) lv_label_set_text_fmt(g_ev_alarm_lbl, "Alarm: %s", g_ev_alarm?"On":"Off");
}
static void ev_repeat_cb(lv_event_t *e){ (void)e;
    g_ev_repeat = repeat_next(g_ev_repeat);
    if(g_ev_repeat_lbl) lv_label_set_text_fmt(g_ev_repeat_lbl, "Repeat: %s", repeat_name(g_ev_repeat));
}
static lv_obj_t *details_row(lv_obj_t *panel, const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(panel);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return l;
}

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

    if(cur_app->app == APP_CAL){    /* Date Book: alarm + repeat above the category */
        lv_obj_t *eh = lv_label_create(panel);
        lv_label_set_text(eh, "Event:");
        lv_obj_set_style_text_font(eh, &lv_font_palm_bold, 0);
        char buf[24];
        snprintf(buf, sizeof buf, "Alarm: %s", g_ev_alarm?"On":"Off");
        g_ev_alarm_lbl = details_row(panel, buf, ev_alarm_cb);
        snprintf(buf, sizeof buf, "Repeat: %s", repeat_name(g_ev_repeat));
        g_ev_repeat_lbl = details_row(panel, buf, ev_repeat_cb);
    }

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

/* Ink is written straight to the draw buffer via the shared I1 helpers -- see
 * i1_px() for why lv_canvas_set_px is unusable here. This pad is where the cost
 * was actually measured: clearing it cost 80 ms during ui_init and 1.006 SECONDS
 * once the full UI was live, and because that stall sits inside lv_timer_handler
 * (the same loop that polls touch) a pen-down ate the whole stroke. Every real
 * stroke on hardware sampled exactly 1 point -- below the 4-point floor in
 * graffiti_recognize() -- so nothing drew and nothing was recognized. */
static lv_draw_buf_t *ink_db;                 /* cached draw buf for the ink canvas */

static void inkpx(int x, int y){ i1_px(ink_db, x, y, 1); }
static void ink_clear(void){
    if(!ink_canvas) return;
    i1_clear(ink_db);                                  /* palette index 0 = COL_GRAF */
    lv_obj_invalidate(ink_canvas);                     /* one, not 17.8k */
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
    /* bounding box of this segment, invalidated once after the run (+1 for the
     * 2px pen). Only the touched strip is redrawn, so the SPI blit stays small. */
    int bx1 = x0 < x ? x0 : x, bx2 = x0 > x ? x0 : x;
    int by1 = y0 < y ? y0 : y, by2 = y0 > y ? y0 : y;
    int dx = x > x0 ? x - x0 : x0 - x, sx_ = x0 < x ? 1 : -1;
    int dy = y > y0 ? y0 - y : y - y0, sy_ = y0 < y ? 1 : -1;   /* dy <= 0 */
    int err = dx + dy;
    for(;;){
        inkpx(x0, y0);
        inkpx(x0 + 1, y0);
        inkpx(x0, y0 + 1);
        if(x0 == x && y0 == y) break;
        int e2 = 2 * err;
        if(e2 >= dy){ err += dy; x0 += sx_; }
        if(e2 <= dx){ err += dx; y0 += sy_; }
    }
    ink_lx = x; ink_ly = y;

    lv_area_t a;
    lv_obj_get_coords(ink_canvas, &a);           /* absolute; invalidate_area clips */
    int ax = a.x1, ay = a.y1;
    a.x1 = ax + bx1;     a.y1 = ay + by1;
    a.x2 = ax + bx2 + 1; a.y2 = ay + by2 + 1;
    lv_obj_invalidate_area(ink_canvas, &a);
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
    if(graf_capture_hook && graf_capture_hook()){      /* trainer train-mode: grab raw stroke */
        graffiti_clear();
        return;
    }
    char c = graffiti_recognize(digits);
    if(c) graf_echo(c);                                /* flash what was recognized */
    if(!c){ show_punct(0); return; }                   /* nothing / punct rejected */
    if(c == GRAF_SHIFT){ graf_case = (graf_case + 1) % 3; show_case(); return; }
    if(c == GRAF_PUNCT){ show_punct(1); return; }       /* tap: arm punctuation */
    show_punct(0);                                      /* any real char clears it */
    if(graf_char_hook){ graf_char_hook(c); return; }    /* trainer intercepts input */
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

/* ===================== Lock-screen dashboard (roadmap: product) =============
 * A full-screen, info-dense glance view drawn in the mono Palm LCD: big clock,
 * two world times, cached weather (temp / rain / air + a 6-hour strip), battery,
 * next event + next due, sunrise/sunset, and the moon phase. It renders entirely
 * OFFLINE -- weather comes from an SD snapshot refreshed on HotSync (dash.c); the
 * clock/agenda/astronomy are computed on-device. Pool-safe: one I1 canvas for all
 * the graphics (clock digits, moon, rain bars) plus flat labels; no draw-layer
 * widgets. Swipe up to unlock into the launcher. */
#define DASH_CW LCD_W
#define DASH_CH LCD_H
static lv_obj_t *g_lock;                 /* the overlay root, or NULL when unlocked */
static lv_obj_t *g_dash_cv;              /* the I1 graphics canvas */
static lv_obj_t *g_dash_time_ap;         /* AM/PM label (repositioned to the clock width) */
static WxCache   g_wx;                    /* weather snapshot for this lock session */
static int       g_havewx;                /* 1 if g_wx is valid */
static uint8_t   dash_buf[LV_CANVAS_BUF_SIZE(DASH_CW, DASH_CH, 1, 1) + 16];

/* 4x7 pixel digits 0-9 (top row first; a set bit = leftmost of 4 columns). Drawn
 * scaled onto the canvas so the hero clock needs no large font. */
static const uint8_t DASH_DIG[10][7] = {
    {6,9,9,9,9,9,6},{2,6,2,2,2,2,7},{6,9,1,2,4,8,15},{14,1,1,6,1,1,14},
    {1,3,5,9,15,1,1},{15,8,14,1,1,9,6},{6,8,8,14,9,9,6},{15,1,2,2,4,4,4},
    {6,9,9,6,9,9,6},{6,9,9,7,1,1,6},
};
static const char *DASH_DOW_L[] = {"Sunday","Monday","Tuesday","Wednesday",
                                   "Thursday","Friday","Saturday"};
static const char *DASH_DOW_S[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *DASH_MON_L[] = {"","January","February","March","April","May",
    "June","July","August","September","October","November","December"};

static int ti_wday(time_t t){ struct tm x; localtime_r(&t,&x); return x.tm_wday; }
static int localtime_mon(time_t t){ struct tm x; localtime_r(&t,&x); return x.tm_mon+1; }
static int localtime_mday(time_t t){ struct tm x; localtime_r(&t,&x); return x.tm_mday; }
static const char *month_long(int m){ return (m>=1&&m<=12)?DASH_MON_L[m]:""; }

/* "45m" / "3h" / "2d" since the weather snapshot was made. Bare span, no "ago",
 * so it reads correctly both after "synced" and inside the stale-weather line. */
static const char *dash_age_span(const WxCache *w){
    static char b[24];
    int mins = dash_weather_age_min(w);
    if(mins < 60)        snprintf(b,sizeof b,"%dm",mins);
    else if(mins < 1440) snprintf(b,sizeof b,"%dh",mins/60);
    else                 snprintf(b,sizeof b,"%dd",mins/1440);
    return b;
}
/* US-AQI band word. */
static const char *aqi_word(int aqi){
    if(aqi<=50) return "Good";
    if(aqi<=100) return "Moderate";
    if(aqi<=150) return "Unhealthy*";       /* sensitive groups */
    if(aqi<=200) return "Unhealthy";
    if(aqi<=300) return "Very unhealthy";
    return "Hazardous";
}

/* The dashboard is written straight to the draw buffer via the shared I1 helpers
 * -- see i1_px() for why lv_canvas_set_px is unusable here. At 240x320 a full
 * repaint through set_px cost ~77k tree walks: seconds of solid CPU inside one
 * lv_timer_handler pass, which starved IDLE0 into a task-watchdog trigger and,
 * because touch is serviced from that same loop, swallowed the unlock swipe and
 * left the screen half drawn. dash_paint issues one invalidate at the end. */
static lv_draw_buf_t *g_dash_db;      /* cached draw buf for the dash canvas */

static void dpx(int x,int y){ i1_px(g_dash_db, x, y, 1); }   /* index 1 = COL_LINE */
static void dash_clear(void){        /* clear to palette index 0 (COL_BODY) */
    i1_clear(g_dash_db);
}
static void dfill(int x,int y,int w,int h){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) dpx(x+i,y+j);
}
static void dhdots(int x0,int x1,int y){ for(int x=x0;x<=x1;x+=3) dpx(x,y); }

/* The hero clock is drawn as seven-segment digits: bars with 45-degree bevelled
 * ends that miter cleanly at the corners, so the big time reads as a smooth
 * calculator-style clock instead of the old blocky 4x7 bitmap. Segments a..g map
 * to bits 1,2,4,8,16,32,64. */
#define SEG_L   18                        /* segment length */
#define SEG_T   6                         /* segment thickness */
#define SEG_GAP 5                         /* gap between digit boxes */
static const uint8_t SEG7[10] = {63,6,91,79,102,109,125,7,127,111};
static void dseg_h(int xc,int y,int len,int t){          /* horizontal bar, left x=xc, mid-line y */
    int half=t/2;
    for(int i=-half;i<=half;i++){ int in=half-(i<0?-i:i);
        for(int x=xc+in;x<=xc+len-in;x++) dpx(x,y+i); }
}
static void dseg_v(int x,int yc,int len,int t){          /* vertical bar, top y=yc, mid-line x */
    int half=t/2;
    for(int i=-half;i<=half;i++){ int in=half-(i<0?-i:i);
        for(int y=yc+in;y<=yc+len-in;y++) dpx(x+i,y); }
}
static void dash_digit7(int x,int y,int d,int L,int t){
    if(d<0||d>9) return;
    uint8_t m=SEG7[d];
    if(m&1)  dseg_h(x,   y,     L, t);    /* a top       */
    if(m&2)  dseg_v(x+L, y,     L, t);    /* b top-right */
    if(m&4)  dseg_v(x+L, y+L,   L, t);    /* c bot-right */
    if(m&8)  dseg_h(x,   y+2*L, L, t);    /* d bottom    */
    if(m&16) dseg_v(x,   y+L,   L, t);    /* e bot-left  */
    if(m&32) dseg_v(x,   y,     L, t);    /* f top-left  */
    if(m&64) dseg_h(x,   y+L,   L, t);    /* g middle    */
}
/* draw a "H:MM"/"HH:MM" string in the seven-seg clock; returns pixel width drawn. */
static int dash_bigtime(int x,int y,const char *str){
    int x0=x;
    for(const char *p=str; *p; p++){
        if(*p==':'){
            int cx = x + SEG_T/2;
            dfill(cx, y + SEG_L - SEG_T,   SEG_T, SEG_T);
            dfill(cx, y + SEG_L + SEG_T/2, SEG_T, SEG_T);
            x += 2*SEG_T + SEG_GAP;
        } else if(*p>='0' && *p<='9'){
            dash_digit7(x, y, *p-'0', SEG_L, SEG_T);
            x += SEG_L + SEG_T + SEG_GAP;
        }
    }
    return x - x0 - SEG_GAP;
}
static int dash_bigtime_w(const char *str){
    int w=0; for(const char *p=str; *p; p++) w += (*p==':') ? (2*SEG_T+SEG_GAP) : (SEG_L+SEG_T+SEG_GAP);
    return w>0 ? w-SEG_GAP : 0;
}
/* format a zone's wall clock at time t honouring the 12/24h setting, e.g.
 * "9:34a" (12h) or "09:34" (24h). Empty on a bad zone. */
static void dash_zone_fmt(const char *zone, time_t t, int h24, char *out, int cap){
    char hm[16]; clock_zone_hhmm(zone, t, hm, sizeof hm);   /* 24h "HH:MM" */
    if(!hm[0] || cap<=0){ if(cap>0) out[0]=0; return; }
    int H = (hm[0]-'0')*10 + (hm[1]-'0');
    if(h24){ snprintf(out, cap, "%s", hm); return; }
    int h12 = H%12; if(h12==0) h12=12;
    snprintf(out, cap, "%d:%c%c%s", h12, hm[3], hm[4], H<12?"a":"p");
}
/* short 3-letter tag for a world-clock zone: the city after '/', upper-cased,
 * first 3 letters (e.g. "Europe/London" -> "LON", "America/New_York" -> "NEW"). */
static void world_tag(const char *zone, char *out, int cap){
    if(cap<=0){ return; }
    const char *city = strrchr(zone, '/');
    city = city ? city+1 : zone;
    int n=0;
    for(; city[n] && n<3 && n<cap-1; n++){
        char c = city[n];
        out[n] = (c>='a'&&c<='z') ? (char)(c-'a'+'A') : c;
    }
    out[n]=0;
}

/* moon disc: illuminated fraction k=illum/100, lit on the right when waxing. */
static void dash_moon_draw(int cx,int cy,int r,int illum,int waxing){
    double k = illum/100.0;
    for(int dy=-r; dy<=r; dy++){
        double span = (r*r - dy*dy);
        span = span>0 ? sqrt(span) : 0;
        double xt = span*(2.0*k - 1.0);
        for(int dx=-r; dx<=r; dx++){
            double dist = sqrt((double)(dx*dx + dy*dy));
            if(dist > r+0.5) continue;
            int lit = waxing ? (dx >= xt) : (dx <= -xt);
            if(dist > r-0.9 || !lit) dpx(cx+dx, cy+dy);   /* outline + shadow are ink */
        }
    }
}

/* soonest upcoming appointment -> "2:30p  Dentist" (1 if found). Recurrence is not
 * expanded (the base date is used); good enough for a glance. */
typedef struct { long best; char line[64]; int found; } NextEv;
static void next_ev_cb(uint32_t uid,const char *pri,const char *sec,void *ctx){
    (void)pri; (void)sec;
    NextEv *n = ctx;
    Appt a;
    if(!data_get_cal(uid,&a)) return;
    if(a.year < 2000) return;
    struct tm ti; memset(&ti,0,sizeof ti);
    ti.tm_year=a.year-1900; ti.tm_mon=a.month-1; ti.tm_mday=a.day;
    ti.tm_hour=a.hasTime?a.sH:0; ti.tm_min=a.hasTime?a.sM:0; ti.tm_isdst=-1;
    time_t t = mktime(&ti);
    time_t now=0; time(&now);
    if(a.hasTime){
        if(t < now) return;                     /* timed event already started/passed */
    } else {                                    /* all-day: keep only today or later */
        struct tm nt; localtime_r(&now,&nt);
        long ad = (a.year*10000L)+(a.month*100L)+a.day;
        long nd = ((nt.tm_year+1900)*10000L)+((nt.tm_mon+1)*100L)+nt.tm_mday;
        if(ad < nd) return;
    }
    if(n->found && t >= n->best) return;
    n->best=(long)t; n->found=1;
    if(a.hasTime){
        int h=a.sH%12; if(h==0) h=12;
        snprintf(n->line,sizeof n->line,"%d:%02d%s  %.40s",
                 h,a.sM,a.sH<12?"a":"p",a.description);
    } else {
        snprintf(n->line,sizeof n->line,"all day  %.40s",a.description);
    }
}
static int dash_next_event(char *out,int cap){
    NextEv n; n.best=0; n.found=0; n.line[0]=0;
    data_datebook(next_ev_cb,&n);
    if(!n.found) return 0;
    snprintf(out,cap,"%s",n.line); return 1;
}

/* soonest-due incomplete to-do (overdue sorts first) -> "Today  Pay rent". */
typedef struct { long best; char line[64]; int found; } NextTd;
static void next_td_cb(uint32_t uid,const char *pri,const char *sec,void *ctx){
    (void)pri; (void)sec;
    NextTd *n = ctx;
    Todo t;
    if(!data_get_todo(uid,&t)) return;
    if(t.completed || !t.hasDue) return;
    struct tm ti; memset(&ti,0,sizeof ti);
    ti.tm_year=t.dueY-1900; ti.tm_mon=t.dueM-1; ti.tm_mday=t.dueD; ti.tm_isdst=-1;
    time_t due = mktime(&ti);
    if(n->found && due >= n->best) return;
    n->best=(long)due; n->found=1;
    time_t now=0; time(&now); struct tm nt; localtime_r(&now,&nt);
    const char *when;
    char wb[16];
    if(t.dueY==nt.tm_year+1900 && t.dueM==nt.tm_mon+1 && t.dueD==nt.tm_mday) when="Today";
    else if(due < now){ when="Overdue"; }
    else { struct tm dt; localtime_r(&due,&dt); snprintf(wb,sizeof wb,"%s %d",CAL_MON[t.dueM],t.dueD); when=wb; }
    snprintf(n->line,sizeof n->line,"%s  %.40s",when,t.description);
}
static int dash_next_due(char *out,int cap){
    NextTd n; n.best=0; n.found=0; n.line[0]=0;
    data_todo(next_td_cb,&n);
    if(!n.found) return 0;
    snprintf(out,cap,"%s",n.line); return 1;
}

/* a small left-aligned label on the overlay at (x,y), Palm font, optional bold. */
static lv_obj_t *dash_lbl(int x,int y,const char *txt,int bold){
    lv_obj_t *l = lv_label_create(g_lock);
    lv_obj_set_style_text_font(l, bold?&lv_font_palm_bold:&lv_font_palm, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* swipe-up detection (same robust manual scheme the News reader uses). */
static int g_lock_py, g_lock_ly;
static void lock_press_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(),&p); g_lock_py=g_lock_ly=p.y; }
static void lock_pressing_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(),&p); if(p.y>0) g_lock_ly=p.y; }
static void lock_release_cb(lv_event_t *e){ (void)e;
    if(g_lock_py - g_lock_ly > 40){                 /* dragged up -> unlock */
        if(g_lock){ lv_obj_del(g_lock); g_lock=NULL; g_dash_cv=NULL; g_dash_db=NULL; g_dash_time_ap=NULL; }
        /* the launcher is built lazily on the FIRST unlock (at boot the content area
         * is empty behind the lock, so the launcher grid and the dashboard never share
         * the 24 KB pool). Later wakes re-lock over whatever app is showing, so only
         * rebuild the launcher when nothing is there. */
        if(lv_obj_get_child_count(content) == 0) show_launcher();
    }
}

/* paint the canvas graphics + (re)set the time labels from the current clock. */
static void dash_paint(void){
    if(!g_lock || !g_dash_cv) return;

    dash_clear();

    time_t now=0; time(&now); struct tm ti; localtime_r(&now,&ti);
    int h24 = appcfg()->clock24;
    int hh = h24 ? ti.tm_hour : (ti.tm_hour%12 ? ti.tm_hour%12 : 12);
    char tb[8]; snprintf(tb,sizeof tb,"%d:%02d",hh,ti.tm_min);
    int tw=dash_bigtime_w(tb), tx=10, ty=28;
    dash_bigtime(tx,ty,tb);
    if(g_dash_time_ap){                              /* AM/PM only in 12-hour mode */
        if(h24){ lv_label_set_text(g_dash_time_ap, ""); }
        else {
            lv_label_set_text(g_dash_time_ap, ti.tm_hour<12?"AM":"PM");
            lv_obj_set_pos(g_dash_time_ap, tx+tw+8, ty+14);
        }
    }
    /* zone separators + unlock chevron */
    dhdots(10,DASH_CW-10,104);
    dhdots(10,DASH_CW-10,140);
    dhdots(10,DASH_CW-10,220);
    dhdots(10,DASH_CW-10,262);
    for(int i=0;i<6;i++){ dpx(DASH_CW/2-6+i,306-i); dpx(DASH_CW/2+6-i,306-i); }

    /* hourly rain-probability bars (fill_bg above wiped the canvas, so all the
     * graphics are redrawn here, not in ui_show_lock). */
    if(g_havewx){
        for(int i=0;i<g_wx.nhours && i<6;i++){
            int cx = 22 + i*39, bh = g_wx.hr[i].rain*28/100;
            dfill(cx-7,190-bh,15,bh?bh:1);
            dhdots(cx-8,cx+8,191);
        }
    }
    /* moon disc (far right, clear of its label) */
    { int illum=0,wax=1;
      dash_moon(now,&illum,&wax,NULL);
      dash_moon_draw(224,282,13,illum,wax); }

    /* one invalidate for the whole canvas, instead of one per pixel */
    lv_obj_invalidate(g_dash_cv);
}

void ui_show_lock(void){
    /* COACH OWNS THE SCREEN: while a session runs the mark and the countdown are
     * what a tap must reveal, and once it ends the reflect screen is. The port
     * layer raises the lock when the screen sleeps and refreshes it on every wake,
     * so without this a session that ended while the device was face-down would be
     * answered by the dashboard -- and content_clear() below would have deleted
     * "how did it go" on the way past. */
    if(co_owns_screen()) return;
    if(g_lock){ dash_paint(); return; }             /* already showing -> just refresh */
    /* Free whatever app view is in the content area first. The lock covers the whole
     * screen anyway, and this keeps the 24 KB LVGL pool holding only the chrome + the
     * dashboard at once (never chrome + an app + the dashboard). The content area is
     * left empty, so unlocking rebuilds the launcher (see lock_release_cb). */
    content_clear();
    kill_kb();
    time_t now=0; time(&now);
    dash_weather_seed_sample(WX_PATH);
    /* Two different questions. `loaded` is "is there a snapshot at all", which is
     * what the age line reports; `havewx` is "may it be drawn", which every actual
     * reading is gated on. A day-old temperature is not a late temperature, it is
     * the wrong one, and this screen is read at a glance with no second look. */
    WxCache wx; int loaded = dash_weather_load(&wx);
    int havewx = loaded && dash_weather_fresh(&wx, WX_STALE_MIN);
    g_wx = wx; g_havewx = havewx;                    /* dash_paint() draws the bars from this */

    g_lock = lv_obj_create(lv_screen_active());     /* on the active screen (like News),
                                                       so the swipe events fire reliably */
    lv_obj_set_size(g_lock, LCD_W, LCD_H);
    lv_obj_set_pos(g_lock, 0, 0);
    lv_obj_set_style_radius(g_lock, 0, 0);
    lv_obj_set_style_border_width(g_lock, 0, 0);
    lv_obj_set_style_bg_color(g_lock, COL_BODY, 0);
    lv_obj_set_style_pad_all(g_lock, 0, 0);
    lv_obj_set_style_text_font(g_lock, &lv_font_palm, 0);
    lv_obj_clear_flag(g_lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_lock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_lock, lock_press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(g_lock, lock_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_lock, lock_release_cb,  LV_EVENT_RELEASED, NULL);

    g_dash_cv = lv_canvas_create(g_lock);
    lv_canvas_set_buffer(g_dash_cv, dash_buf, DASH_CW, DASH_CH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_dash_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_dash_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_set_pos(g_dash_cv, 0, 0);
    lv_obj_clear_flag(g_dash_cv, LV_OBJ_FLAG_CLICKABLE);
    g_dash_db = lv_canvas_get_draw_buf(g_dash_cv);   /* dpx/dash_clear write this directly */

    /* ---- status strip ---- */
    char sb[48];
    snprintf(sb,sizeof sb,"%s \xC2\xB7 %s %d",
             DASH_DOW_S[ti_wday(now)], CAL_MON[localtime_mon(now)], localtime_mday(now));
    dash_lbl(6,4,sb,0);
    int bp = power_battery_pct();
    if(loaded) snprintf(sb,sizeof sb,"synced %s ago \xC2\xB7 ", dash_age_span(&wx));
    else       snprintf(sb,sizeof sb," ");
    if(bp>=0) snprintf(sb+strlen(sb),sizeof sb-strlen(sb),"%d%%",bp);
    else      snprintf(sb+strlen(sb),sizeof sb-strlen(sb),"USB");
    lv_obj_t *sr=dash_lbl(0,4,sb,0); lv_obj_align(sr,LV_ALIGN_TOP_RIGHT,-6,4);

    /* ---- AM/PM (positioned beside the hero clock in dash_paint) ---- */
    g_dash_time_ap = dash_lbl(0,0,"",1);

    /* ---- world clocks: their OWN row BELOW the clock, so a two-digit hour's
     * AM/PM can never collide with them. Zones + 12/24h come from Preferences;
     * an empty zone hides that slot. The 3-letter tag is derived from the city. */
    { const Config *cf = appcfg();
      int h24 = cf->clock24;
      const char *zs[2] = { cf->world1, cf->world2 };
      int wx0[2] = { 10, 128 };
      for(int k=0;k<2;k++){
          if(!zs[k][0]) continue;
          char tag[8]; world_tag(zs[k], tag, sizeof tag);
          char tm[16]; dash_zone_fmt(zs[k], now, h24, tm, sizeof tm);
          char l[28]; snprintf(l,sizeof l,"%s %s", tag, tm);
          dash_lbl(wx0[k], 74, l, 0);
      }
    }

    /* ---- date line ---- */
    { char db[40]; snprintf(db,sizeof db,"%s, %s %d",
        DASH_DOW_L[ti_wday(now)], month_long(localtime_mon(now)), localtime_mday(now));
      dash_lbl(10,90,db,0); }

    /* ---- weather ---- */
    if(havewx){
        char wl[48];
        snprintf(wl,sizeof wl,"%d\xC2\xB0  %s",wx.cur_tempF,dash_wcode_desc(wx.cur_code));
        dash_lbl(10,110,wl,1);
        if(wx.aqi>=0){ snprintf(wl,sizeof wl,"Air %d \xC2\xB7 %s",wx.aqi,aqi_word(wx.aqi)); dash_lbl(10,126,wl,0); }
        /* 6-hour strip: temp (top), rain bar (canvas), hour + rain% (bottom) */
        for(int i=0;i<wx.nhours && i<6;i++){
            int cx = 22 + i*39;
            char c[12];
            int hh=wx.hr[i].hour24%12; if(hh==0) hh=12;
            snprintf(c,sizeof c,"%d\xC2\xB0",wx.hr[i].tempF); dash_lbl(cx-8,146,c,0);
            /* the rain bar itself is drawn on the canvas in dash_paint() */
            snprintf(c,sizeof c,"%d%s",hh,wx.hr[i].hour24<12?"a":"p"); dash_lbl(cx-8,196,c,0);
            snprintf(c,sizeof c,"%d%%",wx.hr[i].rain);     dash_lbl(cx-8,208,c,0);
        }
    } else if(loaded){
        /* Say which it is. A blank space where the weather was reads as a fault; a
         * line naming the age reads as a decision, and points at the fix. Centred
         * in the band the readings vacated, not pinned to the top of it, so the
         * space around it looks emptied on purpose rather than half-drawn. */
        char wl[48];
        snprintf(wl,sizeof wl,"Weather is %s old", dash_age_span(&wx));
        lv_obj_t *o = dash_lbl(0,0,wl,1);
        lv_obj_align(o,LV_ALIGN_TOP_MID,0,150);
        o = dash_lbl(0,0,"hidden until the next HotSync",0);
        lv_obj_align(o,LV_ALIGN_TOP_MID,0,170);
    } else {
        dash_lbl(10,118,"Weather syncs on HotSync",0);
    }

    /* ---- agenda ---- */
    { char e[64];
      if(dash_next_event(e,sizeof e)){ char l[80]; snprintf(l,sizeof l,"NEXT  %s",e); dash_lbl(10,226,l,0); }
      else dash_lbl(10,226,"NEXT  no upcoming events",0);
      if(dash_next_due(e,sizeof e)){ char l[80]; snprintf(l,sizeof l,"DUE   %s",e); dash_lbl(10,244,l,0); }
      else dash_lbl(10,244,"DUE   nothing due",0); }

    /* ---- sun + moon ---- */
    if(havewx && wx.sunrise_min>=0){
        char sun[24];
        int rh=wx.sunrise_min/60, rm=wx.sunrise_min%60, sh=wx.sunset_min/60, sm=wx.sunset_min%60;
        int rh12=rh%12; if(rh12==0) rh12=12; int sh12=sh%12; if(sh12==0) sh12=12;
        snprintf(sun,sizeof sun,"Rise  %d:%02d%s",rh12,rm,rh<12?"a":"p"); dash_lbl(10,268,sun,0);
        snprintf(sun,sizeof sun,"Set   %d:%02d%s",sh12,sm,sh<12?"a":"p"); dash_lbl(10,284,sun,0);
    }
    { int illum=0; const char *nm="";
      dash_moon(now,&illum,NULL,&nm);       /* the disc is drawn on the canvas in dash_paint() */
      char ml[24]; snprintf(ml,sizeof ml,"%s",nm);
      lv_obj_t*o=dash_lbl(0,266,ml,0); lv_obj_align(o,LV_ALIGN_TOP_RIGHT,-54,266);
      snprintf(ml,sizeof ml,"%d%% lit",illum);
      o=dash_lbl(0,282,ml,0); lv_obj_align(o,LV_ALIGN_TOP_RIGHT,-54,282); }

    /* ---- unlock hint ---- */
    { lv_obj_t*o=dash_lbl(0,308,"swipe up to unlock",0); lv_obj_align(o,LV_ALIGN_BOTTOM_MID,0,-2); }

    dash_paint();
}

/* keep the locked clock fresh (minute tick); no-op while unlocked. */
/* The lock is now raised the moment the screen sleeps, so it is up and ticking
 * while the panel is dark -- and a full 240x320 canvas repaint plus a flush every
 * 15 s, forever, into a screen nobody is looking at, is exactly the kind of thing
 * that eats a battery quietly. Skip it while blanked; the wake repaints anyway. */
static void dash_tick(lv_timer_t *t){ (void)t;
    if(g_lock && !power_screen_off()) dash_paint();
}

/* ========================= Games (product roadmap) ==========================
 * A "Games" launcher app opening a small menu of low-RAM games. First up:
 * Minesweeper -- board logic in minesweeper.c (pure/testable), the view here on a
 * 1-bpp canvas (grid + stipple for unrevealed, the DASH_DIG font for counts, discs
 * for mines). A Dig/Flag mode toggle picks what a tap does (clearer than long-press
 * on a resistive panel). Pool-safe: one canvas + a few labels/buttons. */
/* ---- ONE 1-bpp canvas buffer, shared by every game ----------------------------
 * The four games are mutually exclusive screens: each show_*() calls kill_kb() and
 * content_clear(), which deletes the previous canvas -- and nulls the pointer to
 * it -- before the next one is created, so two game canvases can never be live at
 * once and no stale canvas pointer is left behind. Private buffers cost
 * ~18 KB of BSS on a board with 320 KB of DRAM and no PSRAM; one buffer sized to
 * the largest board costs 5 KB. LVGL reads the buffer only while drawing, and a
 * screen swap always happens between draws (from an event callback, never mid-
 * render), so the reuse is safe. Sized for the biggest game canvas -- Sudoku and
 * Zip are both 240x164; a smaller canvas simply uses less of it. */
#define GAME_CVW 240
#define GAME_CVH 164
static uint8_t game_cv_buf[LV_CANVAS_BUF_SIZE(GAME_CVW, GAME_CVH, 1, 1) + 16];

#define MSW 9
#define MSH 9
#define MSMINES 10
#define MSC 16                                  /* cell size in px */
#define MSCW (MSW*MSC+1)
#define MSCH (MSH*MSC+1)
static MsGame    g_ms;
static lv_obj_t *g_ms_cv, *g_ms_status, *g_ms_modelbl, *g_ms_timelbl;
static int       g_ms_flag;                     /* 1 = taps place flags, 0 = dig */
static uint32_t  g_ms_seq;                       /* varies the board each New */
static PlayClock g_ms_clk;                       /* pausable play timer (playclock.h) */
static uint32_t  g_ms_best;                      /* best winning time in seconds (0 = none yet) */

/* elapsed PLAY seconds: 0 before the first dig, live while the screen is open,
 * paused the moment you leave, frozen when the game ends. */
static uint32_t ms_elapsed(void){ return pc_secs(&g_ms_clk, (uint32_t)time(NULL)); }
static void ms_fmt_mmss(uint32_t sec, char *out, int cap){
    if(sec > 5999) sec = 5999;                   /* cap the readout at 99:59 */
    snprintf(out, cap, "%u:%02u", (unsigned)(sec/60), (unsigned)(sec%60));
}

static void mpx(int x,int y){
    i1_obj_px(g_ms_cv, x, y, 1);
}
static void mfill(int x,int y,int w,int h){ for(int j=0;j<h;j++) for(int i=0;i<w;i++) mpx(x+i,y+j); }
static void mdigit(int x,int y,int d){
    if(d<1||d>9) return;
    const uint8_t *g = DASH_DIG[d];
    for(int r=0;r<7;r++) for(int c=0;c<4;c++) if(g[r] & (8>>c)) mpx(x+c,y+r);
}
static void mdisc(int cx,int cy,int r){
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++) if(dx*dx+dy*dy<=r*r) mpx(cx+dx,cy+dy);
}
static void ms_render_body(void){
    i1_obj_clear(g_ms_cv);
    for(int r=0;r<=MSH;r++) for(int x=0;x<MSCW;x++) mpx(x, r*MSC);   /* grid */
    for(int c=0;c<=MSW;c++) for(int y=0;y<MSCH;y++) mpx(c*MSC, y);
    for(int r=0;r<MSH;r++) for(int c=0;c<MSW;c++){
        int x0=c*MSC, y0=r*MSC; uint8_t cb=ms_at(&g_ms,r,c); int mine=cb&MS_MINE;
        if(cb & MS_REVEALED){
            if(mine) mdisc(x0+MSC/2, y0+MSC/2, 4);
            else { int a=ms_adj(&g_ms,r,c); if(a>0) mdigit(x0+MSC/2-2, y0+MSC/2-3, a); }
        } else {
            for(int j=2;j<MSC-1;j++) for(int i=2;i<MSC-1;i++) if((i+j)&1) mpx(x0+i,y0+j);
            if(cb & MS_FLAG) mfill(x0+4,y0+4,MSC-8,MSC-8);           /* flag = solid block */
            if(g_ms.state==MS_LOST && mine) mdisc(x0+MSC/2, y0+MSC/2, 4);
        }
    }
    if(g_ms_status){
        if(g_ms.state==MS_WON)      lv_label_set_text(g_ms_status, "You win!");
        else if(g_ms.state==MS_LOST)lv_label_set_text(g_ms_status, "Boom! tap New");
        else lv_label_set_text_fmt(g_ms_status, "%d mines - %d flags", g_ms.mines, ms_flags(&g_ms));
    }
    if(g_ms_timelbl){
        char tb[8], bb[8];
        ms_fmt_mmss(ms_elapsed(), tb, sizeof tb);
        if(g_ms_best){ ms_fmt_mmss(g_ms_best, bb, sizeof bb);
                       lv_label_set_text_fmt(g_ms_timelbl, "Time %s   Best %s", tb, bb); }
        else           lv_label_set_text_fmt(g_ms_timelbl, "Time %s   Best --", tb);
    }
}
static void ms_render(void){
    if(!g_ms_cv) return;
    ms_render_body();
    lv_obj_invalidate(g_ms_cv);          /* one, not MSCW*MSCH */
}
/* 1 Hz tick (created once in ui_init): keep the on-screen clock live while playing. */
static void ms_tick(lv_timer_t *t){ (void)t;
    if(g_ms_active && g_ms_timelbl && g_ms.state==MS_PLAY && g_ms_clk.run){
        char tb[8]; ms_fmt_mmss(ms_elapsed(), tb, sizeof tb);
        char bb[8];
        if(g_ms_best){ ms_fmt_mmss(g_ms_best, bb, sizeof bb);
                       lv_label_set_text_fmt(g_ms_timelbl, "Time %s   Best %s", tb, bb); }
        else           lv_label_set_text_fmt(g_ms_timelbl, "Time %s   Best --", tb);
    }
}
static void ms_new_game(void){
    time_t t=0; time(&t);
    ms_new(&g_ms, MSW, MSH, MSMINES, (uint32_t)t ^ (g_ms_seq++ * 2654435761u));
    g_ms_flag = 0;
    pc_reset(&g_ms_clk);                          /* timer starts on the first dig; best is kept */
}

/* Persist the in-progress board so it survives leaving the app. MsGame is plain
 * POD (no pointers), so a magic-tagged blob of the whole struct is enough; the
 * magic + size + w/h guard against a stale/foreign file. Saved after each move
 * and on New; restored when the screen reopens. */
#define MS_SAV       "/sdcard/mines.sav"
#define MS_SAV_MAGIC 0x4D534733u                 /* "MSG3" (bumped: pausable PlayClock) */
static void ms_save(void){
    FILE *f = fopen(MS_SAV, "wb"); if(!f) return;
    uint32_t magic = MS_SAV_MAGIC;
    /* store a PAUSED snapshot: a reboot must never charge for time powered off */
    PlayClock clk = pc_snapshot(&g_ms_clk, (uint32_t)time(NULL));
    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&g_ms, sizeof g_ms, 1, f);
    fwrite(&clk,  sizeof clk,  1, f);
    fwrite(&g_ms_best, sizeof g_ms_best, 1, f);
    fclose(f);
}
static int ms_load(void){
    FILE *f = fopen(MS_SAV, "rb"); if(!f) return 0;
    uint32_t magic = 0; MsGame tmp; int ok = 0;
    if(fread(&magic, sizeof magic, 1, f) == 1 && magic == MS_SAV_MAGIC &&
       fread(&tmp, sizeof tmp, 1, f) == 1 && tmp.w == MSW && tmp.h == MSH){
        g_ms = tmp; ok = 1;
        /* clock + best follow the board; tolerate a truncated (older) file. The
         * stored clock is always paused -- show_minesweeper() resumes it. */
        if(fread(&g_ms_clk,  sizeof g_ms_clk,  1, f) != 1) pc_reset(&g_ms_clk);
        if(fread(&g_ms_best, sizeof g_ms_best, 1, f) != 1) g_ms_best = 0;
        g_ms_clk.run = 0;
    }
    fclose(f);
    return ok;
}
static void ms_tap_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a; lv_obj_get_coords(g_ms_cv, &a);
    int lx=p.x-a.x1, ly=p.y-a.y1;
    if(lx<0||ly<0||lx>=MSCW||ly>=MSCH) return;
    int c=lx/MSC, r=ly/MSC;
    int wasfirst = g_ms.first;
    if(g_ms_flag) ms_flag(&g_ms,r,c); else ms_reveal(&g_ms,r,c);
    uint32_t now = (uint32_t)time(NULL);
    if(wasfirst && !g_ms.first) pc_start(&g_ms_clk, now);        /* first dig -> start clock */
    if(g_ms.state != MS_PLAY && !g_ms_clk.done){                 /* game just ended -> freeze */
        pc_stop(&g_ms_clk, now);
        if(g_ms.state == MS_WON){
            uint32_t el = ms_elapsed();
            if(el && (g_ms_best == 0 || el < g_ms_best)) g_ms_best = el;              /* new high score */
        }
    }
    ms_render();
    ms_save();
}
static void ms_mode_cb(lv_event_t *e){ (void)e;
    g_ms_flag = !g_ms_flag;
    if(g_ms_modelbl) lv_label_set_text(g_ms_modelbl, g_ms_flag ? "Flag" : "Dig");
}
static void ms_newbtn_cb(lv_event_t *e){ (void)e;
    ms_new_game();
    if(g_ms_modelbl) lv_label_set_text(g_ms_modelbl, "Dig");
    ms_render();
    ms_save();
}
static void show_minesweeper(void){
    kill_kb(); cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Mines");
    update_cat_trigger();

    g_ms_status = lv_label_create(content);
    lv_obj_align(g_ms_status, LV_ALIGN_TOP_LEFT, 6, 5);

    g_ms_timelbl = lv_label_create(content);     /* live timer + best time, below the board */
    lv_obj_set_style_text_font(g_ms_timelbl, &lv_font_palm, 0);
    lv_obj_align(g_ms_timelbl, LV_ALIGN_BOTTOM_LEFT, 6, -1);

    lv_obj_t *mode = lv_button_create(content);
    lv_obj_set_style_radius(mode, 0, 0);
    lv_obj_set_style_pad_all(mode, 3, 0);
    lv_obj_align(mode, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_add_event_cb(mode, ms_mode_cb, LV_EVENT_CLICKED, NULL);
    g_ms_modelbl = lv_label_create(mode);
    lv_label_set_text(g_ms_modelbl, "Dig");

    lv_obj_t *nb = lv_button_create(content);
    lv_obj_set_style_radius(nb, 0, 0);
    lv_obj_set_style_pad_all(nb, 3, 0);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -52, 2);
    lv_obj_add_event_cb(nb, ms_newbtn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nbl = lv_label_create(nb); lv_label_set_text(nbl, "New");

    g_ms_cv = lv_canvas_create(content);
    lv_canvas_set_buffer(g_ms_cv, game_cv_buf, MSCW, MSCH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_ms_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_ms_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_ms_cv, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_flag(g_ms_cv, LV_OBJ_FLAG_CLICKABLE);
    /* PRESSED (not CLICKED) + no self-scroll: on the resistive panel a tap jitters
     * a pixel or two, and CLICKED is suppressed when that jitter looks like a scroll
     * -- so taps silently did nothing on the device. Fire on press-down, like News. */
    lv_obj_clear_flag(g_ms_cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_ms_cv, ms_tap_cb, LV_EVENT_PRESSED, NULL);

    if(!ms_load()) ms_new_game();
    g_ms_flag = 0;                              /* restored board opens in Dig mode */
    pc_resume(&g_ms_clk, (uint32_t)time(NULL)); /* the clock only runs while you are here */
    g_ms_active = 1;                            /* let ms_tick update the clock (cleared in kill_kb) */
    ms_render();
}

/* ---- draw real system-font glyphs onto a 1-bpp canvas -------------------------
 * The games render everything onto ONE I1 canvas (dozens of grid cells + keys as
 * separate widgets would exhaust the 24 KB LVGL object pool). lv_font_palm is a
 * 1-bpp font, so we can copy its glyph bitmaps pixel-for-pixel onto the canvas --
 * the SAME shapes the rest of the UI's labels use, with no widget and no draw
 * layer. `v` is the palette index to paint (1 = ink/black, 0 = knockout/white).
 * These mirror LVGL's own fmt_txt 1-bpp unpacking (bits are MSB-first, packed
 * continuously across rows). */
static void canvas_glyph_at(lv_obj_t *cv, const lv_font_t *font, uint32_t uni, int x, int y, int v){
    lv_font_glyph_dsc_t g;
    if(!lv_font_get_glyph_dsc(font, &g, uni, 0)) return;
    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *)g.resolved_font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t *gd = &fdsc->glyph_dsc[g.gid.index];
    const uint8_t *bits = &fdsc->glyph_bitmap[gd->bitmap_index];
    lv_draw_buf_t *db = cv ? lv_canvas_get_draw_buf(cv) : NULL;   /* fetched once */
    int bw = g.box_w, bh = g.box_h, i = 0;
    for(int gy=0; gy<bh; gy++) for(int gx=0; gx<bw; gx++, i++)
        if(bits[i>>3] & (0x80 >> (i&7)))
            i1_px(db, x+gx, y+gy, v ? 1 : 0);
}
/* glyph centred on point (cx,cy) -- for a single char in a cell/key. */
static void canvas_glyph_c(lv_obj_t *cv, const lv_font_t *font, uint32_t uni, int cx, int cy, int v){
    lv_font_glyph_dsc_t g;
    if(!lv_font_get_glyph_dsc(font, &g, uni, 0)) return;
    canvas_glyph_at(cv, font, uni, cx - g.box_w/2, cy - g.box_h/2, v);
}
/* pixel width of a string in `font` (sum of advances). NB: LVGL 9's public
 * lv_font_glyph_dsc_t.adv_w is already in whole pixels (the fmt_txt driver rounds
 * the internal 1/16-px value down before returning it) -- an extra >>4 here is what
 * stacked the OK/DEL captions on top of each other. */
static int canvas_text_w(const lv_font_t *font, const char *s){
    int w = 0;
    for(; *s; s++){ lv_font_glyph_dsc_t g;
        if(lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)*s, 0)) w += g.adv_w; }
    return w;
}
/* left-aligned string at (x,y-top); returns width drawn. */
static int canvas_text(lv_obj_t *cv, const lv_font_t *font, const char *s, int x, int y, int v){
    int x0 = x;
    for(; *s; s++){ lv_font_glyph_dsc_t g;
        if(!lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)*s, 0)) continue;
        canvas_glyph_at(cv, font, (uint32_t)(uint8_t)*s, x, y, v);
        x += g.adv_w;
    }
    return x - x0;
}

/* ========================= Wordie (Games) ==================================
 * A five-letter, six-guess word game (wordie.c holds the pure logic). The guess
 * grid AND an on-screen QWERTY keyboard are drawn mono on ONE 1-bpp canvas, so it
 * stays pool-cheap (one canvas + a couple of labels/buttons -- no per-cell widgets
 * that would blow the 24 KB object pool). Taps hit-test into keys; the physical
 * Graffiti strip also types letters. Mono state language, applied to the grid
 * cells and the keys (explained by the on-screen legend under the grid):
 *     CORRECT  -> solid black tile, knockout (white) letter   ("right spot")
 *     PRESENT  -> letter + a filled corner tab                ("in the word")
 *     ABSENT   -> letter + a diagonal slash                   ("not in it")
 *     typed    -> single border, black letter   (empty -> single border only)   */

/* Clean 5x6 uppercase font (cap-height 6, bit4 = leftmost of 5), rendered by
 * wd_glyph at scale 2 (10x12 px). Shorter than the old 5x7 so the letter clears
 * the tile edges with a margin instead of touching the bottom. */
static const uint8_t WD_FONT[26][6] = {
  {14,17,17,31,17,17},{30,17,30,17,17,30},{15,16,16,16,16,15},{30,17,17,17,17,30},{31,16,30,16,16,31},{31,16,30,16,16,16}, /* A-F */
  {15,16,23,17,17,14},{17,17,31,17,17,17},{31,4,4,4,4,31},{7,2,2,2,18,12},{17,18,28,18,17,17},{16,16,16,16,16,31},       /* G-L */
  {17,27,21,17,17,17},{17,25,21,19,17,17},{14,17,17,17,17,14},{30,17,30,16,16,16},{14,17,17,21,18,13},{30,17,30,20,18,17}, /* M-R */
  {15,16,14,1,1,30},{31,4,4,4,4,4},{17,17,17,17,17,14},{17,17,17,17,10,4},{17,17,17,21,21,10},{17,10,4,4,10,17},           /* S-X */
  {17,10,4,4,4,4},{31,2,4,8,16,31},                                                                                      /* Y-Z */
};

#define WDCW   240                          /* canvas size */
#define WDCH   160
#define WD_CW  22                           /* grid cell w/h */
#define WD_CH  16                           /* tile height: fits the 14px Palm glyph with margin */
#define WD_GX0 65                           /* grid origin ((240-5*22)/2) */
#define WD_GY0 0
#define WD_LEGY 98                          /* legend strip (below the 6x16 grid) */
#define WD_KY0 112                          /* keyboard top; 3 rows of WD_KEYH */
#define WD_KEYH 16
/* OK (submit) and DEL (backspace) live in the empty margins beside the guess grid
 * (grid spans x 65..175), not crammed into the bottom key row where their captions
 * used to collide with the Z..M letters. One on each side, vertically centred on
 * the grid. */
#define WD_BTNW 52
#define WD_BTNH 24
#define WD_BTNY 34
#define WD_OKX  6
#define WD_DELX (WDCW - WD_BTNW - 6)        /* = 182, mirrored on the right */

static WdGame    g_wd;
static lv_obj_t *g_wd_cv, *g_wd_status;
static uint32_t  g_wd_seq;
static uint32_t  g_wd_streak;                /* consecutive solves (persisted) */
static const char *WD_KROW[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };

/* Persist the in-progress puzzle so it survives leaving the app. WdGame is plain
 * POD; a magic-tagged blob of the struct is enough (magic + size guard a stale/
 * foreign file). Saved after each keystroke/submit and on New; restored on open,
 * so a half-finished guess grid is exactly where you left it. */
#define WD_SAV       "/sdcard/wordie.sav"
#define WD_SAV_MAGIC 0x57444732u                 /* "WDG2" (bumped: now carries the streak) */
static void wd_save(void){
    FILE *f = fopen(WD_SAV, "wb"); if(!f) return;
    uint32_t magic = WD_SAV_MAGIC;
    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&g_wd, sizeof g_wd, 1, f);
    fwrite(&g_wd_streak, sizeof g_wd_streak, 1, f);
    fclose(f);
}
static int wd_load(void){
    FILE *f = fopen(WD_SAV, "rb"); if(!f) return 0;
    uint32_t magic = 0; WdGame tmp; int ok = 0;
    if(fread(&magic, sizeof magic, 1, f) == 1 && magic == WD_SAV_MAGIC &&
       fread(&tmp, sizeof tmp, 1, f) == 1 &&
       tmp.answer[0] >= 'A' && tmp.answer[0] <= 'Z' && tmp.nrows <= WD_ROWS){
        g_wd = tmp; ok = 1;
        if(fread(&g_wd_streak, sizeof g_wd_streak, 1, f) != 1) g_wd_streak = 0;
    }
    fclose(f);
    return ok;
}

static void wdpx(int x,int y,int v){
    i1_obj_px(g_wd_cv, x, y, v);
}
static void wd_glyph(int x,int y,char ch,int s,int v){       /* ch at (x,y), scale s; 5x6 */
    if(ch<'A' || ch>'Z') return;
    const uint8_t *g = WD_FONT[ch-'A'];
    for(int r=0;r<6;r++) for(int c=0;c<5;c++) if(g[r] & (16>>c))
        for(int dy=0;dy<s;dy++) for(int dx=0;dx<s;dx++) wdpx(x+c*s+dx, y+r*s+dy, v);
}
static void wd_rect(int x,int y,int w,int h,int v){
    for(int i=0;i<w;i++){ wdpx(x+i,y,v); wdpx(x+i,y+h-1,v); }
    for(int j=0;j<h;j++){ wdpx(x,y+j,v); wdpx(x+w-1,y+j,v); }
}
static void wd_fill(int x,int y,int w,int h,int v){ for(int j=0;j<h;j++) for(int i=0;i<w;i++) wdpx(x+i,y+j,v); }
static void wd_slash(int x,int y,int w,int h){               /* one corner-to-corner diagonal */
    int steps = (w>h?w:h); if(steps<2) return;
    for(int i=0;i<steps;i++) wdpx(x + i*(w-1)/(steps-1), y + i*(h-1)/(steps-1), 1);
}
/* a solid right-triangle tab in the top-right corner -- the PRESENT marker (letter
 * is in the word, wrong spot). Clearly distinct from CORRECT (whole tile filled)
 * and ABSENT (slash), and it never collides with the centred letter. */
static void wd_tab(int x,int y,int w,int sz){
    for(int j=0;j<sz;j++) for(int i=0;i<=j;i++) wdpx(x+w-1-i, y+1+j, 1);
}
/* a letter is drawn in the real Palm system font (canvas_glyph_c), nudged down 1px
 * so the caps sit optically centred in the tile. */
static void wd_letter(int cx,int cy,char ch,int v){ canvas_glyph_c(g_wd_cv, &lv_font_palm, (uint32_t)(uint8_t)ch, cx, cy+1, v); }
/* one grid cell / key tile: state = WD_ABSENT/PRESENT/CORRECT, or -1 empty, -2 typed. */
static void wd_tile(int x,int y,int w,int h,char ch,int state){
    if(state == WD_CORRECT){
        wd_fill(x, y, w, h, 1);
        if(ch) wd_letter(x+w/2, y+h/2, ch, 0);              /* knockout (white) letter */
        return;
    }
    wd_rect(x, y, w, h, 1);
    if(ch) wd_letter(x+w/2, y+h/2, ch, 1);
    if(state == WD_PRESENT) wd_tab(x, y, w, 6);             /* corner tab = in the word */
    if(state == WD_ABSENT)  wd_slash(x+1, y+1, w-2, h-2);   /* slash = not in the word */
}
static void wd_key(int x,int y,int w,const char *label,char ch,int keystate){
    int kh = WD_KEYH - 2;
    if(keystate == WK_CORRECT){
        wd_fill(x, y, w, kh, 1);
        if(ch) wd_letter(x+w/2, y+kh/2, ch, 0);
        return;
    }
    wd_rect(x, y, w, kh, 1);
    if(ch) wd_letter(x+w/2, y+kh/2, ch, 1);
    else if(label)                                          /* OK / DEL caption, Palm font */
        canvas_text(g_wd_cv, &lv_font_palm, label, x + (w - canvas_text_w(&lv_font_palm, label))/2, y + (kh-14)/2, 1);
    if(keystate == WK_PRESENT) wd_tab(x, y, w, 5);
    if(keystate == WK_ABSENT)  wd_slash(x, y, w, kh);
}
/* a plain bordered command button (OK / DEL) with a vertically-centred caption,
 * drawn in the grid margin. Wider than a key so the whole word clears the border. */
static void wd_button(int x,int y,int w,int h,const char *label){
    wd_rect(x, y, w, h, 1);
    canvas_text(g_wd_cv, &lv_font_palm, label,
                x + (w - canvas_text_w(&lv_font_palm, label))/2, y + (h-14)/2, 1);
}
/* the legend under the grid: a mini sample of each mark + a short caption, so the
 * three tile states are self-explanatory without a separate help screen. */
static void wd_legend(void){
    struct { int x; int st; const char *cap; } it[3] = {
        { 8,   WD_CORRECT, "SPOT" },   /* right letter, right spot */
        { 92,  WD_PRESENT, "WORD" },   /* right letter, wrong spot */
        { 176, WD_ABSENT,  "NONE" },   /* not in the word          */
    };
    for(int k=0;k<3;k++){
        int x = it[k].x, y = WD_LEGY;
        wd_tile(x, y, 12, 12, 0, it[k].st);                 /* blank sample tile */
        int tx = x + 16;
        for(const char *p=it[k].cap; *p; p++){ wd_glyph(tx, y+3, *p, 1, 1); tx += 6; }
    }
}
static void wd_render_body(void){
    i1_obj_clear(g_wd_cv);
    for(int r=0;r<WD_ROWS;r++) for(int c=0;c<WD_LEN;c++){
        int x = WD_GX0 + c*WD_CW, y = WD_GY0 + r*WD_CH;
        char ch = 0; int state = -1;
        if(r < g_wd.nrows){ ch = g_wd.guess[r][c]; state = g_wd.mark[r][c]; }
        else if(r == g_wd.nrows && c < g_wd.cur){ ch = g_wd.row[c]; state = -2; }
        wd_tile(x, y, WD_CW, WD_CH, ch, state);
    }
    int y0 = WD_KY0;
    for(int i=0;i<10;i++){ char ch=WD_KROW[0][i]; wd_key(i*24,      y0,           24, NULL, ch, g_wd.key[ch-'A']); }
    for(int i=0;i<9;i++){  char ch=WD_KROW[1][i]; wd_key(12+i*24,   y0+WD_KEYH,   24, NULL, ch, g_wd.key[ch-'A']); }
    wd_legend();
    int y2 = y0 + 2*WD_KEYH;
    for(int i=0;i<7;i++){ char ch=WD_KROW[2][i]; wd_key(36+i*24,    y2,           24, NULL, ch, g_wd.key[ch-'A']); }
    /* OK / DEL beside the grid (see WD_BTN* -- keeps them off the letter row) */
    wd_button(WD_OKX,  WD_BTNY, WD_BTNW, WD_BTNH, "OK");
    wd_button(WD_DELX, WD_BTNY, WD_BTNW, WD_BTNH, "DEL");

    if(g_wd_status){
        char st[24] = "";
        if(g_wd_streak) snprintf(st, sizeof st, "  Streak %u", (unsigned)g_wd_streak);
        if(g_wd.state == WD_WON)       lv_label_set_text_fmt(g_wd_status, "Solved in %d!%s", g_wd.nrows, st);
        else if(g_wd.state == WD_LOST) lv_label_set_text_fmt(g_wd_status, "Answer: %s", g_wd.answer);
        else                           lv_label_set_text_fmt(g_wd_status, "Guess %d/%d%s", g_wd.nrows+1, WD_ROWS, st);
    }
}
static void wd_render(void){
    if(!g_wd_cv) return;
    wd_render_body();
    lv_obj_invalidate(g_wd_cv);          /* one, not WDCW*WDCH */
}
/* submit the current row and fold the result into the win streak: a solve bumps
 * it, a loss resets it. Only wd_enter can end a game, so this is the one hook. */
static void wd_do_enter(void){
    int prev = g_wd.state;
    if(wd_enter(&g_wd)){
        if(prev == WD_PLAY && g_wd.state == WD_WON)  g_wd_streak++;
        else if(prev == WD_PLAY && g_wd.state == WD_LOST) g_wd_streak = 0;
    }
}
static int wd_in(int lx,int ly,int x,int y,int w,int h){ return lx>=x && lx<x+w && ly>=y && ly<y+h; }
static void wd_key_tap(int lx,int ly){
    if(wd_in(lx,ly, WD_OKX,  WD_BTNY, WD_BTNW, WD_BTNH))      wd_do_enter();  /* OK (submit) */
    else if(wd_in(lx,ly, WD_DELX, WD_BTNY, WD_BTNW, WD_BTNH)) wd_del(&g_wd);  /* DEL (backspace) */
    else if(ly < WD_KY0) return;                             /* rest of the grid area: ignore */
    else {
        int row = (ly - WD_KY0) / WD_KEYH;
        if(row == 0){ int i = lx/24; if(i>=0 && i<10) wd_addch(&g_wd, WD_KROW[0][i]); }
        else if(row == 1){ if(lx<12) return; int i=(lx-12)/24; if(i>=0 && i<9) wd_addch(&g_wd, WD_KROW[1][i]); }
        else if(row == 2){ if(lx<36) return; int i=(lx-36)/24; if(i>=0 && i<7) wd_addch(&g_wd, WD_KROW[2][i]); }
        else return;
    }
    wd_render();
    wd_save();
}
static void wd_tap_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a; lv_obj_get_coords(g_wd_cv, &a);
    wd_key_tap(p.x - a.x1, p.y - a.y1);
}
/* the Graffiti strip also drives Wordie: a letter types, backspace deletes, the
 * newline gesture submits (set as graf_char_hook while the screen is open). */
static void wordie_input(char c){
    if(c == '\b')      wd_del(&g_wd);
    else if(c == '\n') wd_do_enter();
    else if((c>='a'&&c<='z')||(c>='A'&&c<='Z')) wd_addch(&g_wd, c);
    else return;
    wd_render();
    wd_save();
}
static void wd_new_daily(void){
    time_t t = 0; time(&t);
    wd_daily(&g_wd, (long)(t / 86400L));                     /* same date -> same word */
}
static void wd_newbtn_cb(lv_event_t *e){ (void)e;
    wd_random(&g_wd, (uint32_t)time(NULL) ^ (g_wd_seq++ * 2654435761u));
    wd_render();
    wd_save();
}
static void show_wordie(void){
    kill_kb(); cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Wordie");
    update_cat_trigger();

    g_wd_status = lv_label_create(content);
    lv_obj_set_style_text_font(g_wd_status, &lv_font_palm, 0);
    lv_obj_align(g_wd_status, LV_ALIGN_TOP_LEFT, 6, 6);

    lv_obj_t *nb = lv_button_create(content);
    lv_obj_set_style_radius(nb, 0, 0);
    lv_obj_set_style_pad_all(nb, 3, 0);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_add_event_cb(nb, wd_newbtn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(nb); lv_label_set_text(nl, "New");

    g_wd_cv = lv_canvas_create(content);
    lv_canvas_set_buffer(g_wd_cv, game_cv_buf, WDCW, WDCH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_wd_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_wd_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_wd_cv, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(g_wd_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_wd_cv, LV_OBJ_FLAG_SCROLLABLE);      /* resistive-robust (see News/Mines) */
    lv_obj_add_event_cb(g_wd_cv, wd_tap_cb, LV_EVENT_PRESSED, NULL);

    if(!wd_load()) wd_new_daily();
    wd_render();
    graf_char_hook = wordie_input;         /* AFTER kill_kb cleared it: route strokes here */
}

/* ========================= Sudoku (Games) ==================================
 * A 9x9 Sudoku (sudoku.c holds the pure generator + rules). The board AND a
 * number pad are drawn mono on ONE 1-bpp canvas (pool-cheap, like Wordie). The
 * on-brand input is the Graffiti digit strip -- draw 1-9 to fill the selected
 * cell, backspace to clear -- and the number pad gives the same by tap. Clue
 * cells carry a filled corner tab and can't be edited; a rule-breaking entry gets
 * a slash; the selected cell gets a thick border. */
#define SDCW    240
#define SDCH    164
#define SD_CELL 16                          /* grid cell px -> 9*16 = 144 */
#define SD_GX0  ((240 - 9*SD_CELL)/2)       /* = 48, centred */
#define SD_GY0  0
#define SD_PY0  146                         /* number pad top */
#define SD_PKW  24                          /* 10 keys * 24 = 240 */
#define SD_PKH  17
#define SD_HOLES 45                         /* ~36 clues: an approachable board */

static SdGame    g_sd;
static lv_obj_t *g_sd_cv, *g_sd_status, *g_sd_timelbl;
static int       g_sd_sel;                  /* selected cell index 0..80, or -1 */
static uint32_t  g_sd_seq;
static PlayClock g_sd_clk;                  /* pausable solve timer (playclock.h) */
static uint32_t  g_sd_best;                 /* best (fastest) solve in seconds (0 = none yet) */

/* elapsed SOLVE seconds: 0 before the first digit, live while the screen is open,
 * paused when you leave it, frozen once solved. A half-finished puzzle you come
 * back to tomorrow resumes where its clock stopped. */
static uint32_t sd_elapsed(void){ return pc_secs(&g_sd_clk, (uint32_t)time(NULL)); }
/* fill the time/best readout (shared format with Mines). */
static void sd_time_text(void){
    if(!g_sd_timelbl) return;
    char tb[8], bb[8];
    ms_fmt_mmss(sd_elapsed(), tb, sizeof tb);
    if(g_sd_best){ ms_fmt_mmss(g_sd_best, bb, sizeof bb);
                   lv_label_set_text_fmt(g_sd_timelbl, "%s  Best %s", tb, bb); }
    else           lv_label_set_text_fmt(g_sd_timelbl, "%s  Best --", tb);
}

static void sdpx(int x,int y,int v){
    i1_obj_px(g_sd_cv, x, y, v);
}
static void sd_hline(int x0,int x1,int y){ for(int x=x0;x<=x1;x++) sdpx(x,y,1); }
static void sd_vline(int x,int y0,int y1){ for(int y=y0;y<=y1;y++) sdpx(x,y,1); }
static void sd_box(int x,int y,int w,int h){
    for(int i=0;i<w;i++){ sdpx(x+i,y,1); sdpx(x+i,y+h-1,1); }
    for(int j=0;j<h;j++){ sdpx(x,y+j,1); sdpx(x+w-1,y+j,1); }
}
static void sd_slash(int x,int y,int w,int h){        /* top-left -> bottom-right */
    int steps = (w>h?w:h); if(steps<2) return;
    for(int i=0;i<steps;i++) sdpx(x + i*(w-1)/(steps-1), y + i*(h-1)/(steps-1), 1);
}
static void sd_ex(int x,int y,int w,int h){           /* an "X" (both diagonals) */
    int steps = (w>h?w:h); if(steps<2) return;
    for(int i=0;i<steps;i++){
        sdpx(x + i*(w-1)/(steps-1),         y + i*(h-1)/(steps-1), 1);
        sdpx(x + (w-1) - i*(w-1)/(steps-1), y + i*(h-1)/(steps-1), 1);
    }
}
static void sd_render_body(void){
    i1_obj_clear(g_sd_cv);

    /* grid: thin cell lines, doubled on the 3x3 box boundaries */
    for(int i=0;i<=9;i++){
        int gx = SD_GX0 + i*SD_CELL, gy = SD_GY0 + i*SD_CELL;
        sd_vline(gx, SD_GY0, SD_GY0 + 9*SD_CELL);
        sd_hline(SD_GX0, SD_GX0 + 9*SD_CELL, gy);
        if(i%3==0){ sd_vline(gx+1, SD_GY0, SD_GY0 + 9*SD_CELL);
                    sd_hline(SD_GX0, SD_GX0 + 9*SD_CELL, gy+1); }
    }
    for(int r=0;r<9;r++) for(int c=0;c<9;c++){
        int x = SD_GX0 + c*SD_CELL, y = SD_GY0 + r*SD_CELL;
        int p = r*9 + c, v = g_sd.cell[p];
        if(g_sd.given[p]) for(int j=0;j<4;j++) for(int i=0;i<=j;i++) sdpx(x+2+i, y+2+j, 1);  /* clue tab */
        if(v){
            canvas_glyph_c(g_sd_cv, &lv_font_palm, (uint32_t)('0'+v), x+SD_CELL/2, y+SD_CELL/2+1, 1);
            if(!g_sd.given[p] && sd_conflict(&g_sd, r, c)) sd_slash(x+3, y+3, SD_CELL-6, SD_CELL-6);
        }
        if(p == g_sd_sel){ sd_box(x+1, y+1, SD_CELL-1, SD_CELL-1);
                           sd_box(x+2, y+2, SD_CELL-3, SD_CELL-3); }
    }
    /* number pad: 1..9 then an erase key (X) */
    for(int k=0;k<10;k++){
        int x = k*SD_PKW, y = SD_PY0;
        sd_box(x, y, SD_PKW, SD_PKH);
        if(k<9) canvas_glyph_c(g_sd_cv, &lv_font_palm, (uint32_t)('1'+k), x+SD_PKW/2, y+SD_PKH/2+1, 1);
        else    sd_ex(x + (SD_PKW-10)/2, y+3, 10, SD_PKH-6);      /* X = erase */
    }
    if(g_sd_status){
        if(g_sd.state==SD_SOLVED) lv_label_set_text(g_sd_status, "Solved!");
        else lv_label_set_text_fmt(g_sd_status, "%d left", sd_remaining(&g_sd));
    }
    sd_time_text();
}
static void sd_render(void){
    if(!g_sd_cv) return;
    sd_render_body();
    lv_obj_invalidate(g_sd_cv);          /* one, not SDCW*SDCH */
}
/* 1 Hz tick (created once in ui_init): keep the solve clock live while playing. */
static void sd_tick(lv_timer_t *t){ (void)t;
    if(g_sd_active && g_sd_timelbl && g_sd.state==SD_PLAY && g_sd_clk.run) sd_time_text();
}

#define SD_SAV       "/sdcard/sudoku.sav"
#define SD_SAV_MAGIC 0x53444B33u                 /* "SDK3" (bumped: pausable PlayClock) */
static void sd_save(void){
    FILE *f = fopen(SD_SAV, "wb"); if(!f) return;
    uint32_t magic = SD_SAV_MAGIC;
    PlayClock clk = pc_snapshot(&g_sd_clk, (uint32_t)time(NULL));   /* paused snapshot */
    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&g_sd, sizeof g_sd, 1, f);
    fwrite(&g_sd_sel, sizeof g_sd_sel, 1, f);
    fwrite(&clk, sizeof clk, 1, f);
    fwrite(&g_sd_best, sizeof g_sd_best, 1, f);
    fclose(f);
}
static int sd_load(void){
    FILE *f = fopen(SD_SAV, "rb"); if(!f) return 0;
    uint32_t magic = 0; SdGame tmp; int ok = 0;
    if(fread(&magic, sizeof magic, 1, f) == 1 && magic == SD_SAV_MAGIC &&
       fread(&tmp, sizeof tmp, 1, f) == 1){
        g_sd = tmp; ok = 1;
        if(fread(&g_sd_sel, sizeof g_sd_sel, 1, f) != 1 || g_sd_sel < 0 || g_sd_sel >= SD_CELLS) g_sd_sel = -1;
        /* clock + best follow the board; tolerate a truncated (older) file. The
         * stored clock is always paused -- show_sudoku() resumes it. */
        if(fread(&g_sd_clk,  sizeof g_sd_clk,  1, f) != 1) pc_reset(&g_sd_clk);
        if(fread(&g_sd_best, sizeof g_sd_best, 1, f) != 1) g_sd_best = 0;
        g_sd_clk.run = 0;
    }
    fclose(f);
    return ok;
}
/* pick the first empty, non-clue cell as the initial selection (so Graffiti works
 * right away); -1 if the board is full. */
static void sd_select_first_blank(void){
    g_sd_sel = -1;
    for(int p=0;p<SD_CELLS;p++) if(!g_sd.given[p] && !g_sd.cell[p]){ g_sd_sel = p; return; }
}
static void sd_new_game(void){
    sd_new(&g_sd, (uint32_t)time(NULL) ^ (g_sd_seq++ * 2654435761u), SD_HOLES);
    sd_select_first_blank();
    pc_reset(&g_sd_clk);                    /* clock starts on the first digit; best is kept */
}
static void sd_place(int val){          /* apply a digit (or 0=clear) to the selection */
    if(g_sd_sel < 0) return;
    int r = g_sd_sel/9, c = g_sd_sel%9;
    if(sd_is_given(&g_sd, r, c)) return;
    if(!sd_set(&g_sd, r, c, val)) return;                 /* no change -> nothing to do */
    uint32_t now = (uint32_t)time(NULL);
    if(val) pc_start(&g_sd_clk, now);                     /* first digit -> start clock */
    if(g_sd.state == SD_SOLVED && !g_sd_clk.done){        /* just solved -> freeze + score */
        pc_stop(&g_sd_clk, now);
        uint32_t el = sd_elapsed();
        if(el && (g_sd_best == 0 || el < g_sd_best)) g_sd_best = el;            /* new best time */
    } else if(g_sd.state == SD_PLAY && g_sd_clk.done){   /* erased a digit after solving
                                                          * -> you are playing again */
        g_sd_clk.done = 0;
        pc_resume(&g_sd_clk, now);
    }
    sd_render();
    sd_save();
}
static void sd_key_tap(int lx,int ly){
    if(ly >= SD_GY0 && ly < SD_GY0 + 9*SD_CELL && lx >= SD_GX0 && lx < SD_GX0 + 9*SD_CELL){
        int c = (lx - SD_GX0)/SD_CELL, r = (ly - SD_GY0)/SD_CELL;     /* select a cell */
        if(r>=0 && r<9 && c>=0 && c<9){ g_sd_sel = r*9 + c; sd_render(); sd_save(); }
        return;
    }
    if(ly >= SD_PY0 && ly < SD_PY0 + SD_PKH){                          /* number pad */
        int k = lx / SD_PKW;
        if(k>=0 && k<9)      sd_place(k+1);
        else if(k==9)        sd_place(0);
    }
}
static void sd_tap_cb(lv_event_t *e){ (void)e;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a; lv_obj_get_coords(g_sd_cv, &a);
    sd_key_tap(p.x - a.x1, p.y - a.y1);
}
/* the Graffiti strip drives Sudoku: a digit fills the selected cell, backspace or
 * '0' clears it (set as graf_char_hook while the screen is open). */
static void sudoku_input(char c){
    if(c>='1' && c<='9') sd_place(c - '0');
    else if(c=='\b' || c=='0') sd_place(0);
}
static void sd_newbtn_cb(lv_event_t *e){ (void)e;
    sd_new_game();
    sd_render();
    sd_save();
}
static void show_sudoku(void){
    kill_kb(); cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Sudoku");
    update_cat_trigger();

    /* Canvas FIRST so the header widgets draw ON TOP of its top edge: the board
     * fills the content height exactly, so a header created after it would have its
     * lower border clipped by the canvas (that was the New button's missing bottom). */
    g_sd_cv = lv_canvas_create(content);
    lv_canvas_set_buffer(g_sd_cv, game_cv_buf, SDCW, SDCH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_sd_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_sd_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_sd_cv, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(g_sd_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_sd_cv, LV_OBJ_FLAG_SCROLLABLE);      /* resistive-robust (see News/Mines) */
    lv_obj_add_event_cb(g_sd_cv, sd_tap_cb, LV_EVENT_PRESSED, NULL);

    g_sd_status = lv_label_create(content);
    lv_obj_set_style_text_font(g_sd_status, &lv_font_palm, 0);
    lv_obj_align(g_sd_status, LV_ALIGN_TOP_LEFT, 6, 4);

    g_sd_timelbl = lv_label_create(content);     /* live solve clock + best, centred in the header */
    lv_obj_set_style_text_font(g_sd_timelbl, &lv_font_palm, 0);
    lv_obj_align(g_sd_timelbl, LV_ALIGN_TOP_MID, 6, 4);

    lv_obj_t *nb = lv_button_create(content);
    lv_obj_set_style_radius(nb, 0, 0);
    lv_obj_set_style_pad_all(nb, 2, 0);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -4, 1);
    lv_obj_add_event_cb(nb, sd_newbtn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(nb); lv_label_set_text(nl, "New");

    if(!sd_load()) sd_new_game();
    if(g_sd_sel < 0) sd_select_first_blank();
    pc_resume(&g_sd_clk, (uint32_t)time(NULL));   /* the clock only runs while you are here */
    g_sd_active = 1;                              /* let sd_tick update the clock (cleared in kill_kb) */
    sd_render();
    graf_char_hook = sudoku_input;         /* AFTER kill_kb cleared it: route strokes here */
}

/* ========================= Zip (Games) =====================================
 * A one-line path puzzle on a 6x6 grid (zip.c holds the pure generator + rules):
 * start on 1, hit the numbers in order, and cover EVERY cell with a single
 * unbroken path. The board plus the Undo/Clear buttons are drawn mono on ONE 1-bpp
 * canvas -- 36 cells as widgets would exhaust the 24 KB object pool.
 *
 * Input is a DRAG, which is what this puzzle wants and what the earlier games
 * taught us to build for: LV_EVENT_PRESSED starts it and LV_EVENT_PRESSING
 * continues it (never CLICKED -- a resistive tap jitters and LVGL then suppresses
 * CLICKED as a scroll), and zp_touch() bridges a two-cell jump when the pen outruns
 * the sampler or cuts a diagonal. Retracing over your own path rewinds it, so a
 * wrong turn needs no button. The Graffiti strip's backspace is Undo.
 *
 * Mono visual language, sharing Sudoku's grammar:
 *     numbers      -> filled disc with a knockout digit (readable through the path)
 *     path         -> a 7 px ink band joining cell centres
 *     where you are-> the double border Sudoku uses for its selected cell
 *     empty cells  -> a dotted grid, so the band is what your eye lands on         */
#define ZPCW     240                        /* canvas w/h (shares game_cv_buf) */
#define ZPCH     164
#define ZP_CELL  24                         /* 6*24 = 144: a comfortable finger target */
#define ZP_GX0   ((ZPCW - ZP_N*ZP_CELL)/2)  /* = 48, centred */
#define ZP_GY0   0
#define ZP_GW    (ZP_N*ZP_CELL)
#define ZP_BAND  7                          /* path thickness (odd, so it centres) */
#define ZP_BY0   147                        /* the Undo/Clear row, below the grid */
#define ZP_BH    17
#define ZP_BW    84
#define ZP_UNDOX 28
#define ZP_CLRX  (ZPCW - ZP_BW - ZP_UNDOX)  /* = 128, mirrored on the right */

static ZpGame    g_zp;
static lv_obj_t *g_zp_cv, *g_zp_status, *g_zp_timelbl;

/* ---- the one place the content area is torn down -------------------------
 * lv_obj_clean(content) frees every widget in the view, but the file keeps
 * module-global handles to a lot of them (canvases, status labels, form
 * fields). Those were only reset on RE-ENTRY to the screen that owned them, so
 * from the moment you navigated away until you came back, each one pointed at
 * freed memory -- a use-after-free waiting for any timer tick or callback that
 * still reached for it. The 1 Hz game ticks are guarded by their *_active
 * flags, which is why this never fired in practice, but the guard is a
 * coincidence of the current call graph rather than a property of the code.
 *
 * Freeing and nulling in one function is what makes it a property: a new
 * screen cannot forget to reset the previous screen's pointers, because no
 * screen resets them any more -- this does. Only pointers that actually live
 * under `content` belong here. Overlays with their own lifetime (g_calc on
 * lv_layer_top, g_lock/g_dash_cv, the Graffiti strip, the title bar) must NOT
 * be nulled here: their objects survive this call. */
static void content_clear(void){
    if(!content) return;
    lv_obj_clean(content);

    /* edit / preferences forms */
    g_form = NULL; active_ta = NULL; edit_cat_lbl = NULL; g_due_lbl = NULL;
    for(int i = 0; i < 12; i++) g_fields[i] = NULL;
    g_nfields = 0;
    /* record + search tables */
    g_listtbl = NULL; g_findtbl = NULL;
    /* HotSync / discovery status lines */
    hs_status = NULL; disc_status = NULL;
    /* Graffiti + Kana trainers */
    tr_guide = tr_prompt = tr_score = tr_feedback = tr_mode_lbl = NULL;
    ka_kana = ka_prompt = ka_answer = ka_typed = ka_feedback = ka_score = NULL;
    ka_strokes_lbl = ka_model = ka_modelbl = NULL;
    /* News reader */
    g_news_hdr = g_news_feed = g_news_title = g_news_body = g_news_hint = NULL;
    /* Preferences brightness row */
    g_pf_bright_btn = NULL;
    /* games */
    g_ms_cv = g_ms_status = g_ms_modelbl = g_ms_timelbl = NULL;
    g_wd_cv = g_wd_status = NULL;
    g_sd_cv = g_sd_status = g_sd_timelbl = NULL;
    g_zp_cv = g_zp_status = g_zp_timelbl = NULL;
    /* Coach. g_co_seal is NOT cleared here: it lives on lv_layer_top(), not in
     * the content area, and must survive a content teardown -- that is exactly
     * what keeps a session sealed while the shell changes underneath it. */
    g_co_cv = g_co_time = g_co_sub = g_co_status = g_co_hold_lbl = NULL;
}
static uint32_t  g_zp_seq;                  /* varies the board each New */
static PlayClock g_zp_clk;                  /* pausable solve timer (playclock.h) */
static uint32_t  g_zp_best;                 /* fastest solve in seconds (0 = none yet) */

static uint32_t zp_elapsed(void){ return pc_secs(&g_zp_clk, (uint32_t)time(NULL)); }
static void zp_time_text(void){
    if(!g_zp_timelbl) return;
    char tb[8], bb[8];
    ms_fmt_mmss(zp_elapsed(), tb, sizeof tb);
    if(g_zp_best){ ms_fmt_mmss(g_zp_best, bb, sizeof bb);
                   lv_label_set_text_fmt(g_zp_timelbl, "%s  Best %s", tb, bb); }
    else           lv_label_set_text_fmt(g_zp_timelbl, "%s  Best --", tb);
}

static void zppx(int x,int y,int v){
    i1_obj_px(g_zp_cv, x, y, v);
}
static void zp_fill(int x,int y,int w,int h,int v){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) zppx(x+i,y+j,v);
}
static void zp_box(int x,int y,int w,int h){
    for(int i=0;i<w;i++){ zppx(x+i,y,1); zppx(x+i,y+h-1,1); }
    for(int j=0;j<h;j++){ zppx(x,y+j,1); zppx(x+w-1,y+j,1); }
}
/* `<= r*r` puts a single pixel at each of the four cardinal extremes, which reads
 * as a spike on a 17 px disc; trimming the threshold by r rounds those rows off. */
static void zp_disc(int cx,int cy,int r,int v){
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++)
        if(dx*dx+dy*dy <= r*r - r) zppx(cx+dx,cy+dy,v);
}
#define ZP_DISC_R (ZP_CELL/2 - 3)
#define ZP_CX(c) (ZP_GX0 + (c)*ZP_CELL + ZP_CELL/2)
#define ZP_CY(r) (ZP_GY0 + (r)*ZP_CELL + ZP_CELL/2)

/* the path band between two orthogonally adjacent cells (or a stub on one cell). */
static void zp_link(int a,int b){
    int x0=ZP_CX(a%ZP_N), y0=ZP_CY(a/ZP_N), x1=ZP_CX(b%ZP_N), y1=ZP_CY(b/ZP_N);
    if(x0>x1){ int t=x0; x0=x1; x1=t; }
    if(y0>y1){ int t=y0; y0=y1; y1=t; }
    zp_fill(x0-ZP_BAND/2, y0-ZP_BAND/2, (x1-x0)+ZP_BAND, (y1-y0)+ZP_BAND, 1);
}
/* a 1- or 2-digit waypoint number, centred in its disc (`ink` picks knockout or
 * black, so the same helper serves a filled disc and the hollow head ring). */
static void zp_numtext(int cx,int cy,int n,int ink){
    char nb[4]; snprintf(nb, sizeof nb, "%d", n);
    lv_font_glyph_dsc_t gd; int bh = 10;
    if(lv_font_get_glyph_dsc(&lv_font_palm, &gd, '0', 0)) bh = gd.box_h;
    int w = canvas_text_w(&lv_font_palm, nb);
    canvas_text(g_zp_cv, &lv_font_palm, nb, cx - w/2, cy - bh/2, ink);
}
static void zp_button(int x,int y,int w,int h,const char *label){
    zp_box(x, y, w, h);
    canvas_text(g_zp_cv, &lv_font_palm, label,
                x + (w - canvas_text_w(&lv_font_palm, label))/2, y + (h-14)/2, 1);
}
static void zp_render_body(void){
    i1_obj_clear(g_zp_cv);

    /* dotted interior grid + a solid outer frame: the cells recede so the path
     * band is what the eye follows (the same trick as Mines' stipple). */
    for(int i=1;i<ZP_N;i++){
        int gx = ZP_GX0 + i*ZP_CELL, gy = ZP_GY0 + i*ZP_CELL;
        for(int y=ZP_GY0;y<=ZP_GY0+ZP_GW;y++) if((y+gx)&1) zppx(gx,y,1);
        for(int x=ZP_GX0;x<=ZP_GX0+ZP_GW;x++) if((x+gy)&1) zppx(x,gy,1);
    }
    zp_box(ZP_GX0, ZP_GY0, ZP_GW+1, ZP_GW+1);

    for(int i=1;i<g_zp.plen;i++) zp_link(g_zp.path[i-1], g_zp.path[i]);

    /* Numbers on top of the band, so they stay readable once the path covers them.
     * The cell the pen is on is called out two ways, because a double border drawn
     * over a solid disc would be black-on-black and vanish: a plain cell gets
     * Sudoku's double border, a numbered one becomes a HOLLOW disc with a black
     * digit. Either way "you are here" is unmistakable. */
    int h = zp_head(&g_zp);
    int mark = (h >= 0 && g_zp.state != ZP_SOLVED) ? h : -1;
    for(int p=0;p<ZP_CELLS;p++){
        if(!g_zp.num[p]) continue;
        int cx = ZP_CX(p%ZP_N), cy = ZP_CY(p/ZP_N);
        zp_disc(cx, cy, ZP_DISC_R, 1);
        if(p == mark){
            zp_disc(cx, cy, ZP_DISC_R - 2, 0);
            zp_numtext(cx, cy, g_zp.num[p], 1);
        } else {
            zp_numtext(cx, cy, g_zp.num[p], 0);
        }
    }
    if(mark >= 0 && !g_zp.num[mark]){
        int x = ZP_GX0 + (mark%ZP_N)*ZP_CELL, y = ZP_GY0 + (mark/ZP_N)*ZP_CELL;
        zp_box(x+1, y+1, ZP_CELL-1, ZP_CELL-1);
        zp_box(x+2, y+2, ZP_CELL-3, ZP_CELL-3);
    }

    zp_button(ZP_UNDOX, ZP_BY0, ZP_BW, ZP_BH, "Undo");
    zp_button(ZP_CLRX,  ZP_BY0, ZP_BW, ZP_BH, "Clear");

    if(g_zp_status){
        if(g_zp.state==ZP_SOLVED) lv_label_set_text(g_zp_status, "Zipped!");
        else lv_label_set_text_fmt(g_zp_status, "%d left", zp_remaining(&g_zp));
    }
    zp_time_text();
}
static void zp_render(void){
    if(!g_zp_cv) return;
    zp_render_body();
    lv_obj_invalidate(g_zp_cv);          /* one, not ZPCW*ZPCH */
}
/* 1 Hz tick (created once in ui_init): keep the solve clock live while playing. */
static void zp_tick(lv_timer_t *t){ (void)t;
    if(g_zp_active && g_zp_timelbl && g_zp.state==ZP_PLAY && g_zp_clk.run) zp_time_text();
}

#define ZP_SAV       "/sdcard/zip.sav"
#define ZP_SAV_MAGIC 0x5A495031u                  /* "ZIP1" */
static void zp_save(void){
    FILE *f = fopen(ZP_SAV, "wb"); if(!f) return;
    uint32_t magic = ZP_SAV_MAGIC;
    PlayClock clk = pc_snapshot(&g_zp_clk, (uint32_t)time(NULL));   /* paused snapshot */
    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&g_zp, sizeof g_zp, 1, f);
    fwrite(&clk, sizeof clk, 1, f);
    fwrite(&g_zp_best, sizeof g_zp_best, 1, f);
    fclose(f);
}
static int zp_load(void){
    FILE *f = fopen(ZP_SAV, "rb"); if(!f) return 0;
    uint32_t magic = 0; ZpGame tmp; int ok = 0;
    /* zp_valid() is the real guard: without it a corrupt blob could carry a path
     * byte of 255 and index the 36-entry on[] mask out of bounds. */
    if(fread(&magic, sizeof magic, 1, f) == 1 && magic == ZP_SAV_MAGIC &&
       fread(&tmp, sizeof tmp, 1, f) == 1 && zp_valid(&tmp)){
        g_zp = tmp; ok = 1;
        if(fread(&g_zp_clk,  sizeof g_zp_clk,  1, f) != 1) pc_reset(&g_zp_clk);
        if(fread(&g_zp_best, sizeof g_zp_best, 1, f) != 1) g_zp_best = 0;
        g_zp_clk.run = 0;                         /* stored paused; resumed on open */
    }
    fclose(f);
    return ok;
}
static void zp_new_game(void){
    zp_new(&g_zp, (uint32_t)time(NULL) ^ (g_zp_seq++ * 2654435761u));
    pc_reset(&g_zp_clk);                          /* fresh clock; best is kept */
}
/* one place for "the path changed": run the clock, score a solve, redraw, persist. */
static void zp_after_move(void){
    uint32_t now = (uint32_t)time(NULL);
    if(g_zp.plen > 1) pc_start(&g_zp_clk, now);   /* first step off the '1' starts it */
    if(g_zp.state == ZP_SOLVED && !g_zp_clk.done){
        pc_stop(&g_zp_clk, now);
        uint32_t el = zp_elapsed();
        if(el && (g_zp_best == 0 || el < g_zp_best)) g_zp_best = el;   /* new best */
    } else if(g_zp.state == ZP_PLAY && g_zp_clk.done){   /* undone after a solve -> the
                                                          * clock runs again */
        g_zp_clk.done = 0;
        pc_resume(&g_zp_clk, now);
    }
    zp_render();
    zp_save();
}
static void zp_tap_cb(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a; lv_obj_get_coords(g_zp_cv, &a);
    int lx = p.x - a.x1, ly = p.y - a.y1;
    if(lx >= ZP_GX0 && lx < ZP_GX0 + ZP_GW && ly >= ZP_GY0 && ly < ZP_GY0 + ZP_GW){
        int c = (lx - ZP_GX0)/ZP_CELL, r = (ly - ZP_GY0)/ZP_CELL;
        if(zp_touch(&g_zp, r*ZP_N + c)) zp_after_move();
        return;
    }
    /* the buttons act once per press, not on every sample of a drag over them */
    if(code != LV_EVENT_PRESSED) return;
    if(ly < ZP_BY0 || ly >= ZP_BY0 + ZP_BH) return;
    if(lx >= ZP_UNDOX && lx < ZP_UNDOX + ZP_BW){ if(zp_undo(&g_zp)) zp_after_move(); }
    else if(lx >= ZP_CLRX && lx < ZP_CLRX + ZP_BW){ zp_clear(&g_zp); zp_after_move(); }
}
/* the Graffiti strip's backspace undoes a step (the clock keeps running: Clear and
 * Undo are part of the same attempt, only New resets it). */
static void zip_input(char c){
    if(c=='\b' && zp_undo(&g_zp)) zp_after_move();
}
static void zp_newbtn_cb(lv_event_t *e){ (void)e;
    zp_new_game();
    zp_render();
    zp_save();
}
static void show_zip(void){
    kill_kb(); cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Zip");
    update_cat_trigger();

    /* Canvas FIRST so the header widgets draw ON TOP of its top edge -- the board
     * fills the content height exactly, and a header created afterwards would have
     * its lower border clipped (that was Sudoku's missing New-button bottom). */
    g_zp_cv = lv_canvas_create(content);
    lv_canvas_set_buffer(g_zp_cv, game_cv_buf, ZPCW, ZPCH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_zp_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_zp_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_zp_cv, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(g_zp_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_zp_cv, LV_OBJ_FLAG_SCROLLABLE);      /* resistive-robust */
    lv_obj_add_event_cb(g_zp_cv, zp_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_zp_cv, zp_tap_cb, LV_EVENT_PRESSING, NULL);   /* drag to draw */

    g_zp_status = lv_label_create(content);
    lv_obj_set_style_text_font(g_zp_status, &lv_font_palm, 0);
    lv_obj_align(g_zp_status, LV_ALIGN_TOP_LEFT, 6, 4);

    g_zp_timelbl = lv_label_create(content);
    lv_obj_set_style_text_font(g_zp_timelbl, &lv_font_palm, 0);
    lv_obj_align(g_zp_timelbl, LV_ALIGN_TOP_MID, 6, 4);

    lv_obj_t *nb = lv_button_create(content);
    lv_obj_set_style_radius(nb, 0, 0);
    lv_obj_set_style_pad_all(nb, 2, 0);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -4, 1);
    lv_obj_add_event_cb(nb, zp_newbtn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(nb); lv_label_set_text(nl, "New");

    if(!zp_load()) zp_new_game();
    pc_resume(&g_zp_clk, (uint32_t)time(NULL));   /* the clock only runs while you are here */
    g_zp_active = 1;
    zp_render();
    graf_char_hook = zip_input;            /* AFTER kill_kb cleared it: route strokes here */
}

/* Bank + persist the play clock of whichever game screen is closing (see the
 * forward declaration next to kill_kb). Guarded on the screen's live flag so a
 * game that was never opened this session never touches the SD card. */
static void games_pause_clocks(void){
    uint32_t now = (uint32_t)time(NULL);
    if(g_ms_active && g_ms_clk.run){ pc_pause(&g_ms_clk, now); ms_save(); }
    if(g_sd_active && g_sd_clk.run){ pc_pause(&g_sd_clk, now); sd_save(); }
    if(g_zp_active && g_zp_clk.run){ pc_pause(&g_zp_clk, now); zp_save(); }
}

/* The Games "folder": an icon grid mirroring the app launcher (each game is a
 * tappable icon + label), so it reads as a sub-folder of the main launcher rather
 * than a list of text buttons. Add a game by extending GAMES[] + its dispatch. */
static const char           *GAMES[]      = { "Mines", "Wordie", "Sudoku", "Zip" };
static const lv_image_dsc_t *GAME_ICONS[] = { &icon_mines, &icon_wordie, &icon_sudoku, &icon_zip };
#define NGAMES ((int)(sizeof(GAMES)/sizeof(GAMES[0])))

static void games_pick_cb(lv_event_t *e){
    const char *g = lv_event_get_user_data(e);
    if(!strcmp(g,"Mines"))       show_minesweeper();
    else if(!strcmp(g,"Wordie")) show_wordie();
    else if(!strcmp(g,"Sudoku")) show_sudoku();
    else if(!strcmp(g,"Zip"))    show_zip();
}
static void show_games(void){
    kill_kb(); cur_app=NULL; cur_uid=0;
    content_clear();
    lv_label_set_text(title_lbl, "Games");
    update_cat_trigger();

    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_bg_color(grid, COL_BODY, 0);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    for(int i=0;i<NGAMES;i++){
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
        lv_obj_add_event_cb(cell, games_pick_cb, LV_EVENT_CLICKED, (void *)GAMES[i]);

        lv_obj_t *img = lv_image_create(cell);
        lv_image_set_src(img, GAME_ICONS[i]);
        lv_obj_set_style_image_recolor(img, COL_LINE, 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, GAMES[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_palm, 0);
    }
}

/* ========================= Coach (ritual focus timer) =======================
 * The centrepiece app: a ritual-based Pomodoro. Three taps pick energy / domain /
 * intention, one Graffiti stroke becomes the session's SIGIL, and that mark inks in
 * from the bottom as the session elapses. Afterwards two taps record how it went.
 * Six taps and one stroke, no typing.
 *
 * Three things make it worth building on this device rather than as a phone app:
 *
 *   SEALED MODE. While a session runs, Coach owns the whole 240x320 on
 *   lv_layer_top() -- which covers the silkscreen row too, so Home/Menu/Find/Calc
 *   are physically unreachable (they are underneath, not disabled: nothing to
 *   forget to re-enable). ui_show_lock() no-ops while sealed, so waking the screen
 *   mid-session shows the mark rather than the dashboard. Giving up costs a
 *   five-second press-and-hold and breaks the streak.
 *
 *   THE SIGIL. graffiti_raw_stroke() already exists for the Kana trainer; coach.c
 *   normalises the captured polyline to a 0..100 box (80 bytes). Drawn as a thick
 *   band: solid below the fill line, 50% stippled above it. The stipple is the
 *   whole trick -- "half inked" needs no second buffer and no alpha, just a parity
 *   test in the plot function, which is exactly the Palm mono idiom.
 *
 *   NO NEW BUFFERS. Because sealed mode makes it impossible to open a game while a
 *   session runs, the sigil canvas reuses game_cv_buf outright. Coach's entire
 *   static cost is CoachState + one live CoachSigil.
 *
 * The 1 Hz co_tick() is registered once in ui_init() and runs regardless of which
 * screen is up, so a session ends correctly from anywhere. See
 * docs/COACH_DESIGN.md for the product design and coach.h for the pure logic. */

#define CO_SAV        "/sdcard/coach.sav"
#define CO_LOG        "/sdcard/coach.log"
#define CO_SIGF       "/sdcard/coach.sig"
#define CO_SAV_MAGIC  0x434F4131u        /* "COA1" */
#define CO_LOG_MAGIC  0x434F4C31u        /* "COL1" */

#define CO_CVW  GAME_CVW                 /* the sigil canvas shares game_cv_buf */
#define CO_CVH  GAME_CVH
#define CO_BOX  150                      /* the sigil's drawing box, centred     */
#define CO_BOXX ((CO_CVW - CO_BOX) / 2)
#define CO_BOXY ((CO_CVH - CO_BOX) / 2)
#define CO_BAND 4                        /* half-thickness of the mark's stroke  */
#define CO_PREV_Y   48                   /* the pre-flight preview canvas, which  */
#define CO_PREV_H   96                   /* shares the content area with a prompt */
#define CO_PREV_BOX 88                   /* and two buttons -- so it draws small  */
#define CO_HOLD_MS 5000                  /* press-and-hold to abandon a session  */
#define CO_DIM_PCT 15                    /* brightness while a session runs      */
/* End-of-session flash. The first version rode the 1 Hz tick for six ticks, which
 * came out as two dark blinks at desk brightness -- easy to miss from the other
 * side of the room, which is the one place you need it from. Its own timer, so the
 * phase length is a number rather than an artefact of the tick, and the lit phases
 * are driven to full brightness so the swing is the whole range the panel has. */
#define CO_FLASH_MS 1400                 /* one dark or lit phase                */
#define CO_FLASH_N  10                   /* 5 dark + 5 lit = 14 s of asking      */

/* ---- the local timezone offset, in minutes east of UTC ---------------------
 * coach.c takes this as an argument rather than reading TZ itself (that is what
 * makes the whole engine host-testable). Derived from the difference between
 * localtime and gmtime rather than tm_gmtoff, which is not portable. */
static int co_tz(void){
    time_t t = time(NULL);
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int mins = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
    int dday = lt.tm_yday - gt.tm_yday;
    if(dday == 1 || dday < -1)       mins += 1440;   /* local is a day ahead */
    else if(dday == -1 || dday > 1)  mins -= 1440;   /* local is a day behind */
    return mins;
}

/* ------------------------------------------------------------- persistence */
static void co_save(void){
    FILE *f = fopen(CO_SAV, "wb"); if(!f) return;
    g_co.magic = CO_SAV_MAGIC;
    fwrite(&g_co, sizeof g_co, 1, f);
    fwrite(&g_co_sig, sizeof g_co_sig, 1, f);
    fclose(f);
}

static void co_finish(int result, int blocker);

/* A session that was running when the app closed (or the device died) has to be
 * resolved before anything else can happen. Two cases, and the second is the one
 * the missing RTC forces: the epoch is restored from an NVS checkpoint at boot, so
 * a power cycle can move the clock by the whole off-duration. Anything wildly past
 * the planned length is not a long session, it is a bad clock. */
static void co_recover(void){
    if(g_co.phase != CO_PH_RUNNING && g_co.phase != CO_PH_PAUSED) return;
    uint32_t now     = (uint32_t)time(NULL);
    uint32_t el      = pc_secs(&g_co.clk, now);
    uint32_t planned = (uint32_t)g_co.planned_min * 60;
    if(el >= planned + 3600u){
        co_finish(CO_RES_ABANDONED, CO_BLK_NONE);    /* untrustworthy: drop it */
    } else if(el >= planned){
        g_co.phase = CO_PH_REFLECT;                  /* it ended while we were off */
        co_save();
    }
}

static void co_load(void){
    if(g_co_loaded) return;
    coach_state_init(&g_co);
    memset(&g_co_sig, 0, sizeof g_co_sig);
    FILE *f = fopen(CO_SAV, "rb");
    if(f){
        CoachState t;
        if(fread(&t, sizeof t, 1, f) == 1 && t.magic == CO_SAV_MAGIC){
            g_co = t;
            /* clamp anything a truncated or foreign file could have left absurd */
            if(g_co.pref_min < 5 || g_co.pref_min > 120) g_co.pref_min = 25;
            if(g_co.day_goal < 1 || g_co.day_goal > 99)  g_co.day_goal = 6;
            if(g_co.phase > CO_PH_REFLECT)               g_co.phase = CO_PH_IDLE;
            if(g_co.domain >= CO_NDOM)     g_co.domain = CO_DOM_CAREER;
            if(g_co.energy >= CO_NENERGY)  g_co.energy = CO_ENERGY_MED;
            if(g_co.intent >= CO_NINT)     g_co.intent = CO_INT_FINISH;
            if(fread(&g_co_sig, sizeof g_co_sig, 1, f) != 1)
                memset(&g_co_sig, 0, sizeof g_co_sig);
            if(g_co_sig.n > CO_SIG_MAXPT) memset(&g_co_sig, 0, sizeof g_co_sig);
        }
        fclose(f);
    }
    g_co_loaded = 1;
    co_recover();
}

/* Append one finished session. The record and its mark go to two files that stay
 * index-aligned, so the wall can pair them by position without storing a key. */
static void co_log_append(const CoachRec *r){
    int fresh = 1;
    FILE *t = fopen(CO_LOG, "rb");
    if(t){ fresh = 0; fclose(t); }
    FILE *f = fopen(CO_LOG, "ab");
    if(f){
        if(fresh){ uint32_t m = CO_LOG_MAGIC; fwrite(&m, 4, 1, f); }
        fwrite(r, sizeof *r, 1, f);
        fclose(f);
    }
    f = fopen(CO_SIGF, "ab");
    if(f){ fwrite(&g_co_sig, sizeof g_co_sig, 1, f); fclose(f); }
}

/* Stream the log into an aggregate. Records are read one at a time -- the whole
 * history is never resident, only the ~120-byte fold. `since` is an epoch cutoff
 * (0 = everything). Returns the number of records folded. */
static int co_fold(CoachAgg *a, uint32_t since){
    coach_agg_reset(a);
    FILE *f = fopen(CO_LOG, "rb");
    if(!f) return 0;
    uint32_t m = 0;
    if(fread(&m, 4, 1, f) != 1 || m != CO_LOG_MAGIC){ fclose(f); return 0; }
    int tz = co_tz(), n = 0;
    CoachRec r;
    while(fread(&r, sizeof r, 1, f) == 1){
        if(r.start < since) continue;
        coach_agg_add(a, &r, tz);
        n++;
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------ sigil drawing */
/* Canvas y at and below which the mark is solid; above it the band is stippled.
 * One shared variable rather than a parameter so the Bresenham inner loop stays
 * a plain plot call. */
static int co_fill_y;

/* Where the sigil is drawn, and how fat its stroke is. The sealed screen has a
 * whole display to spend, but the pre-flight preview shares a 184 px content area
 * with a prompt, a Begin button and a back link -- so the box has to shrink to fit
 * the canvas it is clipped to, and the stroke has to shrink with it or the mark
 * turns into a blob. Set by co_seal()/co_show_sigil() before the first paint. */
static int co_bx = CO_BOXX, co_by = CO_BOXY, co_bs = CO_BOX, co_band = CO_BAND;

static void co_plot(lv_draw_buf_t *db, int x, int y){
    if(y >= co_fill_y || ((x ^ y) & 1) == 0) i1_px(db, x, y, 1);
}
/* a filled disc, so the band has round joins and caps instead of notches */
static void co_dot(lv_draw_buf_t *db, int x, int y){
    for(int a = -co_band; a <= co_band; a++)
        for(int b = -co_band; b <= co_band; b++)
            if(a*a + b*b <= co_band*co_band) co_plot(db, x + a, y + b);
}
static void co_line(lv_draw_buf_t *db, int x0, int y0, int x1, int y1){
    int dx = abs(x1-x0), sx = x0<x1?1:-1, dy = -abs(y1-y0), sy = y0<y1?1:-1, err = dx+dy;
    for(;;){
        co_dot(db, x0, y0);
        if(x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if(e2 >= dy){ err += dy; x0 += sx; }
        if(e2 <= dx){ err += dx; y0 += sy; }
    }
}

/* Paint the live sigil with `pct` (0..100) of it inked. A session with no usable
 * mark (a tap, or a stroke with no extent) falls back to a plain square so the
 * screen never goes blank on someone mid-session. */
static void co_paint_sigil(int pct){
    if(!g_co_cv) return;
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(g_co_cv);
    i1_clear(db);
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    co_fill_y = co_by + co_bs - (co_bs * pct / 100);

    if(coach_sigil_ok(&g_co_sig)){
        int n = g_co_sig.n;
        for(int i = 0; i < n - 1; i++){
            int ax = co_bx + g_co_sig.xy[2*i]     * co_bs / 100;
            int ay = co_by + g_co_sig.xy[2*i + 1] * co_bs / 100;
            int bx = co_bx + g_co_sig.xy[2*i + 2] * co_bs / 100;
            int by = co_by + g_co_sig.xy[2*i + 3] * co_bs / 100;
            co_line(db, ax, ay, bx, by);
        }
    } else {
        int m = co_bs * 28 / CO_BOX;                  /* the fallback square */
        for(int y = co_by + m; y < co_by + co_bs - m; y++)
            for(int x = co_bx + m; x < co_bx + co_bs - m; x++)
                co_plot(db, x, y);
    }
    lv_obj_invalidate(g_co_cv);                       /* one, not CO_BOX^2 */
}

/* ------------------------------------------------------------- the sealed screen */
static void co_show_reflect(void);

static uint32_t co_elapsed(void){
    return pc_secs(&g_co.clk, (uint32_t)time(NULL));
}
static int co_pct(void){
    uint32_t planned = (uint32_t)g_co.planned_min * 60;
    if(!planned) return 100;
    uint32_t el = co_elapsed();
    return el >= planned ? 100 : (int)(el * 100 / planned);
}

static void co_unseal(void){
    if(g_co_seal){ lv_obj_del(g_co_seal); g_co_seal = NULL; }
    g_co_cv = g_co_time = g_co_sub = g_co_hold_lbl = NULL;
    power_set_brightness(appcfg()->brightness);       /* the desk comes back up */
}

/* the countdown + the fill, refreshed from co_tick */
static void co_seal_refresh(void){
    if(!g_co_seal) return;
    uint32_t planned = (uint32_t)g_co.planned_min * 60;
    uint32_t el = co_elapsed();
    uint32_t left = el >= planned ? 0 : planned - el;
    if(g_co_time)
        lv_label_set_text_fmt(g_co_time, "%u:%02u",
                              (unsigned)(left / 60), (unsigned)(left % 60));
    /* the mark only moves 1% at a time: repaint when it actually changes, not
     * once a second (25 repaints across a 25-minute session, not 1500). */
    int pct = co_pct();
    if(pct != g_co_last_fill){ g_co_last_fill = pct; co_paint_sigil(pct); }
}

static void co_hold_cb(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED){
        g_co_hold_start = lv_tick_get();
    } else if(code == LV_EVENT_PRESSING){
        if(!g_co_hold_start) return;
        uint32_t held = lv_tick_elaps(g_co_hold_start);
        if(held >= CO_HOLD_MS){
            g_co_hold_start = 0;
            co_unseal();
            co_finish(CO_RES_ABANDONED, CO_BLK_NONE);
            show_coach();
            return;
        }
        if(g_co_hold_lbl)
            lv_label_set_text_fmt(g_co_hold_lbl, "hold %u",
                                  (unsigned)((CO_HOLD_MS - held + 999) / 1000));
    } else {                                    /* RELEASED / PRESS_LOST */
        g_co_hold_start = 0;
        if(g_co_hold_lbl) lv_label_set_text(g_co_hold_lbl, "hold to give up");
    }
}

#ifdef UI_DEVTOOLS
static void co_devfinish_cb(lv_event_t *e){ (void)e;
    /* jump the clock to the planned end; co_tick() does the rest exactly as it
     * would have at T-0, so the tested path is the real one. */
    uint32_t now = (uint32_t)time(NULL);
    g_co.clk.accum = (uint32_t)g_co.planned_min * 60;
    g_co.clk.run   = now;
}
#endif

/* Raise the takeover. On lv_layer_top() at full screen size, so it covers the
 * Graffiti strip and the silkscreen buttons as well as the app area -- that IS
 * sealed mode; there is no separate "disable the buttons" path to get wrong. */
static void co_seal(void){
    if(g_co_seal) return;
    kill_kb();
    content_clear();

    g_co_seal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_co_seal, LCD_W, LCD_H);
    lv_obj_set_pos(g_co_seal, 0, 0);
    lv_obj_set_style_bg_color(g_co_seal, COL_BODY, 0);
    lv_obj_set_style_radius(g_co_seal, 0, 0);
    lv_obj_set_style_border_width(g_co_seal, 0, 0);
    lv_obj_set_style_pad_all(g_co_seal, 0, 0);
    lv_obj_set_style_text_font(g_co_seal, &lv_font_palm, 0);
    lv_obj_clear_flag(g_co_seal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_co_seal, LV_OBJ_FLAG_CLICKABLE);   /* swallow stray taps */

    g_co_time = lv_label_create(g_co_seal);
    lv_obj_set_style_text_font(g_co_time, &lv_font_palm_bold, 0);
    lv_obj_align(g_co_time, LV_ALIGN_TOP_MID, 0, 18);

    g_co_cv = lv_canvas_create(g_co_seal);
    lv_canvas_set_buffer(g_co_cv, game_cv_buf, CO_CVW, CO_CVH, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_co_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_co_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_co_cv, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_clear_flag(g_co_cv, LV_OBJ_FLAG_SCROLLABLE);

    g_co_sub = lv_label_create(g_co_seal);
    lv_obj_align(g_co_sub, LV_ALIGN_TOP_MID, 0, 218);
    lv_label_set_text_fmt(g_co_sub, "%s  %s  %s",
                          coach_domain_name(g_co.domain),
                          coach_intent_name(g_co.intent),
                          coach_energy_name(g_co.energy));

    /* the give-up target: small, low, and deliberately unhurried */
    lv_obj_t *hold = lv_obj_create(g_co_seal);
    lv_obj_set_size(hold, 120, 40);
    lv_obj_align(hold, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(hold, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hold, 0, 0);
    lv_obj_set_style_pad_all(hold, 0, 0);
    lv_obj_add_flag(hold, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hold, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hold, co_hold_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(hold, co_hold_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(hold, co_hold_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(hold, co_hold_cb, LV_EVENT_PRESS_LOST, NULL);
    g_co_hold_lbl = lv_label_create(hold);
    lv_label_set_text(g_co_hold_lbl, "hold to give up");
    lv_obj_center(g_co_hold_lbl);

#ifdef UI_DEVTOOLS
    /* Dev scaffolding: sealed mode is, by design, unreachable from the menu -- which
     * also means CI can never reach the reflect/note screens without waiting out a
     * real 25-minute session. This invisible top-left corner target ends the session
     * immediately. Compiled out of a release build along with "Add test events". */
    {
        lv_obj_t *sk = lv_obj_create(g_co_seal);
        lv_obj_set_size(sk, 26, 26);
        lv_obj_align(sk, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(sk, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sk, 0, 0);
        lv_obj_add_flag(sk, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sk, co_devfinish_cb, LV_EVENT_CLICKED, NULL);
    }
#endif
    co_bx = CO_BOXX; co_by = CO_BOXY; co_bs = CO_BOX; co_band = CO_BAND;
    g_co_last_fill = -1;                       /* force the first paint */
    co_seal_refresh();
    power_set_brightness(CO_DIM_PCT);          /* the desk goes quiet */
}

/* ------------------------------------------------------------ finishing up */
static void co_finish(int result, int blocker){
    uint32_t now     = (uint32_t)time(NULL);
    uint32_t el      = co_elapsed();
    uint32_t planned = (uint32_t)g_co.planned_min * 60;
    if(el > planned) el = planned;             /* never bank more than was asked */

    CoachRec r;
    r.start       = g_co.start ? g_co.start : now;
    r.planned_min = g_co.planned_min;
    r.actual_min  = (uint16_t)((el + 30) / 60);
    r.domain      = g_co.domain;
    r.energy      = g_co.energy;
    r.intent      = g_co.intent;
    r.outcome     = co_pack(result, blocker);
    co_log_append(&r);
    /* the note screen runs AFTER the live session is cleared, so hold on to what
     * the Memo/Date Book records need */
    g_co_last_start = r.start;
    g_co_last_min   = r.actual_min ? r.actual_min : r.planned_min;

    if(result == CO_RES_ABANDONED){
        /* giving up breaks the streak, and does NOT establish the day -- an
         * abandoned session should not be able to hold a streak together. */
        g_co.streak = 0;
        if(g_co.total_n < 0xFFFF) g_co.total_n++;
    } else {
        coach_note_completion(&g_co, r.start, co_tz());
    }
    g_co.phase = CO_PH_IDLE;
    g_co.start = 0;
    pc_reset(&g_co.clk);
    co_save();
}

/* ------------------------------------------------------- end-of-session flash */
static lv_timer_t *g_co_flash;           /* NULL when no flash is running        */
static int         g_co_flash_left;      /* phases still to run                  */

/* 1 while the flash is driving the backlight, so the port layer's idle blank and
 * wake-poll leave it alone -- otherwise a tap during a dark phase reads as a wake
 * and the two fight over the same LEDC duty. */
int ui_owns_backlight(void){ return g_co_flash != NULL; }

static void co_flash_stop(void){
    if(g_co_flash){ lv_timer_delete(g_co_flash); g_co_flash = NULL; }
    g_co_flash_left = 0;
    power_set_brightness(appcfg()->brightness);   /* undo the full-brightness lit phase */
    power_backlight(1);
    /* The idle countdown was frozen for the duration (the port layer stands down
     * while the flash owns the backlight, deliberately without touching the timer
     * -- see idle_step). Restart it here, so someone who walked back to a finished
     * session gets a full backlight_sec to answer rather than an instant blank. */
    lv_display_trigger_activity(NULL);
}

static void co_flash_step(lv_timer_t *t){ (void)t;
    /* The flash is asking for attention; once it has it, it is just strobing at
     * someone who is trying to read "how did it go". Any touch since the last
     * phase ends it. (The session itself ran untouched, so the timer never trips
     * on its own first step.) */
    if(lv_display_get_inactive_time(NULL) < CO_FLASH_MS){ co_flash_stop(); return; }
    if(--g_co_flash_left <= 0){ co_flash_stop(); return; }
    power_backlight(g_co_flash_left & 1);
}

static void co_flash_start(void){
    if(g_co_flash) lv_timer_delete(g_co_flash);
    power_set_brightness(100);           /* the lit phases go to full, not to desk level */
    power_backlight(0);                  /* start dark: the CHANGE is what catches an eye */
    g_co_flash_left = CO_FLASH_N;
    g_co_flash = lv_timer_create(co_flash_step, CO_FLASH_MS, NULL);
}

/* ------------------------------------------------------------- the 1 Hz tick */
/* Registered once in ui_init() and never torn down, so a session ends correctly
 * from any screen -- including the lock screen, a game, or the Calculator. */
static void co_tick(lv_timer_t *t){ (void)t;
    if(g_co.phase != CO_PH_RUNNING) return;
    if(co_elapsed() >= (uint32_t)g_co.planned_min * 60){
        g_co.phase = CO_PH_REFLECT;
        pc_stop(&g_co.clk, (uint32_t)time(NULL));
        co_save();
        co_unseal();                                  /* restores desk brightness */
        co_show_reflect();
        co_flash_start();                             /* ...so the flash goes last */
        return;
    }
    co_seal_refresh();
}

/* ---------------------------------------------------------------- the ritual */
static const char *CO_EN_MAP[]  = { "Low", "Medium", "High", "" };
/* 2 x 3. Family and Relationships are here because the domain is what Coach hands
 * back to you in the weekly report -- leaving them out meant the hours that go into
 * the people in your life scored as nothing at all. `domain` is a whole byte on
 * disk, so appending to this list leaves every existing coach.log readable. */
static const char *CO_DOM_MAP[] = { "Career",   "Health",        "\n",
                                    "Learning", "Creative",      "\n",
                                    "Family",   "Relationships", "" };
static const char *CO_INT_MAP[] = { "Finish", "Explore", "Maintain", "" };
static const char *CO_RES_MAP[] = { "Great", "Okay", "Struggled", "" };
static const char *CO_BLK_MAP[] = { "Tired", "Kids", "\n", "Work", "Distracted", "" };

/* one big tap target grid, styled flat like everything else in the shell */
static lv_obj_t *co_matrix(lv_obj_t *par, const char **map, int y, int h,
                           lv_event_cb_t cb){
    lv_obj_t *bm = lv_buttonmatrix_create(par);
    lv_buttonmatrix_set_map(bm, map);
    lv_obj_set_size(bm, 224, h);
    lv_obj_align(bm, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 2, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_radius(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(bm, COL_LINE, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return bm;
}

static void co_show_ritual(int step);

static void co_ritual_cb(lv_event_t *e){
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    switch(g_co_step){
        case 0: g_co_pick_en  = (int)id; break;
        case 1: g_co_pick_dom = (int)id; break;
        case 2: g_co_pick_int = (int)id; break;
    }
    if(g_co_step < 2) co_show_ritual(g_co_step + 1);
    else              co_show_sigil();
}

static void co_back_cb(lv_event_t *e){ (void)e;
    if(g_co_step > 0) co_show_ritual(g_co_step - 1);
    else              show_coach();
}

static void co_home_cb(lv_event_t *e){ (void)e; show_coach(); }

/* The one "back" affordance every Coach screen uses, bottom-left. The ritual
 * steps walk back one question at a time; Marks and This-week have nowhere to
 * walk back TO, so they return to the app's home screen -- without this they
 * were dead ends, reachable only by leaving Coach through the silkscreen. */
static lv_obj_t *co_link(lv_obj_t *par, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 3, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "back");
    return b;
}
static void co_back_link(lv_obj_t *par){ co_link(par, co_back_cb); }
static void co_home_link(lv_obj_t *par){ co_link(par, co_home_cb); }
/* ...and the same link on a scrolling page, where "the bottom" is wherever the
 * content ends rather than the bottom of the visible frame. */
static void co_home_link_at(lv_obj_t *par, int y){
    lv_obj_align(co_link(par, co_home_cb), LV_ALIGN_TOP_LEFT, 4, y);
}

/* Each step carries a line of plain English under the question. A bare "Energy?"
 * over three buttons tells a first-time user nothing about what is being asked or
 * what it will be used for; these three answers are the whole basis of the weekly
 * report, so it is worth two lines of screen to say so. */
static const char *CO_ASK[3] = {
    "How much have you got right now?",
    "Which part of your life is this for?",
    "What are you here to do with the time?"
};

static void co_show_ritual(int step){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    g_co_open = 1; g_co_step = step;
    lv_label_set_text(title_lbl, "Coach");
    update_cat_trigger();

    lv_obj_t *q = lv_label_create(content);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 2);
    lv_label_set_text(q, step == 0 ? "Energy?" : step == 1 ? "Focus on?" : "Intention?");

    lv_obj_t *n = lv_label_create(content);
    lv_obj_align(n, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_label_set_text_fmt(n, "%d of 3", step + 1);

    lv_obj_t *h = lv_label_create(content);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, 224);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 8, 18);
    lv_label_set_text(h, CO_ASK[step]);

    if(step == 0)      co_matrix(content, CO_EN_MAP,  48,  56, co_ritual_cb);
    else if(step == 1) co_matrix(content, CO_DOM_MAP, 48, 104, co_ritual_cb);
    else               co_matrix(content, CO_INT_MAP, 48,  56, co_ritual_cb);

    co_back_link(content);
}

/* ------------------------------------------------------------------- the sigil */
/* Runs on pen-up BEFORE recognition and consumes the stroke, so the mark is
 * whatever the user drew -- it is never fed to the recognizer and never has to
 * resemble a letter. */
static int co_sigil_capture(void){
    int16_t raw[2 * 64];
    int n = graffiti_raw_stroke(raw, 64);
    if(coach_sigil_from_raw(&g_co_sig, raw, n) > 0){
        if(g_co_cv) co_paint_sigil(100);              /* preview it solid */
        if(g_co_status) lv_label_set_text(g_co_status, "Tap Begin, or draw again.");
    } else if(g_co_status){
        lv_label_set_text(g_co_status, "Too small -- draw a bigger mark.");
    }
    return 1;                                          /* always consume it */
}

static void co_begin_cb(lv_event_t *e){ (void)e;
    uint32_t now = (uint32_t)time(NULL);
    g_co.energy      = (uint8_t)g_co_pick_en;
    g_co.domain      = (uint8_t)g_co_pick_dom;
    g_co.intent      = (uint8_t)g_co_pick_int;
    g_co.planned_min = g_co.pref_min;
    g_co.start       = now;
    g_co.phase       = CO_PH_RUNNING;
    pc_reset(&g_co.clk);
    pc_start(&g_co.clk, now);
    co_save();
    co_seal();
}

static void co_show_sigil(void){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    g_co_open = 1;
    lv_label_set_text(title_lbl, "Coach");
    update_cat_trigger();
    memset(&g_co_sig, 0, sizeof g_co_sig);

    lv_obj_t *q = lv_label_create(content);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_label_set_text(q, "Draw your mark.");

    g_co_status = lv_label_create(content);
    lv_label_set_long_mode(g_co_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_co_status, 224);
    lv_obj_align(g_co_status, LV_ALIGN_TOP_LEFT, 8, 20);
    lv_label_set_text(g_co_status,
        "One stroke in the Graffiti area below -- it inks in as the time runs.");

    /* The canvas is clipped to CO_PREV_H, so the box has to be drawn INSIDE that,
     * not at the sealed screen's 150 px -- otherwise the mark is painted past the
     * bottom edge of its own control and comes back cropped. */
    g_co_cv = lv_canvas_create(content);
    lv_canvas_set_buffer(g_co_cv, game_cv_buf, CO_CVW, CO_PREV_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(g_co_cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(g_co_cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(g_co_cv, LV_ALIGN_TOP_MID, 0, CO_PREV_Y);
    lv_obj_clear_flag(g_co_cv, LV_OBJ_FLAG_SCROLLABLE);
    co_bx   = (CO_CVW - CO_PREV_BOX) / 2;
    co_by   = (CO_PREV_H - CO_PREV_BOX) / 2;
    co_bs   = CO_PREV_BOX;
    co_band = CO_BAND * CO_PREV_BOX / CO_BOX;         /* thin the stroke to match */
    if(co_band < 2) co_band = 2;
    co_paint_sigil(100);

    lv_obj_t *go = lv_button_create(content);
    lv_obj_set_style_radius(go, 0, 0);
    lv_obj_set_size(go, 96, 30);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_add_event_cb(go, co_begin_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(go);
    lv_label_set_text_fmt(gl, "Begin %d min", (int)g_co.pref_min);
    lv_obj_center(gl);

    co_back_link(content);
    graf_capture_hook = co_sigil_capture;   /* AFTER kill_kb cleared it */
}

/* ----------------------------------------------------------------- reflect */
static void co_show_note(void);

static void co_blocker_cb(lv_event_t *e){
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    g_co_blk = (int)id + 1;                  /* map 0..3 -> CO_BLK_TIRED.. */
    co_finish(CO_RES_STRUGGLED, g_co_blk);
    co_show_note();
}

static void co_show_blocker(void){
    kill_kb(); content_clear();
    g_co_open = 1; g_co_reflect = 1;      /* still Coach's screen: no lock over it */
    lv_label_set_text(title_lbl, "Coach");

    lv_obj_t *q = lv_label_create(content);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 10);
    lv_label_set_text(q, "What got in the way?");

    co_matrix(content, CO_BLK_MAP, 46, 84, co_blocker_cb);
}

static void co_result_cb(lv_event_t *e){
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    g_co_res = (int)id;                       /* 0 Great, 1 Okay, 2 Struggled */
    if(g_co_res == CO_RES_STRUGGLED){ co_show_blocker(); return; }
    co_finish(g_co_res, CO_BLK_NONE);
    co_show_note();
}

static void co_show_reflect(void){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    g_co_open = 1; g_co_reflect = 1;      /* still Coach's screen: no lock over it */
    lv_label_set_text(title_lbl, "Coach");
    update_cat_trigger();

    lv_obj_t *d = lv_label_create(content);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_label_set_text_fmt(d, "%d:00 done", (int)g_co.planned_min);

    lv_obj_t *q = lv_label_create(content);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 30);
    lv_label_set_text(q, "How did it go?");

    co_matrix(content, CO_RES_MAP, 66, 46, co_result_cb);
}

/* ------------------------------------------------------------------ the note */
/* The note is not a scratchpad: it becomes a real Memo Pad record, and the
 * session becomes a Date Book block, so both ride the next HotSync to iCloud. */
static void co_write_exports(const char *note){
    char buf[160];
    time_t st = (time_t)(g_co_last_start ? g_co_last_start : (uint32_t)time(NULL));
    struct tm lt; localtime_r(&st, &lt);

    if(note && note[0]){
        snprintf(buf, sizeof buf, "Coach %04d-%02d-%02d  %s  %s\n%s",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 coach_domain_name(g_co.domain), coach_result_name(g_co_res), note);
        data_save_memo(0, 0, buf);
    }
    /* Appt is ~900 bytes; this runs from a button callback, never from ui_init,
     * so it is nowhere near the main task's stack ceiling. */
    Appt a;
    memset(&a, 0, sizeof a);
    a.hasTime = 1;
    a.year = lt.tm_year + 1900; a.month = lt.tm_mon + 1; a.day = lt.tm_mday;
    a.sH = lt.tm_hour; a.sM = lt.tm_min;
    int endm = lt.tm_hour * 60 + lt.tm_min + (int)g_co_last_min;
    a.eH = (endm / 60) % 24; a.eM = endm % 60;
    snprintf(a.description, sizeof a.description, "Focus  %s  %s",
             coach_domain_name(g_co.domain), coach_intent_name(g_co.intent));
    data_save_cal(0, 0, &a);
}

static void co_note_save_cb(lv_event_t *e){ (void)e;
    const char *t = active_ta ? lv_textarea_get_text(active_ta) : NULL;
    co_write_exports(t);
    toast_show("Logged");
    show_coach();
}
static void co_note_skip_cb(lv_event_t *e){ (void)e;
    co_write_exports(NULL);
    show_coach();
}

static void co_show_note(void){
    kill_kb(); content_clear();
    g_co_open = 1; g_co_reflect = 1;      /* still Coach's screen: no lock over it */
    lv_label_set_text(title_lbl, "Coach");

    lv_obj_t *q = lv_label_create(content);
    lv_obj_set_style_text_font(q, &lv_font_palm_bold, 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_text(q, "Note?");

    lv_obj_t *ta = lv_textarea_create(content);
    lv_obj_set_size(ta, 224, 60);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 32);
    lv_textarea_set_one_line(ta, false);
    lv_textarea_set_max_length(ta, 64);
    lv_textarea_set_placeholder_text(ta, "optional");
    lv_obj_set_style_radius(ta, 0, 0);
    active_ta = ta;

    lv_obj_t *sv = lv_button_create(content);
    lv_obj_set_style_radius(sv, 0, 0);
    lv_obj_set_size(sv, 80, 28);
    lv_obj_align(sv, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_add_event_cb(sv, co_note_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sv); lv_label_set_text(sl, "Save"); lv_obj_center(sl);

    lv_obj_t *sk = lv_button_create(content);
    lv_obj_set_style_radius(sk, 0, 0);
    lv_obj_set_size(sk, 80, 28);
    lv_obj_align(sk, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_add_event_cb(sk, co_note_skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kl = lv_label_create(sk); lv_label_set_text(kl, "Skip"); lv_obj_center(kl);
}

/* -------------------------------------------------------------- the marks wall */
/* Streams coach.sig from the tail, one 80-byte mark at a time, and draws each into
 * a small cell. Abandoned sessions come back hollow (stippled), which is why the
 * log is read alongside it. */
/* Three rows of six rather than four. Adding the back link took a strip off the
 * foot of this screen, and the choice was 24 marks at 26 px or 18 at 30 px -- at
 * this size a mark is already near the edge of legible, so the count gave way. */
#define CO_WALL_N    18
#define CO_WALL_CELL 38
#define CO_WALL_H    118                 /* 3 rows, clear of header and back link */

static void co_wall_mark(lv_draw_buf_t *db, const CoachSigil *sg, int cx, int cy,
                         int size, int solid){
    co_fill_y = solid ? 0 : 0x7FFF;            /* solid, or stippled throughout */
    if(!coach_sigil_ok(sg)){
        for(int y = cy + size/3; y < cy + size - size/3; y++)
            for(int x = cx + size/3; x < cx + size - size/3; x++)
                co_plot(db, x, y);
        return;
    }
    for(int i = 0; i < sg->n - 1; i++){
        int ax = cx + sg->xy[2*i]     * size / 100;
        int ay = cy + sg->xy[2*i + 1] * size / 100;
        int bx = cx + sg->xy[2*i + 2] * size / 100;
        int by = cy + sg->xy[2*i + 3] * size / 100;
        int dx = abs(bx-ax), sx = ax<bx?1:-1, dy = -abs(by-ay), sy = ay<by?1:-1;
        int err = dx+dy;
        for(;;){                                /* 2 px, so it survives at 30 px */
            co_plot(db, ax, ay); co_plot(db, ax+1, ay); co_plot(db, ax, ay+1);
            if(ax==bx && ay==by) break;
            int e2 = 2*err;
            if(e2 >= dy){ err += dy; ax += sx; }
            if(e2 <= dx){ err += dx; ay += sy; }
        }
    }
}

static void show_coach_marks(void){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    g_co_open = 1; g_co_view = CO_VIEW_MARKS;
    lv_label_set_text(title_lbl, "Marks");
    update_cat_trigger();

    lv_obj_t *cv = lv_canvas_create(content);
    lv_canvas_set_buffer(cv, game_cv_buf, CO_CVW, CO_WALL_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(cv, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(cv, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 16);   /* below the header, above "back" */
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
    i1_clear(db);

    /* how many marks are on the card, and where the tail starts */
    long nsig = 0;
    FILE *f = fopen(CO_SIGF, "rb");
    if(f){ fseek(f, 0, SEEK_END); nsig = ftell(f) / (long)sizeof(CoachSigil); }
    long first = nsig > CO_WALL_N ? nsig - CO_WALL_N : 0;

    /* the matching outcomes, so a hollow mark means "gave up" */
    uint8_t bad[CO_WALL_N];
    memset(bad, 0, sizeof bad);
    FILE *lf = fopen(CO_LOG, "rb");
    if(lf){
        uint32_t m = 0;
        if(fread(&m, 4, 1, lf) == 1 && m == CO_LOG_MAGIC &&
           fseek(lf, 4 + first * (long)sizeof(CoachRec), SEEK_SET) == 0){
            CoachRec r;
            for(int i = 0; i < CO_WALL_N && fread(&r, sizeof r, 1, lf) == 1; i++)
                bad[i] = (uint8_t)(co_result(&r) == CO_RES_ABANDONED);
        }
        fclose(lf);
    }

    int shown = 0;
    if(f && fseek(f, first * (long)sizeof(CoachSigil), SEEK_SET) == 0){
        CoachSigil sg;
        for(int i = 0; i < CO_WALL_N && fread(&sg, sizeof sg, 1, f) == 1; i++){
            int col = i % 6, row = i / 6;
            co_wall_mark(db, &sg, 6 + col * CO_WALL_CELL, 2 + row * CO_WALL_CELL,
                         CO_WALL_CELL - 8, !bad[i]);
            shown++;
        }
    }
    if(f) fclose(f);
    lv_obj_invalidate(cv);

    lv_obj_t *hdr = lv_label_create(content);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 8, 0);
    if(shown == 1)   lv_label_set_text(hdr, "1 mark");
    else if(shown)   lv_label_set_text_fmt(hdr, "Last %d marks", shown);
    else      lv_label_set_text(hdr, "No marks yet -- finish a session.");

    lv_obj_t *ft = lv_label_create(content);
    lv_obj_align(ft, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_label_set_text(ft, "hollow = gave up");

    co_home_link(content);
}

/* ---------------------------------------------------------- the weekly report */
static const char *co_advice_text(int code){
    switch(code){
        case CA_EARLIER:  return "Your later sessions struggle far more than your early "
                                 "ones. Shift sessions earlier.";
        case CA_SHORTER:  return "You are cutting sessions short more often than not. "
                                 "Try shorter sessions.";
        case CA_REDUCE:   return "Most of your low-energy sessions are going badly. "
                                 "Reduce intensity.";
        case CA_INCREASE: return "You are finishing almost everything you start. "
                                 "Increase intensity.";
        case CA_STEADY:   return "This is working. Same again this week.";
    }
    return "Keep logging -- five sessions unlocks advice.";
}

/* ---- the coach's speech-bubble tail -------------------------------------
 * A wedge from the top of the bubble up to the portrait's chin. It is the one
 * shape here that is neither a rectangle nor a glyph, so it is painted -- but
 * NOT with an lv_bar/lv_arc/lv_triangle-style widget, which would allocate a
 * draw layer out of the 24 KB pool and live-lock LVGL (docs/BUILD_PROGRESS.md,
 * "Never use a widget that allocates a draw LAYER"). It is a 24x26 I1 canvas
 * over a 94-byte static buffer, written through the shared i1_px helpers with
 * exactly ONE lv_obj_invalidate() at the end -- the set_px invalidate storm that
 * cost every game a second of tap latency is the other trap on this screen.
 *
 * The canvas is opaque, so its bottom row IS the bubble's top border: the row is
 * drawn black outside the wedge and left white between its edges, which is what
 * makes the tail read as an opening into the balloon rather than a sticker on
 * top of it. */
#define CO_TAIL_W   24
#define CO_TAIL_H   26
#define CO_TAIL_APX 20                         /* apex x: points up at the chin  */
#define CO_TAIL_B0  2                          /* base runs from x=B0..          */
#define CO_TAIL_B1  13                         /* ...to x=B1 on the bottom row   */
static uint8_t co_tail_buf[LV_CANVAS_BUF_SIZE(CO_TAIL_W, CO_TAIL_H, 1, 1) + 16];

static void co_tail_line(lv_draw_buf_t *db, int x0, int y0, int x1, int y1){
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for(;;){
        i1_px(db, x0, y0, 1);
        if(x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if(e2 >= dy){ err += dy; x0 += sx; }
        if(e2 <= dx){ err += dx; y0 += sy; }
    }
}

static void co_tail_paint(lv_obj_t *cv){
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
    if(!db) return;
    i1_clear(db);
    /* the bubble's top border, continued across this canvas except where the
     * wedge opens into it */
    for(int x = 0; x < CO_TAIL_W; x++)
        if(x < CO_TAIL_B0 || x > CO_TAIL_B1) i1_px(db, x, CO_TAIL_H - 1, 1);
    co_tail_line(db, CO_TAIL_B0, CO_TAIL_H - 1, CO_TAIL_APX, 0);   /* trailing edge */
    co_tail_line(db, CO_TAIL_B1, CO_TAIL_H - 1, CO_TAIL_APX, 0);   /* leading edge  */
    lv_obj_invalidate(cv);                      /* exactly one, for the whole tail */
}

/* Geometry of the report, in `content` coordinates (240 x 184 visible).
 * The screen is ONE scrolling page, not a scrolling sub-panel with fixed
 * furniture around it: the stats, the coach and his bubble move together, and
 * the only scrollbar that can ever appear is the page's own, at the far right
 * and clear of the portrait. A quiet week fits with no scrollbar at all.
 *
 * The bubble is sized to the WORST CASE of co_advice_text(): all six strings
 * wrap to at most three lines at this width (measured against lv_font_palm,
 * 14 px a line), so the balloon is a fixed height whatever the coach says, and
 * short advice is centred in it rather than left rattling at the top. */
#define CO_BUB_X    2
#define CO_BUB_W    230                          /* clear of the page scrollbar   */
#define CO_BUB_H    58                           /* 3 * 14 text + pad + border    */
#define CO_FACE_R   232                          /* portrait's right edge, inside
                                                    the page scrollbar            */
#define CO_FACE_TOP 4                            /* its y on a quiet week          */
#define CO_CHIN_GAP 4                            /* chin to the tip of the tail    */
#define CO_STAT_W   168                          /* stats column, clear of the face */
#define CO_STAT_ROW 164                          /* every row fits without wrapping */

static void show_coach_report(void){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    g_co_open = 1; g_co_view = CO_VIEW_WEEK;
    lv_label_set_text(title_lbl, "This week");
    update_cat_trigger();

    uint32_t now = (uint32_t)time(NULL);
    uint32_t since = now > 7u * 86400u ? now - 7u * 86400u : 0;
    CoachAgg a;
    co_fold(&a, since);

    /* the page: everything below lives on this, so a heavy week scrolls as one
     * piece. `content` itself is left alone -- it is shared with every other
     * screen in the app and content_clear() does not reset its flags. */
    lv_obj_t *page = lv_obj_create(content);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, COL_BODY, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    /* ---- the statistics, in a column narrow enough to leave the coach a margin.
     * Height is its content and it does not scroll itself; it just makes the page
     * taller, which is what puts the one scrollbar in the one right place. */
    lv_obj_t *box = lv_obj_create(page);
    lv_obj_set_size(box, CO_STAT_W, LV_SIZE_CONTENT);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_bg_color(box, COL_BODY, 0);
    lv_obj_set_style_pad_all(box, 3, 0);
    lv_obj_set_style_pad_left(box, 4, 0);
    lv_obj_set_style_pad_row(box, 1, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    #define CO_ROW(...) do{ lv_obj_t *l_ = lv_label_create(box); \
                            lv_label_set_long_mode(l_, LV_LABEL_LONG_WRAP); \
                            lv_obj_set_width(l_, CO_STAT_ROW); \
                            lv_label_set_text_fmt(l_, __VA_ARGS__); }while(0)

    CO_ROW("Pomodoros   %d", (int)a.n);
    CO_ROW("Focus time  %uh %02um",
           (unsigned)(a.focus_min / 60), (unsigned)(a.focus_min % 60));

    /* only the domains that were actually used. With six of them, printing the
     * empty ones pushed the advice -- the point of the screen -- off the bottom.
     * The bar caps at 6 rather than 12: "Relationships" plus twelve '#' is 205 px
     * and no longer fits the narrowed column, and six cells still rank the week. */
    for(int d = 0; d < CO_NDOM; d++){
        if(!a.dom[d]) continue;
        char bar[7];
        int nb = a.dom[d] > 6 ? 6 : a.dom[d];
        for(int i = 0; i < nb; i++) bar[i] = '#';
        bar[nb] = 0;
        CO_ROW("%-13s %-6s %d", coach_domain_name(d), bar, (int)a.dom[d]);
    }

    int bs = coach_best_slot(&a);
    if(bs >= 0) CO_ROW("Best time   %s, %d%%", coach_slot_name(bs),
                       coach_slot_ok_pct(&a, bs));
    int tb = coach_top_blocker(&a);
    if(tb != CO_BLK_NONE) CO_ROW("Top blocker %s (%d)", coach_blocker_name(tb),
                                 (int)a.blk[tb]);
    int hi = coach_energy_great_pct(&a, CO_ENERGY_HIGH);
    int lo = coach_energy_great_pct(&a, CO_ENERGY_LOW);
    /* "High .. / Low .." is 183 px and would wrap in the narrowed column */
    if(hi >= 0 && lo >= 0) CO_ROW("Energy      Hi %d%% Lo %d%%", hi, lo);
    #undef CO_ROW

    /* Read the portrait's size off the descriptor rather than restating it: it is
     * generated art (tools/gen_coach_face.py) and everything below is placed from
     * it, so a regenerated face at a different size still lands correctly. */
    const int face_w = (int)coach_face.header.w;
    const int face_h = (int)coach_face.header.h;
    const int face_x = CO_FACE_R - face_w;
    /* Where the bubble lands: below the stats, but never so high that it eats into
     * the portrait's spot at the top of the page. The coach then hangs off the
     * bubble rather than off the top of the screen -- a long week pushes the pair
     * down together, so the tail stays the short hop from his chin to the balloon
     * instead of stretching into a wire. He is beside the stat column either way;
     * on a heavy week it is the lower half of it. */
    const int bub_min = CO_FACE_TOP + face_h + CO_CHIN_GAP + (CO_TAIL_H - 1);
    lv_obj_update_layout(box);
    int bub_y = lv_obj_get_height(box) + 6;
    if(bub_y < bub_min) bub_y = bub_min;
    int face_y = bub_y - (CO_TAIL_H - 1) - CO_CHIN_GAP - face_h;

    /* ---- the coach himself: flash-resident A8, recolored to the ink colour the
     * same way the launcher icons are. No pool cost and nothing to repaint. */
    lv_obj_t *face = lv_image_create(page);
    lv_image_set_src(face, &coach_face);
    lv_obj_set_pos(face, face_x, face_y);
    lv_obj_set_style_image_recolor(face, COL_LINE, 0);
    lv_obj_set_style_image_recolor_opa(face, LV_OPA_COVER, 0);

    /* ---- the bubble: a plain bordered rectangle. Rounded corners and a border
     * are drawn straight into the frame buffer; only the indicator widgets take
     * a layer, and this is not one of them. */
    lv_obj_t *bub = lv_obj_create(page);
    lv_obj_set_size(bub, CO_BUB_W, CO_BUB_H);
    lv_obj_set_pos(bub, CO_BUB_X, bub_y);
    lv_obj_set_style_radius(bub, 6, 0);
    lv_obj_set_style_border_width(bub, 1, 0);
    lv_obj_set_style_border_color(bub, COL_LINE, 0);
    lv_obj_set_style_bg_color(bub, COL_BODY, 0);
    lv_obj_set_style_pad_all(bub, 6, 0);
    lv_obj_clear_flag(bub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *say = lv_label_create(bub);
    lv_label_set_long_mode(say, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(say, CO_BUB_W - 2 - 12);
    lv_obj_set_style_text_align(say, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(say, co_advice_text(coach_advise(&a)));
    lv_obj_center(say);

    /* ---- and the tail, joining the two. Created after the bubble so it paints
     * over the top border, which is the border it replaces. */
    lv_obj_t *tail = lv_canvas_create(page);
    lv_canvas_set_buffer(tail, co_tail_buf, CO_TAIL_W, CO_TAIL_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(tail, 0, lv_color_to_32(COL_BODY, 0xFF));
    lv_canvas_set_palette(tail, 1, lv_color_to_32(COL_LINE, 0xFF));
    lv_obj_set_pos(tail, face_x + face_w / 2 - CO_TAIL_APX, bub_y - (CO_TAIL_H - 1));
    co_tail_paint(tail);

    co_home_link_at(page, bub_y + CO_BUB_H + 4);
}

/* ------------------------------------------------------------------ the home */
static void co_start_cb(lv_event_t *e){ (void)e; co_show_ritual(0); }
static void co_marks_cb(lv_event_t *e){ (void)e; show_coach_marks(); }
static void co_week_cb(lv_event_t *e){ (void)e; show_coach_report(); }

static void show_coach(void){
    kill_kb(); cur_app = NULL; cur_uid = 0;
    content_clear();
    co_load();
    g_co_open = 1;
    lv_label_set_text(title_lbl, "Coach");
    update_cat_trigger();

    /* a session survived the trip here: pick it back up rather than starting over */
    if(g_co.phase == CO_PH_RUNNING){ co_seal(); return; }
    if(g_co.phase == CO_PH_REFLECT){ co_show_reflect(); return; }
    g_co_view = CO_VIEW_HOME;

    uint32_t now = (uint32_t)time(NULL);
    int tz = co_tz();
    int today  = coach_today_now(&g_co, now, tz);
    int streak = coach_streak_now(&g_co, now, tz);

    lv_obj_t *cnt = lv_label_create(content);
    lv_obj_set_style_text_font(cnt, &lv_font_palm_bold, 0);
    lv_obj_align(cnt, LV_ALIGN_TOP_LEFT, 10, 2);
    lv_label_set_text_fmt(cnt, "%d today", today);

    /* streak moves up onto the count's line: the explainer below needs the rows */
    lv_obj_t *sk = lv_label_create(content);
    lv_obj_align(sk, LV_ALIGN_TOP_RIGHT, -10, 2);
    lv_label_set_text_fmt(sk, "streak %d day%s", streak, streak == 1 ? "" : "s");

    /* the day-goal bar: ten cells of '#' and '-'. A label, never an lv_bar -- a
     * bar allocates a draw layer and live-locks LVGL on this pool. */
    char bar[13];
    int goal = g_co.day_goal ? g_co.day_goal : 6;
    int filled = today >= goal ? 10 : today * 10 / goal;
    bar[0] = '[';
    for(int i = 0; i < 10; i++) bar[1 + i] = i < filled ? '#' : '-';
    bar[11] = ']'; bar[12] = 0;
    lv_obj_t *bl = lv_label_create(content);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 10, 20);
    lv_label_set_text_fmt(bl, "%s of %d", bar, goal);

    /* What the app IS, on the screen where you decide whether to use it. A lone
     * Start button asked people to commit to something unnamed -- and because a
     * session seals the display, that is a bigger commitment than it looks.
     *
     * This says what Coach is FOR, not what the next screens will ask. Copy that
     * narrates steps ("pick a domain, draw a mark") is meaningless to someone who
     * has not seen those screens yet, and redundant to someone who has. */
    lv_obj_t *ex = lv_label_create(content);
    lv_label_set_long_mode(ex, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ex, 222);
    lv_obj_align(ex, LV_ALIGN_TOP_LEFT, 10, 38);
    lv_label_set_text(ex, "Work in one timed stretch with nothing else on the "
                          "screen. Coach keeps score and learns when you focus "
                          "best.");

    lv_obj_t *go = lv_button_create(content);
    lv_obj_set_size(go, 200, 42);
    lv_obj_set_style_radius(go, 0, 0);
    lv_obj_align(go, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_add_event_cb(go, co_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(go);
    lv_obj_set_style_text_font(gl, &lv_font_palm_bold, 0);
    lv_label_set_text_fmt(gl, "Start %d min", (int)g_co.pref_min);
    lv_obj_center(gl);

    /* one line of earned insight -- only once there is enough to earn it */
    CoachAgg a;
    int n = co_fold(&a, 0);
    lv_obj_t *st = lv_label_create(content);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 10, 132);
    int bs = n >= 5 ? coach_best_slot(&a) : -1;
    int td = n >= 5 ? coach_top_domain(&a) : -1;
    if(bs >= 0 && td >= 0)
        lv_label_set_text_fmt(st, "strongest: %s, %s", coach_domain_name(td),
                              coach_slot_name(bs));
    else
        lv_label_set_text_fmt(st, "%d session%s logged so far", (int)g_co.total_n,
                              g_co.total_n == 1 ? "" : "s");

    lv_obj_t *mk = lv_button_create(content);
    lv_obj_set_style_radius(mk, 0, 0);
    lv_obj_set_size(mk, 88, 28);
    lv_obj_align(mk, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_add_event_cb(mk, co_marks_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(mk); lv_label_set_text(ml, "Marks"); lv_obj_center(ml);

    lv_obj_t *wk = lv_button_create(content);
    lv_obj_set_style_radius(wk, 0, 0);
    lv_obj_set_size(wk, 88, 28);
    lv_obj_align(wk, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
    lv_obj_add_event_cb(wk, co_week_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wk); lv_label_set_text(wl, "Week"); lv_obj_center(wl);
}

void ui_init(void){
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BODY, 0);
    lv_obj_set_style_text_font(scr, &lv_font_palm, 0);   /* authentic Palm font, inherited */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* per-device Graffiti calibration (trainer's training mode): if the user has
     * recorded their own strokes, load them so recognition uses them system-wide. */
    graffiti_user_load("/sdcard/graf_user.dat");

    /* News reader's RSS sources: load the SD list, seeding the built-in feeds on
     * first run so News works out of the box (edit via Preferences > News feeds). */
    feeds_load_or_seed(FEEDS_PATH);

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
    ink_db = lv_canvas_get_draw_buf(ink_canvas);   /* inkpx/ink_clear write this directly */
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

    /* Boot straight to the lock-screen dashboard; the launcher is built lazily on
     * unlock (see lock_release_cb) so its icon grid and the dashboard never occupy
     * the 24 KB LVGL pool at once -- that overflowed the pool on the 32-bit wasm
     * build (the 64-bit native sim's 48 KB pool hid it). A 15 s timer keeps the
     * locked clock fresh; the port layer re-raises the lock on every wake. */
    lv_timer_create(dash_tick, 15000, NULL);
    lv_timer_create(ms_tick, 1000, NULL);        /* live Mines clock (no-op unless it's showing) */
    lv_timer_create(sd_tick, 1000, NULL);        /* live Sudoku clock (no-op unless it's showing) */
    lv_timer_create(zp_tick, 1000, NULL);        /* live Zip clock (no-op unless it's showing) */
    /* Coach runs from ANY screen: the session must end correctly whether the
     * user is on the lock screen, in a game, or in the Calculator. */
    lv_timer_create(co_tick, 1000, NULL);
    co_load();                                   /* resolve a session left running */
    ui_show_lock();
}
