#!/usr/bin/env python3
"""Draw Critters' rooms: tools/game_art/interior.png, a 128 x 128 tileset.

Drawn for this project and given to the public domain (CC0), because the
interiors Tuxemon has are built from `core_indoor_walls` and
`core_indoor_floors`, which its ATTRIBUTIONS.md says come partly from
XYG-licensed sets, and the game takes clean licences only (GAME_PLAN.md §13
Q6). The house interiors (tools/game_maps/*_home.json and the rest) are laid
out from these tiles.

The sheet is a grid of 16 x 16 cells, 8 across, read by mkgame.py like any
other tileset (a layout names a tile as "i:<n>", n = row * 8 + column, or a
stamp by [column, row, width, height]). PIECES says where each piece is.

    python3 tools/gen_interior_sheet.py            # writes tools/game_art/interior.png
    python3 tools/gen_interior_sheet.py --check    # the PNG matches this drawing, pixel for pixel
"""
import sys
from pathlib import Path

ART = Path(__file__).resolve().parent / "game_art"
W = H = 128

K = (52, 36, 30)                    # outline
FLOOR = ((204, 156, 104), (176, 128, 80), (224, 182, 130))     # plank, seam, lit
CORNICE = ((110, 72, 44), (150, 104, 64))
PAPER = ((236, 222, 188), (222, 204, 166))                      # wallpaper, its stripe
SKIRT = ((150, 104, 64), (110, 72, 44), (84, 56, 36))
WOOD = ((150, 98, 58), (112, 70, 40), (188, 132, 84))           # furniture: face, shade, lit
GLASS = ((150, 200, 232), (204, 232, 248))
WHITE = (244, 242, 232)
GREY = ((168, 168, 176), (124, 124, 136), (204, 204, 212))
RUG = ((176, 62, 52), (132, 42, 40), (222, 180, 84))
MAT = ((92, 136, 76), (64, 100, 54))
LEAF = ((72, 152, 76), (44, 108, 54), (120, 192, 100))
POT = ((188, 98, 60), (140, 68, 40))
BLANKET = ((76, 116, 188), (52, 82, 144), (116, 156, 216))
BOOKS = [(176, 56, 48), (60, 120, 72), (64, 92, 168), (212, 172, 64), (120, 72, 140), (196, 120, 60)]

# name: (column, row, width, height), in cells
PIECES = {
    "floor": (0, 0, 1, 1), "floor_b": (1, 0, 1, 1), "wall": (2, 0, 1, 1), "wall_low": (3, 0, 1, 1),
    "window": (4, 0, 1, 2), "picture": (5, 0, 1, 2), "clock": (6, 0, 1, 2), "mat": (7, 0, 1, 1),
    "chair": (7, 1, 1, 1),
    "bookshelf": (0, 2, 2, 2), "bed": (2, 2, 1, 2), "table": (3, 2, 2, 2), "plant": (5, 2, 1, 2),
    "chair_back": (6, 2, 1, 1), "crate": (7, 2, 1, 1), "barrel": (6, 3, 1, 1), "stool": (7, 3, 1, 1),
    "rug": (0, 4, 3, 2), "counter": (3, 4, 2, 2), "desk": (5, 4, 2, 2), "lamp": (7, 4, 1, 2),
    "machine": (0, 6, 2, 2), "fireplace": (2, 6, 2, 2), "cabinet": (4, 6, 2, 2),
}


class Canvas:
    def __init__(self):
        self.px = [[None] * W for _ in range(H)]

    def set(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):          # inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, c)

    def box(self, x0, y0, x1, y1, fill, edge=K):
        self.rect(x0, y0, x1, y1, edge)
        self.rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill)

    def ellipse(self, cx, cy, rx, ry, c):
        for y in range(cy - ry, cy + ry + 1):
            for x in range(cx - rx, cx + rx + 1):
                if ((x - cx) / (rx + 0.5)) ** 2 + ((y - cy) / (ry + 0.5)) ** 2 <= 1:
                    self.set(x, y, c)


def origin(name):
    c, r, _, _ = PIECES[name]
    return c * 16, r * 16


def floor(cv, name, shift):
    ox, oy = origin(name)
    for y in range(16):
        plank = y // 4
        for x in range(16):
            c = FLOOR[0]
            if y % 4 == 3:
                c = FLOOR[1]
            elif y % 4 == 0 and (x + plank * 3) % 7 == 0:
                c = FLOOR[2]
            if (x - (plank * 6 + shift)) % 16 == 0 and y % 4 != 3:
                c = FLOOR[1]
            cv.set(ox + x, oy + y, c)


