/* kana_data.h -- ordered kana syllabary for the Kana trainer (roadmap #3, Tier 1).
 *
 * The learning order is the classic gojuon: all basic hiragana first, then all
 * basic katakana. Each entry pairs the kana (UTF-8, rendered with lv_font_kana)
 * with its Hepburn romaji -- the answer the user draws in the Graffiti strip,
 * one Latin letter at a time (e.g. "ku" = draw 'k' then 'u'). Dakuten/handakuten
 * and combos are intentionally out of scope for Tier 1's sound drill. */
#ifndef KANA_DATA_H
#define KANA_DATA_H

typedef struct {
    const char *kana;    /* UTF-8, one kana */
    const char *romaji;  /* Hepburn, lowercase a-z only (what the user draws) */
    unsigned char script; /* 0 = hiragana, 1 = katakana */
} KanaEntry;

/* the full ordered table (hiragana gojuon, then katakana gojuon). */
extern const KanaEntry KANA[];
extern const int KANA_N;

#endif
