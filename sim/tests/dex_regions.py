#!/usr/bin/env python3
"""After the smoke tour: the Dex's case never changes (docs/DEX_PLAN.md 2.1, 7).

Every dex_* shot must have the bezel, the title bar and the keys identical,
pixel for pixel, bar the clock's digits. And on leaving, the title bar must be
the system's black again. Standard library only.
"""
import sys
from pathlib import Path


def ppm(p):
    b = Path(p).read_bytes()
    parts, i = [], 0
    while len(parts) < 4:
        j = i
        while b[j] not in b" \n\t":
            j += 1
        parts.append(b[i:j])
        i = j + 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, b[i:]


FIXED = [
    ("the name", 0, 0, 71, 23),
    ("home and menu", 167, 0, 239, 23),
    ("the strip", 0, 24, 239, 59),
    ("the upper bezel's top", 0, 60, 239, 64),
    ("its left", 0, 65, 13, 194),
    ("its right", 226, 65, 239, 194),
    ("its bottom", 0, 195, 239, 211),
    ("the lower screen's edge", 0, 212, 239, 212),
    ("the space below it", 0, 286, 239, 291),
    ("the keys", 0, 292, 239, 319),
]
SHOTS = ["dex_no_save", "dex_greet", "dex_list", "dex_list_p2", "dex_list_caught", "dex_entry_caught",
         "dex_entry_back", "dex_where", "dex_entry_seen", "dex_talk_seen", "dex_entry_never",
         "dex_where_never", "dex_tall"]


def main(d):
    d = Path(d)
    w, h, base = ppm(d / (SHOTS[0] + ".ppm"))
    fails = 0
    for s in SHOTS[1:]:
        _, _, px = ppm(d / (s + ".ppm"))
        for name, x0, y0, x1, y1 in FIXED:
            for y in range(y0, y1 + 1):
                a, b = (y * w + x0) * 3, (y * w + x1 + 1) * 3
                if px[a:b] != base[a:b]:
                    print(f"dex_regions: {s}: {name} differs from {SHOTS[0]} at row {y}")
                    fails += 1
                    break
    _, _, left = ppm(d / "dex_left.ppm")
    if left[(2 * w + 60) * 3:(2 * w + 61) * 3] != b"\0\0\0":
        print("dex_regions: the title bar isn't black after leaving")
        fails += 1
    if fails:
        sys.exit(1)
    print(f"dex_regions: the case is identical on {len(SHOTS)} shots")


main(sys.argv[1])