def wall(cv, ox, oy):
    """the wall's upper cell: a cornice, then striped paper"""
    for y in range(16):
        for x in range(16):
            if y < 2:
                c = CORNICE[0]
            elif y == 2:
                c = CORNICE[1]
            else:
                c = PAPER[1] if x % 4 == 1 else PAPER[0]
            cv.set(ox + x, oy + y, c)


def wall_low(cv, ox, oy):
    """the lower cell: paper, then the skirting board and its shadow"""
    for y in range(16):
        for x in range(16):
            if y < 10:
                c = PAPER[1] if x % 4 == 1 else PAPER[0]
            elif y == 10:
                c = SKIRT[0]
            elif y < 15:
                c = SKIRT[1] if y != 12 else SKIRT[0]
            else:
                c = SKIRT[2]
            cv.set(ox + x, oy + y, c)


def window(cv):
    ox, oy = origin("window")
    wall(cv, ox, oy)
    wall_low(cv, ox, oy + 16)
    cv.box(ox + 2, oy + 5, ox + 13, oy + 23, WHITE)
    cv.rect(ox + 3, oy + 6, ox + 12, oy + 22, GLASS[0])
    for i in range(5):                              # a glint
        cv.set(ox + 4 + i, oy + 11 - i, GLASS[1])
        cv.set(ox + 5 + i, oy + 11 - i, GLASS[1])
    cv.rect(ox + 7, oy + 6, ox + 8, oy + 22, WHITE)  # the frame's cross
    cv.rect(ox + 3, oy + 14, ox + 12, oy + 14, WHITE)
    cv.rect(ox + 1, oy + 24, ox + 14, oy + 25, WHITE)     # the sill
    cv.rect(ox + 1, oy + 26, ox + 14, oy + 26, PAPER[1])


def picture(cv):
    ox, oy = origin("picture")
    wall(cv, ox, oy)
    wall_low(cv, ox, oy + 16)
    cv.box(ox + 3, oy + 5, ox + 12, oy + 14, WOOD[0])
    cv.rect(ox + 4, oy + 6, ox + 11, oy + 13, GLASS[0])
    cv.rect(ox + 4, oy + 11, ox + 11, oy + 13, LEAF[0])
    cv.rect(ox + 6, oy + 10, ox + 9, oy + 10, LEAF[0])
    cv.set(ox + 10, oy + 7, RUG[2])


def clock(cv):
    ox, oy = origin("clock")
    cv.box(ox + 3, oy + 2, ox + 12, oy + 30, WOOD[0])
    cv.rect(ox + 4, oy + 3, ox + 11, oy + 3, WOOD[2])
    cv.ellipse(ox + 7, oy + 8, 3, 3, WHITE)
    cv.set(ox + 7, oy + 6, K); cv.set(ox + 7, oy + 7, K); cv.set(ox + 8, oy + 8, K)
    cv.box(ox + 5, oy + 14, ox + 10, oy + 26, WOOD[1])
    cv.rect(ox + 7, oy + 15, ox + 8, oy + 21, RUG[2])
    cv.ellipse(ox + 7, oy + 23, 1, 1, RUG[2])
    cv.rect(ox + 3, oy + 31, ox + 12, oy + 31, SKIRT[2])


def mat(cv):
    floor(cv, "mat", 3)
    ox, oy = origin("mat")
    cv.box(ox + 2, oy + 3, ox + 13, oy + 13, MAT[0], MAT[1])
    for x in range(ox + 4, ox + 12, 2):
        cv.rect(x, oy + 5, x, oy + 11, MAT[1])


def chair(cv, name, back):
    ox, oy = origin(name)
    if back:      # seen from behind: the back is nearest
        cv.box(ox + 3, oy + 1, ox + 12, oy + 6, WOOD[0])
        cv.box(ox + 3, oy + 6, ox + 12, oy + 13, WOOD[1])
        cv.rect(ox + 4, oy + 7, ox + 11, oy + 7, WOOD[0])
    else:
        cv.box(ox + 3, oy + 1, ox + 12, oy + 8, WOOD[1])
        cv.rect(ox + 4, oy + 2, ox + 11, oy + 2, WOOD[0])
        cv.box(ox + 2, oy + 8, ox + 13, oy + 12, WOOD[0])
        cv.rect(ox + 3, oy + 9, ox + 12, oy + 9, WOOD[2])
    cv.rect(ox + 3, oy + 13, ox + 4, oy + 15, K)
    cv.rect(ox + 11, oy + 13, ox + 12, oy + 15, K)


