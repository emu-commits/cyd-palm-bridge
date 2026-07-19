/* kana_write.h -- per-stroke matcher for the Kana writing challenge (Tier 2).
 *
 * A self-contained copy of the $1 unistroke core (resample -> uniform-scale
 * normalize -> centroid origin -> nearest by average point distance, with a small
 * angle search) that compares ONE user-drawn stroke against ONE expected kana
 * stroke. It is deliberately separate from graffiti.c so the Latin recognizer is
 * untouched. Because the app enforces official stroke ORDER, multi-stroke
 * recognition decomposes into N of these single-stroke checks in sequence; the
 * matcher is direction-sensitive, so a stroke drawn the wrong way is rejected. */
#ifndef KANA_WRITE_H
#define KANA_WRITE_H
#include <stdint.h>
#include "kana_strokes.h"

/* average $1 distance between a user stroke (raw x,y int16 pairs, `un` points) and
 * an expected kana stroke. Lower = closer; typical accept threshold ~26. Returns a
 * large value if either stroke is degenerate. */
float kana_stroke_dist(const int16_t *uxy, int un, const KStroke *expect);

#endif
