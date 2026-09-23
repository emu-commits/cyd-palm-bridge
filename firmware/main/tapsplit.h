/* tapsplit.h -- recover the lift between two fast taps. Header-only, pure.
 *
 * The touch panel is sampled, not interrupt-driven. When someone types fast on
 * the Calculator or the phone keypad, the gap between lifting one finger and
 * landing the next can be shorter than a sample period, so the input layer
 * never sees the lift: it sees ONE press that slides from the first key to the
 * second. A button matrix reads that as "slid off the key" and cancels it --
 * the calculator (which types on press) lost the second digit, and the phone
 * keypad (which types on release, so that a hold on 0 can mean +) lost both.
 *
 * A finger cannot move tens of pixels in one sample period while pressed; a
 * lift and a new tap elsewhere can. So when a press JUMPS further than `jump`
 * pixels between two consecutive samples, report a release at the old point
 * now and the press at the new point on the next read. That is exactly what
 * the finger did.
 *
 * `enabled` is for screens of discrete keys only. A Graffiti stroke or a drag
 * moves continuously and must never be cut in two; the caller passes 1 only
 * while a keypad is up (ui_discrete_taps()).
 *
 * Both input paths -- firmware/main/lvgl_port.c on the device and
 * sim/sim_port.c in the simulator -- run this same function, which is what
 * lets the smoke gate a behaviour whose cause is a sample rate. */
#ifndef TAPSPLIT_H
#define TAPSPLIT_H

typedef struct {
    int down;          /* what was last REPORTED                       */
    int x, y;          /* where it was last reported                    */
    int pend;          /* a synthesized release is out; press follows   */
} TapSplit;

/* Feed one raw sample; returns the state to report (1 pressed, 0 released)
 * and writes the point to report. */
static inline int tapsplit_step(TapSplit *t, int raw_down, int x, int y,
                                int enabled, int jump, int *ox, int *oy){
    if(t->pend){
        t->pend = 0;                 /* the release went out last read */
    } else if(raw_down && t->down && enabled){
        int dx = x - t->x, dy = y - t->y;
        if(dx * dx + dy * dy > jump * jump){
            t->pend = 1;
            t->down = 0;             /* lift, at the OLD point */
            *ox = t->x; *oy = t->y;
            return 0;
        }
    }
    t->down = raw_down;
    if(raw_down){ t->x = x; t->y = y; }
    *ox = t->x; *oy = t->y;
    return raw_down;
}

#endif
