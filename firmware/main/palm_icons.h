/* Palm app launcher icons (from PumpkinOS tAIB resources), LVGL A8 images. */
#ifndef PALM_ICONS_H
#define PALM_ICONS_H
#include "lvgl.h"
extern const lv_image_dsc_t icon_datebook, icon_address, icon_todo, icon_memo, icon_hotsync;
extern const lv_image_dsc_t icon_graffiti;
extern const lv_image_dsc_t icon_news;
extern const lv_image_dsc_t icon_kana;      /* Kana trainer (roadmap #3, Tier 1) */
extern const lv_image_dsc_t icon_games;     /* Games launcher (product roadmap) */
extern const lv_image_dsc_t icon_mines;     /* Minesweeper (Games sub-launcher) */
extern const lv_image_dsc_t icon_wordie;    /* Wordie (Games sub-launcher) */
extern const lv_image_dsc_t icon_sudoku;    /* Sudoku (Games sub-launcher) */
extern const lv_image_dsc_t icon_zip;       /* Zip path puzzle (Games sub-launcher) */
extern const lv_image_dsc_t icon_coach;     /* Coach focus timer (ritual Pomodoro) */
extern const lv_image_dsc_t icon_guru;      /* Guru longevity habits (gen_guru_icon.py) */
/* The speakers: a portrait stands in the right-hand margin of a screen that has
 * something to say, beside the content and attached to a speech bubble. Coach's
 * is on the weekly report; the Assistant and the Guru are for the screens that
 * come after it. All 60 wide, heights differing with the art -- read them off
 * the descriptor rather than restating them. Regenerate with tools/gen_faces.py.
 * Flash rodata like every icon here, so the 32 KB LVGL pool never holds one. */
extern const lv_image_dsc_t coach_face;        /* 60x66, 3960 bytes */
extern const lv_image_dsc_t assistant_face;    /* 60x77, 4620 bytes */
extern const lv_image_dsc_t guru_face;         /* 60x74, 4440 bytes */
/* The nine Settings tiles (W2). All 24x22 A8, all knocked out of one shared ink
 * disk, all from tools/gen_settings_icons.py -- regenerate the whole set, never
 * one of them, or the disk drifts and the grid stops looking like a set.
 * Declared in the order they are laid out on the Settings screen. */
extern const lv_image_dsc_t icon_set_wifi;      /* four remembered networks   */
extern const lv_image_dsc_t icon_set_accounts;  /* CalDAV/CardDAV login       */
extern const lv_image_dsc_t icon_set_news;      /* the feed list              */
extern const lv_image_dsc_t icon_set_datetime;  /* time, zone, 12/24h, worlds */
extern const lv_image_dsc_t icon_set_display;   /* brightness, backlight      */
extern const lv_image_dsc_t icon_set_location;  /* lat/long -> weather        */
extern const lv_image_dsc_t icon_set_sync;      /* policy + collections       */
extern const lv_image_dsc_t icon_set_owner;     /* name on the lock screen    */
extern const lv_image_dsc_t icon_set_about;     /* version + provenance       */
/* silkscreen buttons (flank the Graffiti area) */
extern const lv_image_dsc_t silk_home, silk_menu, silk_find, silk_calc;
#endif
