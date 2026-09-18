#!/usr/bin/env python3
"""Emit the Guru launcher icon as an LVGL A8 image descriptor for palm_icons.c.

24x22 A8 (0 = transparent, 255 = ink), matching the other launcher icons so it
sits in the grid without a colour-format conversion.

Her third eye, lit. Built the way the Palm icons are built -- Memo Pad, To Do,
Date Book and Address are all a SOLID DISK with the subject knocked out of it in
white -- so the disk is the ink and the eye is the hole, not the other way round.
The iris is then painted back in, which is what stops it reading as a doughnut.

The aura is four arcs CONCENTRIC with the disk, not spokes radiating from it.
Spokes were tried first and read as eyelashes, which turns an enlightened eye
into a cartoon one. Arcs sit at a constant radius, so the eye looks surrounded
rather than fringed. Four with wide gaps beats eight with narrow ones at this
size: the gaps are what make it a halo instead of a second ring.

A galaxy-brain head was the first idea and does not survive 24x22 in one bit --
a profile only reads as a head if the brow, nose and chin each get pixels of
their own, and here they collapse into a blob. It wants about twice this canvas.

    python3 tools/gen_guru_icon.py       # prints the C to paste into palm_icons.c
"""
import math

W, H = 24, 22
CX, CY = 12, 11
DISK_R, IRIS_R = 7.4, 2.6          # the disk, and the iris painted back into it
EYE_HW, EYE_HH = 5.6, 3.3          # the eye's half-width and half-height
AURA_R = 9.8                       # the halo's radius, clear of the disk

g = [[0] * W for _ in range(H)]


def px(x, y, v=255):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = v


def disc(cx, cy, r, v=255):
    for j in range(H):
        for i in range(W):
            if (i - cx) ** 2 + (j - cy) ** 2 <= r * r:
                px(i, j, v)


def vesica(cx, cy, hw, hh, v=255):
    """a pointed oval -- an eye, not an ellipse: the corners come to a point"""
    for x in range(int(cx - hw), int(cx + hw) + 1):
        t = (x - cx) / float(hw)
        h = hh * (1.0 - t * t)
        for y in range(int(round(cy - h)), int(round(cy + h)) + 1):
            px(x, y, v)


def arc(cx, cy, r, a0, a1, v=255):
    """stepped by ANGLE, not by x -- stepping by x leaves holes on the steep part"""
    steps = max(8, int(math.radians(abs(a1 - a0)) * r * 4))
    for k in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * k / steps)
        px(round(cx + math.cos(a) * r), round(cy + math.sin(a) * r), v)


disc(CX, CY, DISK_R)                       # the disk is the ink
vesica(CX, CY, EYE_HW, EYE_HH, 0)          # the eye is the hole
disc(CX, CY, IRIS_R, 255)                  # ...with the iris painted back in
for start in (0, 90, 180, 270):            # the aura: four concentric arcs
    arc(CX, CY, AURA_R, start + 12, start + 78)

rows = [",".join(str(v) for v in r) for r in g]
print("/* the Guru's launcher icon -- tools/gen_guru_icon.py */")
print("static const uint8_t icon_guru_map[] = {%s};" % ",".join(rows))
print("const lv_image_dsc_t icon_guru = {")
print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
      ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (W, H, W))
print("  .data_size=%d,.data=icon_guru_map };" % (W * H))
