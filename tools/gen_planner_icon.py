#!/usr/bin/env python3
"""Emit the Planner launcher icon as an LVGL A8 image descriptor for palm_icons.c.

The Planner is To Do and Memo in one app, so its icon is both at once: a page of
notes on the Palm launcher's black disc, tilted like the Palm pages beside it,
whose top two lines are checkboxes (the first one ticked) and whose last lines
are plain text. 25x22 A8 like the Palm bitmap icons (0 = transparent, 255 = ink),
so ui.c recolours it the same way they are.

**The text below is the artwork.** It began as a page drawn at 8x, rotated 15
degrees and box-filtered down against a 50% ink threshold (a flat page and a
25-degree tilt were tried beside the To Do and Memo icons; 15 read best), and
is kept as art so it can be touched up by hand without Pillow.

    python3 tools/gen_planner_icon.py              # prints the C for palm_icons.c
    python3 tools/gen_planner_icon.py --preview    # the art as text
"""
import sys

W, H = 25, 22
ART = """\
.........######..........
.......##########........
.....#########...##......
....#######......###.....
...####...........###....
...#...........#..###....
..###..###.####...####...
..###..###........####...
..###..###.........###...
.####..........##..####..
.#####..###.###....####..
.#####..#.#........####..
..####..###.....##..##...
..#####.....####....##...
..#####..###........##...
...####.....###.....#....
...####...##.............
....####........####.....
.....###.....######......
......##.#########.......
........########.........
.........................
"""


def build():
    rows = ART.split()
    assert len(rows) == H and all(len(r) == W for r in rows), "art must be 25x22"
    return [[255 if c == '#' else 0 for c in r] for r in rows]


def main():
    g = build()
    if '--preview' in sys.argv:
        for row in g:
            print(''.join('#' if v else '.' for v in row))
        return
    vals = [v for row in g for v in row]
    print('static const uint8_t icon_planner_map[] = {')
    for i in range(0, len(vals), W):
        print('  ' + ','.join('0x%02x' % v for v in vals[i:i + W]) + ',')
    print('};')
    print('const lv_image_dsc_t icon_planner = {')
    print('  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,'
          '.w=%d,.h=%d,.stride=%d,.reserved_2=0},' % (W, H, W))
    print('  .data_size=%d,.data=icon_planner_map };' % (W * H))


if __name__ == '__main__':
    main()
