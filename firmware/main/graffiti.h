/* graffiti.h -- unistroke text entry ($1 recognizer). U6 framework: the pipeline
 * (stroke capture -> recognize -> char) is here; template set + thresholds need
 * on-device tuning. */
#ifndef GRAFFITI_RECOGNIZER_H   /* not GRAFFITI_H -- collides with display.h's strip-height macro */
#define GRAFFITI_RECOGNIZER_H

void graffiti_clear(void);              /* start a new stroke */
void graffiti_add_point(int x, int y);  /* add a sampled point during the stroke */
/* recognize the buffered stroke: returns the character, or 0 if none/too short. */
char graffiti_recognize(void);

#endif
