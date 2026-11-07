#!/usr/bin/env python3
"""Emit the Knowledge launcher icon as an LVGL A8 image descriptor for palm_icons.c.

A small tree, smiling: a lobed canopy in solid ink with two eyes and a smile
knocked out of it, a trunk that flares at the root, and the ground it stands
on. A tree because Knowledge is a graph that grows, each step a branch off the
one before (KNOWLEDGEQUEST_PLAN.md). Ink on nothing rather than knocked out of
the black disc as Study's book is: drawn white in a disc, at 1x, it read as a
round face, not a tree. 24x22 A8 like the other launcher icons (0 =
transparent, 255 = ink), so ui.c recolours it the same way.

**The text below is the artwork**, drawn at 1:1 and symmetric about the
middle, kept as text so it can be touched up by hand without Pillow.

    python3 tools/gen_knowledge_icon.py              # prints the C for palm_icons.c
    python3 tools/gen_knowledge_icon.py --preview    # the art as text
"""
import sys

W, H = 24, 22
ART = """\
........................
..........####..........
.........######.........
......##.######.##......
.....##############.....
....################....
....################....
....################....
...######.####.######...
..#######.####.#######..
..####################..
..########.##.########..
..#########..#########..
...##################...
.....##############.....
..........####..........
..........####..........
..........####..........
..........####..........
.........######.........
........########........
.....##############.....
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
    print('/* the Knowledge launcher icon -- tools/gen_knowledge_icon.py */')
    print('static const uint8_t icon_knowledge_map[] = {')
    for i in range(0, len(vals), W):
        print('  ' + ','.join('0x%02x' % v for v in vals[i:i + W]) + ',')
    print('};')
    print('const lv_image_dsc_t icon_knowledge = {')
    print('  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,'
          '.w=%d,.h=%d,.stride=%d,.reserved_2=0},' % (W, H, W))
    print('  .data_size=%d,.data=icon_knowledge_map };' % (W * H))


if __name__ == '__main__':
    main()
