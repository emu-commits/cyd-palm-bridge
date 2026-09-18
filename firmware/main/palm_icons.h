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
/* The speakers: a portrait stands in the right-hand margin of a screen that has
 * something to say, beside the content and attached to a speech bubble. Coach's
 * is on the weekly report; the Assistant and the Guru are for the screens that
 * come after it. All 60 wide, heights differing with the art -- read them off
 * the descriptor rather than restating them. Regenerate with tools/gen_faces.py.
 * Flash rodata like every icon here, so the 24 KB LVGL pool never holds one. */
extern const lv_image_dsc_t coach_face;        /* 60x67, 4020 bytes */
extern const lv_image_dsc_t assistant_face;    /* 60x77, 4620 bytes */
extern const lv_image_dsc_t guru_face;         /* 60x74, 4440 bytes */
/* silkscreen buttons (flank the Graffiti area) */
extern const lv_image_dsc_t silk_home, silk_menu, silk_find, silk_calc;
#endif
