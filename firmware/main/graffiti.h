/* graffiti.h -- unistroke text entry ($1 recognizer). U6 framework: the pipeline
 * (stroke capture -> recognize -> char) is here; template set + thresholds need
 * on-device tuning. */
#ifndef GRAFFITI_RECOGNIZER_H   /* not GRAFFITI_H -- collides with display.h's strip-height macro */
#define GRAFFITI_RECOGNIZER_H

/* shift gesture (upstroke): capitalize the next letter. Caller holds the state. */
#define GRAF_SHIFT '\x01'

void graffiti_clear(void);              /* start a new stroke */
void graffiti_add_point(int x, int y);  /* add a sampled point during the stroke */
/* recognize the buffered stroke against the letter set (digits==0, lowercase a-z)
 * or the digit set (digits!=0). Gestures: horizontal L->R swipe = ' ' (space),
 * R->L = '\b' (backspace), vertical bottom->top = GRAF_SHIFT. Returns the
 * character/gesture code, or 0 if none/too short. */
char graffiti_recognize(int digits);

#endif
