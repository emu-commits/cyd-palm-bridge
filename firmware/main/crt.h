/* crt.h -- a TV's look for one rectangle of the screen (docs/DEX_PLAN.md 2.4).
 *
 * It runs in the display's flush, on the pixels LVGL has just drawn, so nothing
 * is drawn differently for it and it needs no copy of the box: each pixel reads
 * only its own row. The device and the simulator both call it from their
 * flush_cb, before anything else touches the buffer.
 *
 * Pure C, no LVGL. The memory (small tables, under 1 KB) is heap, taken by
 * crt_on() and given back by crt_off(). */
#ifndef CRT_H
#define CRT_H

#include <stdint.h>

enum { CRT_OFF, CRT_LIGHT, CRT_STANDARD, CRT_STRONG, CRT_NSTRENGTHS };

/* Treat x0..x1, y0..y1 (inclusive, screen coordinates) as the tube, with corners
 * rounded at `radius` (at most CRT_MAX_RADIUS). Outside them, a ring of `well` one
 * pixel wide, and beyond that `outside`: the box's corners are squares, and whatever
 * is round the box has to show in them (RGB565 both). The caller draws the rest of
 * the ring, at radius + 1, one pixel outside the box. The caller invalidates the box.
 * 0, or -1 if the tables can't be had: the screen then simply has no effect. */
#define CRT_MAX_RADIUS 12
int  crt_on(int x0, int y0, int x1, int y1, int radius, uint16_t well, uint16_t outside);
void crt_off(void);
int  crt_active(void);

/* The rounded corners and nothing else, whatever the strength, and no widening: for
 * Critters, whose text the effect made unreadable on the panel. Until crt_off(). */
void crt_plain(int on);

/* The system's menu or an alert over the screen isn't part of the tube: the pixels
 * in this rectangle are left alone, and the picture round it keeps the effect.
 * x2 < x1 for none. It returns 1 if that's a change, for the caller to invalidate. */
int  crt_exclude(int x1, int y1, int x2, int y2);

/* How strong (CRT_OFF keeps the rounded corners). */
void crt_set_strength(int s);
int  crt_strength(void);
const char *crt_strength_name(int s);

/* flush_cb: the strip x1..x2, y1..y2 at px (RGB565 in the CPU's order, before any swap). */
void crt_flush(int x1, int y1, int x2, int y2, uint16_t *px);

/* The display's LV_EVENT_INVALIDATE_AREA: an area that touches the box widens to
 * the box's whole rows, because a pixel's glow depends on its neighbours and a
 * part of a row, redrawn alone, would leave a seam at its edges. */
void crt_widen(int *x1, int y1, int *x2, int y2);

#endif
