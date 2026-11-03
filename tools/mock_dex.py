#!/usr/bin/env python3
"""Draw the Dex mock-ups in docs/img/dex/ (design aid, not product code).

    python3 tools/mock_dex.py --tuxemon <checkout>

Everything is 240x320, the device's size, drawn with the firmware's Palm fonts.
The bezel below is one fixed drawing used by every screen, as the app must be:
only the two subscreens' contents change. See docs/DEX_PLAN.md section 2.
"""
import argparse
import os
import re
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs/img/dex")

RED, RED_HI = (123, 28, 41), (156, 48, 60)
GREY, BLACK, WHITE = (214, 215, 214), (0, 0, 0), (255, 255, 255)
CREAM, GREEN, INK = (247, 243, 231), (156, 189, 90), (24, 48, 24)
KEY, KEY_HI = (66, 112, 189), (99, 146, 214)
# The upper screen is a TV (DEX_PLAN 2.4). Cards on it keep the cream as their own.
TV, TV_INK, TV_DIM = (38, 70, 140), (244, 244, 236), (170, 192, 232)
TV_SIL, TV_WELL = (12, 24, 60), (36, 36, 40)
TV_RADIUS, TV_BLOOM, TV_SCAN, TV_VIGNETTE, TV_LIFT = 7, 0.30, 0.80, 0.35, (5, 6, 9)

# The fixed geometry: x0, y0, x1, y1, inclusive.
UPPER = (15, 66, 224, 193)   # 210 x 128: the tallest critter at 2x just fits
LOWER = (7, 213, 232, 285)   # 226 x 73: five lines of text
# The title bar keeps the system's layout and dimensions, as hardware (y 0-23).
CLOCK = (72, 2, 166, 21)     # a small green screen behind the clock
HOME, MENU = (169, 1, 211, 20), (214, 1, 238, 20)
KEYS = ((6, 44, "<"), (50, 118, "List"), (124, 190, "Talk"), (196, 233, ">"))


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


def chamfer(d, x0, y0, x1, y1, fill, c=3):
    pts = [(x0 + c, y0), (x1 - c, y0), (x1, y0 + c), (x1, y1 - c), (x1 - c, y1), (x0 + c, y1), (x0, y1 - c), (x0, y0 + c)]
    d.polygon(pts, fill=fill, outline=BLACK)


def bezel():
    """The gadget: identical on every screen, including the key labels."""
    im = Image.new("RGB", (240, 320), RED)
    top = Image.open(os.path.join(OUT, "bezel_top.png")).convert("RGB")
    im.paste(top.crop((0, 24, 240, 56)), (0, 24))          # the lens and lights
    d = ImageDraw.Draw(im)
    d.line((0, 23, 239, 23), fill=RED_HI)
    chamfer(d, *CLOCK, GREEN, 2)
    chamfer(d, *HOME, KEY, 2)
    chamfer(d, *MENU, KEY, 2)
    d.line((HOME[0] + 2, HOME[1] + 1, HOME[2] - 2, HOME[1] + 1), fill=KEY_HI)
    d.line((MENU[0] + 2, MENU[1] + 1, MENU[2] - 2, MENU[1] + 1), fill=KEY_HI)
    # The name, clock, Home and Menu glyphs are the system's, exactly where they
    # sit on its title bar; only their colours change.
    t, p = top.load(), im.load()
    for y in range(2, 20):
        for x in range(240):
            if t[x, y] == WHITE:
                p[x, y] = CREAM if x < 30 else INK if x < 160 else WHITE
    d.line((0, 56, 239, 56), fill=RED_HI)
    chamfer(d, 6, 60, 233, 206, GREY, 4)
    d.rectangle((14, 65, 225, 194), fill=TV_WELL)
    d.rectangle(UPPER, fill=TV)
    d.rectangle((26, 197, 32, 203), fill=(214, 41, 41), outline=BLACK)   # the light
    chamfer(d, 6, 212, 233, 286, GREEN, 3)
    for x0, x1, label in KEYS:
        chamfer(d, x0, 292, x1, 314, KEY, 2)
        d.line((x0 + 2, 294, x1 - 2, 294), fill=KEY_HI)
        cx, cy = (x0 + x1) // 2, 303
        if label == "<":
            d.polygon([(cx - 4, cy), (cx + 3, cy - 6), (cx + 3, cy + 6)], fill=WHITE)
        elif label == ">":
            d.polygon([(cx + 4, cy), (cx - 3, cy - 6), (cx - 3, cy + 6)], fill=WHITE)
        else:
            BOLD.draw(im, cx - BOLD.width(label) // 2, 296, label, WHITE)
    return im, d


def mark(d, x, y, caught):
    """The Dex's own marks, 11x11: caught is a solid gold diamond, seen a hollow one."""
    pts = [(x + 5, y), (x + 10, y + 5), (x + 5, y + 10), (x, y + 5)]
    if caught:
        d.polygon(pts, fill=(250, 196, 48), outline=TV_SIL)
        d.line((x + 3, y + 4, x + 4, y + 3), fill=WHITE)   # a glint
    else:
        d.polygon(pts, fill=TV, outline=TV_INK)


def silhouette(s, col=TV_SIL):
    a = s.getchannel("A").point(lambda v: 255 if v else 0)
    o = Image.new("RGBA", s.size, col + (255,))
    o.putalpha(a)
    return o


def tube(im):
    """The TV effect, on the finished upper screen only: a soft sideways glow,
    dark odd rows and dark corners, each pixel reading only its own row. Then
    the tube's rounded corners, with the dark well showing outside them."""
    w, h = UPPER[2] - UPPER[0] + 1, UPPER[3] - UPPER[1] + 1
    src = im.crop((UPPER[0], UPPER[1], UPPER[2] + 1, UPPER[3] + 1))
    a, out = src.load(), Image.new("RGB", (w, h))
    o = out.load()
    for y in range(h):
        scan = TV_SCAN if y % 2 else 1.0
        dy = (y - (h - 1) / 2) / (h / 2)
        for x in range(w):
            dx = (x - (w - 1) / 2) / (w / 2)
            f = scan * (1 - TV_VIGNETTE * min(dx ** 4 + dy ** 4, 1))
            lf, rt = a[max(x - 1, 0), y], a[min(x + 1, w - 1), y]
            o[x, y] = tuple(min(255, int((v + TV_BLOOM * (l + r) / 2 * (255 - v) / 255) * f + k))
                            for v, l, r, k in zip(a[x, y], lf, rt, TV_LIFT))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), TV_RADIUS, fill=255)
    d = ImageDraw.Draw(im)
    d.rectangle(UPPER, fill=TV_WELL)
    im.paste(out, UPPER[:2], mask)