def bookshelf(cv):
    ox, oy = origin("bookshelf")
    cv.box(ox + 1, oy + 2, ox + 30, oy + 31, WOOD[1])
    cv.rect(ox + 2, oy + 3, ox + 29, oy + 3, WOOD[2])
    k = 0
    for shelf, (y0, y1) in enumerate(((5, 12), (15, 22), (25, 29))):
        x = ox + 3
        while x < ox + 28:
            w = 2 + (k * 7 + shelf) % 3
            if x + w > ox + 28:
                w = ox + 28 - x
            top = y0 + (k * 5) % 3
            col = BOOKS[(k * 5 + shelf * 2) % len(BOOKS)]
            cv.rect(x, oy + top, x + w - 1, oy + y1, col)
            cv.rect(x, oy + top, x, oy + y1, K)
            k += 1
            x += w + (1 if k % 4 == 0 else 0)
        cv.rect(ox + 2, oy + y1 + 1, ox + 29, oy + y1 + 2, WOOD[0])


def bed(cv):
    ox, oy = origin("bed")
    cv.box(ox + 1, oy + 2, ox + 14, oy + 9, WOOD[0])          # the headboard
    cv.rect(ox + 2, oy + 3, ox + 13, oy + 3, WOOD[2])
    cv.box(ox + 1, oy + 8, ox + 14, oy + 30, BLANKET[0])
    cv.box(ox + 3, oy + 9, ox + 12, oy + 14, WHITE)            # the pillow
    cv.rect(ox + 2, oy + 16, ox + 13, oy + 17, BLANKET[2])     # the fold
    cv.rect(ox + 2, oy + 18, ox + 13, oy + 18, BLANKET[1])
    cv.rect(ox + 2, oy + 29, ox + 13, oy + 29, BLANKET[1])
    cv.rect(ox + 1, oy + 31, ox + 14, oy + 31, SKIRT[2])


def table(cv):
    ox, oy = origin("table")
    cv.box(ox + 2, oy + 6, ox + 29, oy + 22, WOOD[0])
    cv.rect(ox + 3, oy + 7, ox + 28, oy + 8, WOOD[2])
    cv.rect(ox + 3, oy + 19, ox + 28, oy + 21, WOOD[1])        # the edge facing us
    for x in (ox + 4, ox + 26):
        cv.rect(x, oy + 23, x + 1, oy + 28, K)
    cv.ellipse(ox + 15, oy + 12, 4, 2, WHITE)                  # a bowl of fruit
    cv.set(ox + 13, oy + 11, RUG[0]); cv.set(ox + 15, oy + 10, BOOKS[3]); cv.set(ox + 17, oy + 11, LEAF[0])
    cv.rect(ox + 12, oy + 14, ox + 18, oy + 14, GREY[1])


def plant(cv):
    ox, oy = origin("plant")
    for cx, cy, rx, ry, c in ((7, 10, 6, 6, LEAF[1]), (5, 8, 4, 4, LEAF[0]), (10, 12, 4, 4, LEAF[0]),
                              (7, 15, 5, 3, LEAF[0]), (5, 6, 1, 1, LEAF[2]), (10, 10, 1, 1, LEAF[2])):
        cv.ellipse(ox + cx, oy + cy, rx, ry, c)
    cv.box(ox + 3, oy + 19, ox + 12, oy + 30, POT[0])
    cv.rect(ox + 2, oy + 19, ox + 13, oy + 21, POT[1])
    cv.rect(ox + 3, oy + 31, ox + 12, oy + 31, SKIRT[2])


def crate(cv):
    ox, oy = origin("crate")
    cv.box(ox + 1, oy + 2, ox + 14, oy + 15, WOOD[0])
    cv.rect(ox + 2, oy + 3, ox + 13, oy + 4, WOOD[2])
    for i in range(10):
        cv.set(ox + 3 + i, oy + 5 + i, WOOD[1])
        cv.set(ox + 12 - i, oy + 5 + i, WOOD[1])


def barrel(cv):
    ox, oy = origin("barrel")
    cv.ellipse(ox + 7, oy + 8, 6, 7, K)
    cv.ellipse(ox + 7, oy + 8, 5, 6, WOOD[0])
    cv.ellipse(ox + 7, oy + 4, 4, 2, WOOD[2])
    for y in (7, 12):
        cv.rect(ox + 2, oy + y, ox + 12, oy + y, GREY[1])


