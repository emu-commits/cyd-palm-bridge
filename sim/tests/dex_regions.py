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


def striped(px, w, x0, x1, y0=40, y1=190):
    """Any column from x0 to x1 going dark, light, dark, light for 8 rows."""
    for x in range(x0, x1):
        run = 0
        for y in range(y0, y1):
            at = [sum(px[((y + k) * w + x) * 3:((y + k) * w + x) * 3 + 3]) for k in range(2)]
            prev = sum(px[((y - 1) * w + x) * 3:((y - 1) * w + x) * 3 + 3])
            flip = (at[0] < at[1]) != (prev < at[0]) and at[0] != at[1] and prev != at[0]
            run = run + 1 if flip else 0
            if run >= 8:
                return True
    return False


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
    # the system's menu isn't seen through the tube, and the screen round it still is
    # a TV; the effect's odd rows are the tell
    for shot in ("critters_menu", "critters_auto_menu"):
        # Critters gave the effect up: no scanlines anywhere, menu or game (DEX_PLAN.md 10)
        wm, hm, px = ppm(d / (shot + ".ppm"))
        if striped(px, wm, 12, 148) or striped(px, wm, 160, 222):
            print(f"dex_regions: {shot}: Critters has scanlines")
            fails += 1
    for shot in ("dex_menu",):
        wm, hm, px = ppm(d / (shot + ".ppm"))
        # the menu is Options/Power, Dex/TV effect, About: it ends near y 150
        # (one row shorter since Settings became a launcher tile), and below it
        # the TV screen rightly has its scanlines
        if striped(px, wm, 12, 148, 40, 146):
            print(f"dex_regions: {shot}: the menu has scanlines")
            fails += 1
        if not striped(px, wm, 160, 222):
            print(f"dex_regions: {shot}: no scanlines on the screen beside the menu")
            fails += 1
    if fails:
        sys.exit(1)
    print(f"dex_regions: the case is identical on {len(SHOTS)} shots")


main(sys.argv[1])