def lower_text(im, lines, x=None, w=None):
    """lines: (text, bold) pairs in the lower screen, wrapped."""
    x = LOWER[0] + 5 if x is None else x
    w = LOWER[2] - 4 - x if w is None else w
    y = LOWER[1] + 1
    out = []
    for text, bold in lines:
        f = BOLD if bold else PALM
        # a line that fits keeps its spacing; only longer ones are wrapped
        out += [(f, text)] if f.width(text) <= w else [(f, s) for s in f.wrap(text, w)]
    assert len(out) <= 5, "lower screen overflows: %r" % out
    for f, s in out:
        f.draw(im, x, y, s, INK)
        y += 14


class Sheets:
    def __init__(self, tux):
        self.tux = tux

    def __call__(self, slug):
        return Image.open(os.path.join(self.tux, "mods/tuxemon/gfx/sprites/battle", slug + "-sheet.png")).convert("RGBA")


def list_screen(sheets):
    im, d = bezel()
    rows = [(1, "agnite", "Agnite", 2), (2, "agnidon", "Agnidon", 2), (3, None, "???", 0),
            (4, "anoleaf", "Anoleaf", 2), (5, "agnidon", "???", 1)]
    y = UPPER[1] + 4
    for n, slug, name, state in rows:
        PALM.draw(im, 19, y + 5, "%03d" % n, TV_INK)
        if slug:
            ic = sheets(slug).crop((0, 64, 24, 88))
            if state == 1:
                ic = silhouette(ic)
            im.paste(ic, (44, y), ic)
        PALM.draw(im, 74, y + 5, name, TV_INK)
        if state:
            mark(d, 202, y + 7, state == 2)
        y += 24
    d.rectangle((216, UPPER[1] + 1, 222, UPPER[3] - 1), fill=WHITE, outline=BLACK)
    d.rectangle((217, UPPER[1] + 2, 221, UPPER[1] + 19), fill=BLACK)
    return im


def list_info(im):
    lower_text(im, [("SEEN 11    CAUGHT 6 / 33", True), ("Showing all, page 1 of 7.", False),
                    ("Tap a critter to open it.", False), ("Press List for caught only.", False)])