def stool(cv):
    ox, oy = origin("stool")
    cv.ellipse(ox + 7, oy + 6, 5, 3, K)
    cv.ellipse(ox + 7, oy + 6, 4, 2, WOOD[2])
    for x in (ox + 4, ox + 10):
        cv.rect(x, oy + 9, x + 1, oy + 15, WOOD[1])


def rug(cv):
    ox, oy = origin("rug")
    cv.rect(ox, oy, ox + 47, oy + 31, RUG[2])
    cv.rect(ox + 2, oy + 2, ox + 45, oy + 29, RUG[0])
    cv.rect(ox + 4, oy + 4, ox + 43, oy + 27, RUG[1])
    cv.rect(ox + 5, oy + 5, ox + 42, oy + 26, RUG[0])
    for cx in (12, 24, 36):                              # diamonds
        for d in range(6):
            for s in (-1, 1):
                cv.set(ox + cx + s * d, oy + 10 + d, RUG[2])
                cv.set(ox + cx + s * d, oy + 21 - d, RUG[2])


def counter(cv):
    """a kitchen counter: a pot on the stove, cupboards below"""
    ox, oy = origin("counter")
    cv.box(ox + 1, oy + 10, ox + 30, oy + 15, GREY[2], GREY[1])     # the top
    cv.box(ox + 1, oy + 15, ox + 30, oy + 31, WOOD[0])
    cv.rect(ox + 15, oy + 16, ox + 16, oy + 30, WOOD[1])
    for x in (ox + 12, ox + 19):
        cv.rect(x, oy + 21, x + 1, oy + 23, GREY[0])
    cv.ellipse(ox + 23, oy + 8, 5, 3, K)                           # the pot
    cv.ellipse(ox + 23, oy + 8, 4, 2, GREY[1])
    cv.rect(ox + 19, oy + 9, ox + 27, oy + 12, GREY[1])
    cv.rect(ox + 22, oy + 3, ox + 24, oy + 4, WHITE)                # steam
    cv.set(ox + 21, oy + 2, WHITE)
    cv.ellipse(ox + 8, oy + 12, 3, 1, K)                             # a ring on the hob


def desk(cv):
    """a desk with papers, a flask and a book: Dr Wren's"""
    ox, oy = origin("desk")
    cv.box(ox + 1, oy + 8, ox + 30, oy + 20, WOOD[0])
    cv.rect(ox + 2, oy + 9, ox + 29, oy + 9, WOOD[2])
    cv.box(ox + 1, oy + 20, ox + 30, oy + 29, WOOD[1])
    cv.rect(ox + 5, oy + 24, ox + 8, oy + 25, WOOD[2])
    cv.rect(ox + 23, oy + 24, ox + 26, oy + 25, WOOD[2])
    cv.rect(ox + 1, oy + 30, ox + 30, oy + 31, SKIRT[2])
    cv.box(ox + 4, oy + 11, ox + 12, oy + 17, WHITE, GREY[1])          # papers
    for y in (13, 15):
        cv.rect(ox + 6, oy + y, ox + 10, oy + y, GREY[1])
    cv.box(ox + 15, oy + 12, ox + 20, oy + 17, BOOKS[2])             # a book
    cv.ellipse(ox + 25, oy + 13, 3, 3, K)                             # a flask
    cv.ellipse(ox + 25, oy + 13, 2, 2, LEAF[2])
    cv.rect(ox + 24, oy + 6, ox + 26, oy + 10, K)
    cv.rect(ox + 25, oy + 7, ox + 25, oy + 10, GLASS[1])


def lamp(cv):
    ox, oy = origin("lamp")
    cv.ellipse(ox + 7, oy + 7, 5, 4, K)
    cv.ellipse(ox + 7, oy + 7, 4, 3, BOOKS[3])
    cv.rect(ox + 4, oy + 9, ox + 11, oy + 10, RUG[2])
    cv.rect(ox + 7, oy + 11, ox + 8, oy + 27, K)
    cv.ellipse(ox + 7, oy + 29, 4, 1, K)


