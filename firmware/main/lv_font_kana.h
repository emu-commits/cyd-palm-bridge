/* lv_font_kana.h -- compact CJK-kana bitmap font (hiragana + katakana), 38px 1bpp,
 * subset of IPAGothic. Latin glyphs stay in lv_font_palm; this covers only the
 * kana ranges so the Kana trainer can show the target character. */
#ifndef LV_FONT_KANA_H
#define LV_FONT_KANA_H
#include "lvgl.h"
extern const lv_font_t lv_font_kana;
/* the same kana at 20 px, for Study's readings (tools/gen_kana_font.py);
 * anything else in a reading falls back to lv_font_palm */
extern const lv_font_t lv_font_kana_20;
#endif
