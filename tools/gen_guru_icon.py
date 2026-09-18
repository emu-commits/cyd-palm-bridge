#!/usr/bin/env python3
"""Emit the Guru launcher icon as an LVGL A8 image descriptor for palm_icons.c.

24x22 A8 (0 = transparent, 255 = ink), matching the other post-Palm icons so it
sits in the launcher grid without a colour-format conversion.

It is her portrait reduced to a mark: the bob as a solid ellipse, the face
knocked out of it, and the third eye above the pair -- the one feature that is
unmistakably hers at any size. A literal checklist was the obvious alternative
and was rejected: the To Do List icon is already a checklist, and two apps with
the same mark is worse than an app whose mark needs one look to learn.

The face is knocked OUT of the hair rather than drawn on top of it, which is why
the two ellipses share a centre column but not a centre row -- the hair sits a
pixel high so it reads as a fringe above the brow rather than a helmet.

    python3 tools/gen_guru_icon.py       # prints the C to paste into palm_icons.c
"""
W, H = 24, 22
g = [[0] * W for _ in range(H)]

CX = 12                       # both ellipses share this column
HAIR_CY, HAIR_RX, HAIR_RY = 11, 9.4, 10.4
FACE_CY, FACE_RX, FACE_RY = 12, 5.5, 7.5


def ellipse(cx, cy, rx, ry, v):
    for j in range(H):
        for i in range(W):
            if ((i - cx) ** 2) / (rx * rx) + ((j - cy) ** 2) / (ry * ry) <= 1.0:
                g[j][i] = v


def box(x, y, w, h):
    for j in range(y, y + h):
        for i in range(x, x + w):
            if 0 <= i < W and 0 <= j < H:
                g[j][i] = 255


ellipse(CX, HAIR_CY, HAIR_RX, HAIR_RY, 255)    # the bob
ellipse(CX, FACE_CY, FACE_RX, FACE_RY, 0)      # ...with the face cut out of it

box(9, 13, 2, 2)                               # eyes
box(13, 13, 2, 2)
box(11, 9, 2, 2)                               # the third
box(10, 17, 4, 1)                              # mouth

rows = [",".join(str(v) for v in r) for r in g]
data = ",".join(rows)
print("/* the Guru's launcher icon -- tools/gen_guru_icon.py */")
print("static const uint8_t icon_guru_map[] = {%s};" % data)
print("const lv_image_dsc_t icon_guru = {")
print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
      ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (W, H, W))
print("  .data_size=%d,.data=icon_guru_map };" % (W * H))
