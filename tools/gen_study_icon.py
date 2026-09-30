#!/usr/bin/env python3
"""Emit the Study launcher icon as an LVGL A8 image descriptor for palm_icons.c.

A closed textbook knocked out of the Palm launcher's black disc: a white cover
with the spine's hinge drawn in, two title lines, and the page block showing
under the cover's bottom edge. Upright rather than tilted: the Planner's page
leans because Palm's Memo and To Do pages do, and a book on a shelf doesn't.
24x22 A8 like the other launcher icons (0 = transparent, 255 = ink), so ui.c
recolours it the same way.

**The text below is the artwork**, drawn at 1:1 on a disc of radius 10.7, and
kept as text so it can be touched up by hand without Pillow.

    python3 tools/gen_study_icon.py              # prints the C for palm_icons.c
    python3 tools/gen_study_icon.py --preview    # the art as text
"""
import sys

W, H = 24, 22
ART = """\
..........####..........
.......##########.......
......############......
....################....
....##..........####....
...###.#.........####...
..####.#.........#####..
..####.#.#####...#####..
..####.#.........#####..
.#####.#..###....######.
.#####.#.........######.
.#####.#.........######.
.#####.#.........######.
..####.#.........#####..
..####.#.........#####..
..####.###############..
...###..#........####...
....###..........###....
....################....
......############......
.......##########.......
..........####..........
"""


def build():
    rows = ART.split()
    assert len(rows) == H and all(len(r) == W for r in rows), "art must be 24x22"
    return [[255 if c == '#' else 0 for c in r] for r in rows]


def main():
    g = build()
    if '--preview' in sys.argv:
        for row in g:
            print(''.join('#' if v else '.' for v in row))
        return
    vals = [v for row in g for v in row]
    print('/* the Study launcher icon -- tools/gen_study_icon.py */')
    print('static const uint8_t icon_study_map[] = {')
    for i in range(0, len(vals), W):
        print('  ' + ','.join('0x%02x' % v for v in vals[i:i + W]) + ',')
    print('};')
    print('const lv_image_dsc_t icon_study = {')
    print('  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,'
          '.w=%d,.h=%d,.stride=%d,.reserved_2=0},' % (W, H, W))
    print('  .data_size=%d,.data=icon_study_map };' % (W * H))


if __name__ == '__main__':
    main()
