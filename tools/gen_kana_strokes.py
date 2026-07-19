#!/usr/bin/env python3
"""Generate firmware/main/kana_strokes.c/.h from KanjiVG.

KanjiVG (http://kanjivg.tagaini.net, CC BY-SA 3.0) provides per-character SVG
stroke paths *in stroke order*. This script fetches the 92 basic-kana SVGs,
flattens each stroke path into a short polyline resampled by arc length, and
packs them (parallel to KANA[] in kana_data.h) into a C table the Kana trainer's
writing challenge (roadmap #3, Tier 2) matches user strokes against.

Requires: svgpathtools (pip install svgpathtools) + network access to
raw.githubusercontent.com. Run from the repo root:  python3 tools/gen_kana_strokes.py
"""
import os, re, sys, urllib.request

# the 92 basic kana in KANA[] order (hiragana gojuon, then katakana gojuon).
HIRA = [("あ",0x3042),("い",0x3044),("う",0x3046),("え",0x3048),("お",0x304a),
    ("か",0x304b),("き",0x304d),("く",0x304f),("け",0x3051),("こ",0x3053),
    ("さ",0x3055),("し",0x3057),("す",0x3059),("せ",0x305b),("そ",0x305d),
    ("た",0x305f),("ち",0x3061),("つ",0x3064),("て",0x3066),("と",0x3068),
    ("な",0x306a),("に",0x306b),("ぬ",0x306c),("ね",0x306d),("の",0x306e),
    ("は",0x306f),("ひ",0x3072),("ふ",0x3075),("へ",0x3078),("ほ",0x307b),
    ("ま",0x307e),("み",0x307f),("む",0x3080),("め",0x3081),("も",0x3082),
    ("や",0x3084),("ゆ",0x3086),("よ",0x3088),
    ("ら",0x3089),("り",0x308a),("る",0x308b),("れ",0x308c),("ろ",0x308d),
    ("わ",0x308f),("を",0x3092),("ん",0x3093)]
KATA = [("ア",0x30a2),("イ",0x30a4),("ウ",0x30a6),("エ",0x30a8),("オ",0x30aa),
    ("カ",0x30ab),("キ",0x30ad),("ク",0x30af),("ケ",0x30b1),("コ",0x30b3),
    ("サ",0x30b5),("シ",0x30b7),("ス",0x30b9),("セ",0x30bb),("ソ",0x30bd),
    ("タ",0x30bf),("チ",0x30c1),("ツ",0x30c4),("テ",0x30c6),("ト",0x30c8),
    ("ナ",0x30ca),("ニ",0x30cb),("ヌ",0x30cc),("ネ",0x30cd),("ノ",0x30ce),
    ("ハ",0x30cf),("ヒ",0x30d2),("フ",0x30d5),("ヘ",0x30d8),("ホ",0x30db),
    ("マ",0x30de),("ミ",0x30df),("ム",0x30e0),("メ",0x30e1),("モ",0x30e2),
    ("ヤ",0x30e4),("ユ",0x30e6),("ヨ",0x30e8),
    ("ラ",0x30e9),("リ",0x30ea),("ル",0x30eb),("レ",0x30ec),("ロ",0x30ed),
    ("ワ",0x30ef),("ヲ",0x30f2),("ン",0x30f3)]
KANA = HIRA + KATA
BASE = "https://raw.githubusercontent.com/KanjiVG/kanjivg/master/kanji/%05x.svg"


def fetch(cp, cache="/tmp/kvg"):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, "%05x.svg" % cp)
    if not os.path.exists(path):
        urllib.request.urlretrieve(BASE % cp, path)
    return open(path).read()


def strokes_for(svg):
    from svgpathtools import parse_path
    out = []
    for d in re.findall(r'<path[^>]*\bd="([^"]+)"', svg):
        p = parse_path(d)
        L = p.length(error=1e-3)
        if L <= 0.001:
            z = p.point(0.0); out.append([(z.real, z.imag)]); continue
        K = max(4, min(14, round(L / 10) + 2))
        pts = []
        for i in range(K):
            t = 0.0 if i == 0 else 1.0 if i == K - 1 else p.ilength(min(L * i / (K - 1), L * 0.999999), s_tol=1e-3)
            z = p.point(t); pts.append((z.real, z.imag))
        out.append(pts)
    return out


def u8(v):
    v = int(round(v)); return 0 if v < 0 else 108 if v > 108 else v


def main():
    body, tbl = [], []
    for idx, (ch, cp) in enumerate(KANA):
        sts = strokes_for(fetch(cp))
        snames = []
        for si, pts in enumerate(sts):
            arr = ",".join("%d,%d" % (u8(x), u8(y)) for x, y in pts)
            nm = "ks_%02d_%d" % (idx, si)
            body.append("static const unsigned char %s[]={%s};" % (nm, arr))
            snames.append((nm, len(pts)))
        gname = "kg_%02d" % idx
        body.append("static const KStroke %s[]={%s};" % (gname, ",".join("{%d,%s}" % (n, nm) for nm, n in snames)))
        tbl.append("  {%d,%s}, /* %05x %s */" % (len(sts), gname, cp, ch))
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dst = os.path.join(root, "firmware", "main")
    with open(os.path.join(dst, "kana_strokes.h"), "w") as f:
        f.write(HEADER)
    with open(os.path.join(dst, "kana_strokes.c"), "w") as f:
        f.write(SRC_HEAD + "\n".join(body) + SRC_MID + "\n".join(tbl) + SRC_TAIL)
    print("wrote kana_strokes.{c,h} for %d kana" % len(KANA))


HEADER = '''/* kana_strokes.h -- per-kana stroke polylines WITH official stroke order, for the
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
'''
SRC_HEAD = '''/* kana_strokes.c -- GENERATED from KanjiVG (CC BY-SA 3.0). Do not hand-edit;
 * regenerate with tools/gen_kana_strokes.py. Stroke order = KanjiVG path order. */
#include "kana_strokes.h"

'''
SRC_MID = '''

const KanaStrokes KANA_STROKES[] = {
'''
SRC_TAIL = '''
};
const int KANA_STROKES_N = (int)(sizeof(KANA_STROKES)/sizeof(KANA_STROKES[0]));
'''

if __name__ == "__main__":
    sys.exit(main())
