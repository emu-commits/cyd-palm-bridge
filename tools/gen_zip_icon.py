#!/usr/bin/env python3
"""Emit the Zip launcher icon as an LVGL A8 image descriptor for palm_icons.c.

The Games icons are 24x22 A8 (0 = transparent, 255 = ink) so they sit alongside
the Palm bitmap icons without a colour format conversion. Zip's icon is the game
itself in miniature: a snake path threading a grid, with a start dot on the first
cell and the tail leaving the last one.

    python3 tools/gen_zip_icon.py        # prints the C to paste into palm_icons.c
"""
W, H = 24, 22
g = [[0] * W for _ in range(H)]


def box(x, y, w, h):
    for j in range(y, y + h):
        for i in range(x, x + w):
            if 0 <= i < W and 0 <= j < H:
                g[j][i] = 255


def disc(cx, cy, r):
    for j in range(cy - r, cy + r + 1):
        for i in range(cx - r, cx + r + 1):
            if (i - cx) ** 2 + (j - cy) ** 2 <= r * r and 0 <= i < W and 0 <= j < H:
                g[j][i] = 255


# a boustrophedon snake: three 2px runs joined alternately left/right, which reads
# as "one line through every cell" even at 24x22.
BAND = 2
for y in (4, 10, 16):
    box(4, y, 16, BAND)          # the horizontal runs
box(18, 4, BAND, 8)              # right-hand turn (run 1 -> run 2)
box(4, 10, BAND, 8)              # left-hand turn  (run 2 -> run 3)

box(3, 3, 4, 4)                  # the '1' you start on (a fat node, not a stray dot)
box(18, 16, BAND, 4)             # the tail leaving the last cell

rows = [",".join(str(v) for v in r) for r in g]
data = ",".join(rows)
print("static const uint8_t icon_zip_map[] = {%s};" % data)
print("const lv_image_dsc_t icon_zip = {")
print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
      ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (W, H, W))
print("  .data_size=%d,.data=icon_zip_map };" % (W * H))
