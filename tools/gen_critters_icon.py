#!/usr/bin/env python3
"""Emit the Critters icon as an LVGL A8 image descriptor for palm_icons.c.

The Games icons are 24x22 A8 (0 = transparent, 255 = ink), like Zip's
(tools/gen_zip_icon.py). Critters' icon is a creature of our own drawing: a
round body with two pointed ears, eyes knocked out of the ink, and two feet.
Nothing from Tuxemon or anyone else's art.

    python3 tools/gen_critters_icon.py           # prints the C to paste into palm_icons.c
    python3 tools/gen_critters_icon.py --show    # the picture, in text
"""
import sys

W, H = 24, 22
g = [[0] * W for _ in range(H)]


def ellipse(cx, cy, rx, ry, v=255):
    for j in range(H):
        for i in range(W):
            if ((i + 0.5 - cx) / rx) ** 2 + ((j + 0.5 - cy) / ry) ** 2 <= 1:
                g[j][i] = v


def triangle(a, b, c):
    def side(p, q, r):
        return (r[0] - p[0]) * (q[1] - p[1]) - (r[1] - p[1]) * (q[0] - p[0])
    for j in range(H):
        for i in range(W):
            p = (i + 0.5, j + 0.5)
            s1, s2, s3 = side(a, b, p), side(b, c, p), side(c, a, p)
            if (s1 >= 0 and s2 >= 0 and s3 >= 0) or (s1 <= 0 and s2 <= 0 and s3 <= 0):
                g[j][i] = 255


triangle((5, 1), (4, 10), (10, 7))      # the ears
triangle((19, 1), (20, 10), (14, 7))
ellipse(12, 12, 9, 8)                   # the body
ellipse(8.5, 11, 1.2, 1.9, 0)           # the eyes
ellipse(15.5, 11, 1.2, 1.9, 0)
for i in range(11, 13):                 # a small mouth
    g[15][i] = 0
for x in (6, 15):                       # the feet
    for j in (19, 20):
        for i in range(x, x + 3):
            g[j][i] = 255

if "--show" in sys.argv:
    print("\n".join("".join("#" if v else "." for v in r) for r in g))
    sys.exit(0)
data = ",".join(",".join(str(v) for v in r) for r in g)
print("static const uint8_t icon_critters_map[] = {%s};" % data)
print("const lv_image_dsc_t icon_critters = {")
print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
      ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (W, H, W))
print("  .data_size=%d,.data=icon_critters_map };" % (W * H))
