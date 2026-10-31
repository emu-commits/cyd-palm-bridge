/* ui_monster.h -- Critters, the monster game's screens (docs/GAME_PLAN.md §7).
 *
 * ui.c owns the shell: it clears the content area, turns full screen on, and
 * calls mon_ui_show() to build the game into it. Everything the game needs
 * comes from one heap block allocated there and freed by mon_ui_close(),
 * which content_clear() calls after the game's objects are gone. */
#ifndef UI_MONSTER_H
#define UI_MONSTER_H

#include "lvgl.h"

#define MON_DIR "/sdcard/games/monster"

/* Build the game into `parent` (240 x 296, full screen). */
void mon_ui_show(lv_obj_t *parent);
/* Save and free everything. A battle in progress is kept in pause.dat and
 * comes back on the next open (the lock pauses the game: §13 Q3). */
void mon_ui_close(void);
/* Write the game to the card as mon_ui_close() would, and keep playing. The
 * web emulator calls it before each copy of the card to IndexedDB, because a
 * closed tab never reaches mon_ui_close(). */
void mon_ui_checkpoint(void);
/* 1 while the game is open */
int  mon_ui_open(void);
/* 1 while the game is open on USB power: the screen stays on (§13 Q3) */
int  mon_ui_keep_awake(void);

/* the Palm menu's items */
int  mon_ui_playing(void);       /* a game is under way (Party, Save, New game) */
int  mon_ui_save(void);          /* 0 saved; else the error is in mon_ui_error() */
const char *mon_ui_error(void);
void mon_ui_party(void);
void mon_ui_bag(void);           /* the items and the coins; in a battle, its Item menu */
void mon_ui_new_game(void);      /* asks first */
void mon_ui_credits(void);
/* Auto-play (GAME_PLAN.md §8): on while a game is under way; a touch on the
 * game turns it off */
int  mon_ui_auto(void);
void mon_ui_set_auto(int on);
/* ui.c's: the title bar says so while Auto-play is on */
void ui_mon_auto_changed(int on);

#ifdef UI_DEVTOOLS
/* The smoke tour: put the player at (x, y) of area i, facing N E S W = 0..3. */
void mon_ui_test_place(int area, int x, int y, int facing);
/* ...and set story flag f and the purse */
void mon_ui_test_give(int flag, int coins);
#endif

#endif