def machine(cv):
    """a humming machine: a screen, lights and dials"""
    ox, oy = origin("machine")
    cv.box(ox + 1, oy + 3, ox + 30, oy + 31, GREY[0], GREY[1])
    cv.rect(ox + 2, oy + 4, ox + 29, oy + 4, GREY[2])
    cv.box(ox + 4, oy + 7, ox + 20, oy + 18, (28, 52, 40))
    for y, w in ((9, 10), (11, 6), (13, 12), (15, 8)):
        cv.rect(ox + 6, oy + y, ox + 5 + w, oy + y, (112, 220, 140))
    for i, c in enumerate(((220, 64, 56), (232, 196, 64), (96, 200, 96))):
        cv.ellipse(ox + 25, oy + 9 + i * 4, 1, 1, c)
    cv.box(ox + 4, oy + 22, ox + 27, oy + 28, GREY[1])
    for x in range(ox + 6, ox + 26, 4):
        cv.rect(x, oy + 24, x + 1, oy + 26, GREY[2])


def fireplace(cv):
    ox, oy = origin("fireplace")
    brick = ((156, 100, 84), (120, 74, 62))
    cv.rect(ox + 1, oy + 4, ox + 30, oy + 31, K)
    for y in range(oy + 5, oy + 31):
        for x in range(ox + 2, ox + 30):
            row = (y - oy) // 4
            seam = (y - oy) % 4 == 0 or (x - ox + (row % 2) * 4) % 8 == 0
            cv.set(x, y, brick[1] if seam else brick[0])
    cv.rect(ox, oy + 3, ox + 31, oy + 6, WOOD[1])                   # the mantel
    cv.rect(ox, oy + 3, ox + 31, oy + 3, WOOD[2])
    cv.box(ox + 7, oy + 14, ox + 24, oy + 31, (24, 18, 18))
    for cx, cy, rx, ry, c in ((15, 26, 6, 4, (220, 88, 40)), (14, 25, 3, 3, (244, 180, 64)),
                              (18, 27, 2, 2, (244, 180, 64))):
        cv.ellipse(ox + cx, oy + cy, rx, ry, c)
    cv.rect(ox + 9, oy + 30, ox + 22, oy + 30, WOOD[1])


def cabinet(cv):
    """a tall cupboard with a plate on show"""
    ox, oy = origin("cabinet")
    cv.box(ox + 2, oy + 2, ox + 29, oy + 31, WOOD[0])
    cv.rect(ox + 3, oy + 3, ox + 28, oy + 3, WOOD[2])
    cv.box(ox + 5, oy + 6, ox + 26, oy + 16, GLASS[0], WOOD[1])
    cv.ellipse(ox + 11, oy + 11, 3, 3, WHITE)
    cv.ellipse(ox + 20, oy + 11, 3, 3, WHITE)
    cv.ellipse(ox + 11, oy + 11, 1, 1, BLANKET[0])
    cv.ellipse(ox + 20, oy + 11, 1, 1, RUG[0])
    cv.box(ox + 5, oy + 19, ox + 15, oy + 29, WOOD[0], WOOD[1])
    cv.box(ox + 16, oy + 19, ox + 26, oy + 29, WOOD[0], WOOD[1])
    cv.rect(ox + 13, oy + 23, ox + 13, oy + 25, GREY[2])
    cv.rect(ox + 18, oy + 23, ox + 18, oy + 25, GREY[2])


def draw():
    from PIL import Image
    cv = Canvas()
    floor(cv, "floor", 0)
    floor(cv, "floor_b", 8)
    wall(cv, *origin("wall"))
    wall_low(cv, *origin("wall_low"))
    window(cv)
    picture(cv)
    clock(cv)
    mat(cv)
    chair(cv, "chair", False)
    chair(cv, "chair_back", True)
    bookshelf(cv)
    bed(cv)
    table(cv)
    plant(cv)
    crate(cv)
    barrel(cv)
    stool(cv)
    rug(cv)
    counter(cv)
    desk(cv)
    lamp(cv)
    machine(cv)
    fireplace(cv)
    cabinet(cv)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for y in range(H):
        for x in range(W):
            if cv.px[y][x]:
                im.putpixel((x, y), cv.px[y][x] + (255,))
    return im


def main():
    im = draw()
    out = ART / "interior.png"
    if "--check" in sys.argv:
        from PIL import Image
        if not out.exists():
            raise SystemExit(f"{out} is missing (rerun without --check)")
        have = Image.open(out).convert("RGBA")
        if have.size != im.size or have.tobytes() != im.tobytes():
            raise SystemExit(f"{out} differs from the drawing (rerun without --check)")
        print(f"{out.name}: ok")
        return
    ART.mkdir(exist_ok=True)
    im.save(out, optimize=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
