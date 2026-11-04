#!/usr/bin/env python3
"""The firmware's Palm fonts, read from their C sources, for tools that must
know how text will wrap (mkgame's description check, the Dex's fit gate) or draw
it (the mock-ups). Standard library only."""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLACK = (0, 0, 0)


class Font:
    def __init__(self, name):
        src = open(os.path.join(ROOT, "firmware/main", name + ".c")).read()
        body = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
        self.bits = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]{2}", body))
        self.g = [tuple(int(v) for v in m) for m in re.findall(
            r"\.bitmap_index=(\d+),\.adv_w=(\d+),\.box_w=(\d+),\.box_h=(\d+),\.ofs_x=(-?\d+),\.ofs_y=(-?\d+)", src)]
        self.h = int(re.search(r"\.line_height=(\d+)", src).group(1))
        self.base = int(re.search(r"\.base_line=(\d+)", src).group(1))

    def width(self, s):
        return sum(self.g[ord(c) - 31][1] for c in s) // 16

    def draw(self, im, x, y, s, col=BLACK):
        p = im.load()
        for c in s:
            i, adv, w, h, ox, oy = self.g[ord(c) - 31]
            top = y + self.h - self.base - h - oy
            n = i * 8
            for r in range(h):
                for k in range(w):
                    if self.bits[n >> 3] >> (7 - (n & 7)) & 1:
                        p[x + ox + k, top + r] = col
                    n += 1
            x += adv // 16

    def wrap(self, s, w):
        lines, cur = [], ""
        for word in s.split():
            t = (cur + " " + word).strip()
            if self.width(t) > w:
                lines.append(cur)
                cur = word
            else:
                cur = t
        return lines + [cur]


PALM, BOLD, BIG = Font("lv_font_palm"), Font("lv_font_palm_bold"), Font("lv_font_palm_bold_2x")