def entry(sheets, slug, x=0, state=2):
    im, d = bezel()
    s = sheets(slug).crop((x, 0, x + 64, 64))
    if state == 1:
        s = silhouette(s)
    if state:
        s2 = s.resize((128, 128), Image.NEAREST)
        im.paste(s2, (UPPER[0], UPPER[1]), s2)
    else:
        BIG.draw(im, 70, 115, "?", TV_INK)
    tx = 147
    BOLD.draw(im, tx, 74, "No. 001" if state == 2 else "No. 005" if state == 1 else "No. 006", TV_INK)
    BOLD.draw(im, tx, 90, "Agnite" if state == 2 else "???", TV_INK)
    if state == 2:
        d.rectangle((tx, 108, tx + 32, 123), fill=(224, 96, 48), outline=TV_INK)
        BOLD.draw(im, tx + 4, 109, "FIRE", WHITE)
        PALM.draw(im, tx, 128, "Lv 24 to", TV_INK)
        PALM.draw(im, tx, 142, "Agnidon", TV_INK)
        mark(d, tx, 161, True)
        PALM.draw(im, tx + 13, 158, "caught", TV_INK)
        PALM.draw(im, tx, 176, "Back" if x else "Front", TV_DIM)
    else:
        PALM.draw(im, tx, 108, "seen" if state == 1 else "never seen", TV_INK)
    return im


def entry_info(im, state=2):
    if state == 2:
        lower_text(im, [("FALSE DRAGON", True), ("HT 0.8 m     WT 24.0 kg", False),
                        ("It is playful, but must be taught early on to keep its fire in check.", False)])
    else:
        lower_text(im, [("???", True), ("HT ???     WT ???", False), ("???", False)])


def speaker(im, d, colour, head):
    """He stands against the right of the upper screen; what he's talking about
    is on a card to his left, like a news report's. Returns the card's inside."""
    im.paste(colour, (UPPER[2] + 1 - colour.width, UPPER[3] + 1 - colour.height))
    x0, y0, x1, y1 = UPPER[0] + 3, UPPER[1] + 3, UPPER[2] - colour.width - 2, UPPER[3] - 3
    d.rectangle((x0, y0, x1, y1), fill=CREAM, outline=BLACK)   # a card has its own background
    d.rectangle((x0 + 1, y0 + 1, x1 - 1, y0 + 16), fill=RED)
    BOLD.draw(im, (x0 + x1 - BOLD.width(head)) // 2, y0 + 2, head, WHITE)
    return x0 + 1, y0 + 17, x1 - 1, y1 - 1


def card_foot(im, d, box, a, b):
    """The card's lower third, as a report captions its picture."""
    x0, y0, x1, y1 = box
    d.rectangle((x0, y1 - 31, x1, y1), fill=(40, 40, 48))
    BOLD.draw(im, (x0 + x1 - BOLD.width(a)) // 2, y1 - 30, a, WHITE)
    PALM.draw(im, (x0 + x1 - PALM.width(b)) // 2, y1 - 15, b, WHITE)


GREET = "Six of thirty-three. Something leafy keeps darting through Route 2 at dusk. Have a look."
WHERE = "Your Agnite came from Dr Wren's lab. In the wild? Only the Old Keep's grass, and only after Ivy's rematch."


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--tuxemon", required=True)
    args = a.parse_args()
    sheets = Sheets(args.tuxemon)
    colour = Image.open(os.path.join(OUT, "speaker/colour_102.png")).convert("RGB")

    def save(im, n):
        tube(im)
        im.save(os.path.join(OUT, n), optimize=True)

    im, d = bezel()                                        # opening
    box = speaker(im, d, colour, "YOUR DEX")
    x0, y0, x1, y1 = box
    BOLD.draw(im, x0 + 6, y0 + 6, "CAUGHT")
    BIG.draw(im, x0 + 6, y0 + 20, "6")
    PALM.draw(im, x0 + 26, y0 + 34, "of 33")
    BOLD.draw(im, x0 + 6, y0 + 58, "SEEN")
    BIG.draw(im, x0 + 6, y0 + 72, "11")
    lower_text(im, [(GREET, False)])
    save(im, "dex_2_greet.png")

    im = list_screen(sheets)
    list_info(im)
    save(im, "dex_3_list.png")

    im = entry(sheets, "agnite")
    entry_info(im)
    save(im, "dex_4_caught.png")

    im = entry(sheets, "agnite", 64)
    entry_info(im)
    save(im, "dex_5_back.png")

    im = entry(sheets, "agnidon", 0, 1)
    entry_info(im, 1)
    save(im, "dex_6_seen.png")

    im = entry(sheets, "agnite", 0, 0)
    entry_info(im, 0)
    save(im, "dex_7_never.png")

    im, d = bezel()                                        # where
    box = speaker(im, d, colour, "FIELD REPORT")
    x0, y0, x1, y1 = box
    s = sheets("agnite").crop((0, 0, 64, 64))
    im.paste(s, ((x0 + x1 - 64) // 2, y0 + 3), s)
    card_foot(im, d, box, "Agnite", "No. 001")
    lower_text(im, [(WHERE, False)])
    save(im, "dex_8_where.png")


if __name__ == "__main__":
    main()
