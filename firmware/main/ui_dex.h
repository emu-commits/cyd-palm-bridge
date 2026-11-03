/* ui_dex.h -- Dex, the critter index's screens (docs/DEX_PLAN.md).
 *
 * ui.c owns the shell: it clears the content area, turns full screen on, dresses
 * the title bar as part of the gadget, and calls dex_ui_show(). Everything comes
 * from heap taken there and given back by dex_ui_close(), which content_clear()
 * calls after the Dex's objects are gone. */
#ifndef UI_DEX_H
#define UI_DEX_H

#include "lvgl.h"

/* Build the gadget into `parent` (240 x 296, full screen). */
void dex_ui_show(lv_obj_t *parent);
void dex_ui_close(void);
int  dex_ui_open(void);

/* The TV effect's strength, which the Dex and Critters share (crt.h): the
 * choice is kept on the card, in its own file. */
void tv_strength_load(void);
void tv_strength_next(void);        /* the menu's item: Light, Standard, Strong, Off */

#ifdef UI_DEVTOOLS
/* The smoke tour: write a save with these critters seen and caught (comma-separated
 * stable ids, or "-"). With both "-" and no game wanted: `clear` removes the save. */
void dex_ui_test_save(const char *seen, const char *caught);
void dex_ui_test_clear(void);
#endif

#endif
