/* graffiti.h -- unistroke text entry ($1 recognizer). U6 framework: the pipeline
 * (stroke capture -> recognize -> char) is here; template set + thresholds need
 * on-device tuning. */
#ifndef GRAFFITI_RECOGNIZER_H   /* not GRAFFITI_H -- collides with display.h's strip-height macro */
#define GRAFFITI_RECOGNIZER_H

/* shift gesture (upstroke): capitalize the next letter. Caller holds the state. */
#define GRAF_SHIFT '\x01'
/* punctuation shift (a single tap): the NEXT stroke is read as punctuation, like
 * real Graffiti. graffiti.c holds the armed state internally; it returns this code
 * so the caller can light a punctuation indicator. Cleared once the next stroke
 * resolves (to a punctuation char, or nothing if rejected). Period is the tap that
 * follows the shift -- i.e. two taps, exactly as on PalmOS. */
#define GRAF_PUNCT '\x02'

void graffiti_clear(void);              /* start a new stroke */
void graffiti_add_point(int x, int y);  /* add a sampled point during the stroke */
/* recognize the buffered stroke against the letter set (digits==0, lowercase a-z)
 * or the digit set (digits!=0). Gestures: horizontal L->R swipe = ' ' (space),
 * R->L = '\b' (backspace), vertical bottom->top = GRAF_SHIFT, single tap =
 * GRAF_PUNCT (arms punctuation for the next stroke). Returns the character/gesture
 * code, or 0 if none/too short. */
char graffiti_recognize(int digits);

/* the ideal stroke for lowercase letter c (a-z): control points in a ~0..10 grid
 * (y down), *npairs point pairs. NULL for non-letters. Used by the trainer to
 * draw a stroke guide. */
const float *graffiti_letter_template(char c, int *npairs);

/* like graffiti_letter_template but for ANY trainable glyph: a-z, 0-9, and the
 * punctuation set. NULL for glyphs drawn as a tap (e.g. '.'). Trainer guide. */
const float *graffiti_glyph_template(char c, int *npairs);

/* $1 match distance of the last recognized letter/digit (lower = better). Used by
 * the trainer for a graded score. Valid right after graffiti_recognize(). */
float graffiti_last_distance(void);

/* --- per-device user templates (the trainer's "training mode") --- */
/* capture the just-drawn stroke as the user's template for lowercase c. 1 on ok. */
int  graffiti_capture_user(char c);
/* how many letters have a user template; clear them all. */
int  graffiti_user_count(void);
void graffiti_user_reset(void);
/* persist / restore the user templates (caller supplies the SD path). 1 on ok. */
int  graffiti_user_save(const char *path);
int  graffiti_user_load(const char *path);

#endif
