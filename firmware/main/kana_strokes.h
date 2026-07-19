/* kana_strokes.h -- per-kana stroke polylines WITH official stroke order, for the
 * Kana trainer's writing challenge (roadmap #3, Tier 2). Parallel to KANA[] in
 * kana_data.h. Derived from KanjiVG (CC BY-SA 3.0, http://kanjivg.tagaini.net):
 * each SVG stroke path was flattened + resampled into a short polyline in a
 * ~0..108 grid (y down, same convention as the graffiti templates). */
#ifndef KANA_STROKES_H
#define KANA_STROKES_H

typedef struct { unsigned char npts; const unsigned char *pts; } KStroke;   /* pts[2*npts], 0..108 */
typedef struct { unsigned char n; const KStroke *s; } KanaStrokes;           /* one kana, n strokes in order */

extern const KanaStrokes KANA_STROKES[];   /* index-aligned with KANA[] */
extern const int KANA_STROKES_N;

#endif
