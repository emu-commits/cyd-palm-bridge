#!/usr/bin/env python3
"""Draw Critters' player: tools/game_art/player*.png, 48 x 128 walking sheets.

Drawn for this project and given to the public domain (CC0), so the game's
player has a clean licence (GAME_PLAN.md §13 Q6). Tuxemon's own player sheet
(`adventurer`) has no credit line, and its history runs back through sheets
the credits list as non-commercial or XYG-licensed, so it isn't used. The
figure comes in six colourings, the looks a new game lets the player choose
from (SHEETS below).

The layout is Tuxemon's (mkgame.py reads it the same way): rows front (walking
down), left, right, back; columns step, stand, step. Each frame is 16 x 32,
the figure standing on the bottom of it.

    python3 tools/gen_player_sheet.py            # writes every sheet in tools/game_art/
    python3 tools/gen_player_sheet.py --check    # the PNGs match these drawings, pixel for pixel
"""
import sys
from pathlib import Path

ART = Path(__file__).resolve().parent / "game_art"

PAL = {
    ".": None,
    "K": (32, 28, 36),          # outline
    "H": (120, 64, 36),         # hair
    "h": (160, 96, 52),         # hair, lit
    "S": (244, 196, 150),       # skin
    "s": (214, 158, 118),       # skin, shaded
    "E": (32, 28, 36),          # eyes
    "R": (52, 112, 208),        # jacket
    "r": (36, 80, 160),         # jacket, shaded
    "W": (236, 236, 236),       # collar
    "P": (64, 60, 88),          # trousers
    "B": (112, 60, 32),         # boots
}

HEAD_FRONT = """
......KKKK......
....KKhhhhKK....
...KhhHHHHHHK...
..KhHHHHHHHHHK..
..KHHHHHHHHHHK..
..KHHSSSSSSHHK..
..KHSSSSSSSSHK..
..KSSESSSSESSK..
..KSSESSSSESSK..
...KSSSSSSSSK...
....KKsSSsKK....
"""
HEAD_BACK = """
......KKKK......
....KKhhhhKK....
...KhhHHHHhhK...
..KhHHHHHHHHHK..
..KHHHHHHHHHHK..
..KHHHHHHHHHHK..
..KHHHHHHHHHHK..
..KHHHHHHHHHHK..
..KsHHHHHHHHsK..
...KsHHHHHHsK...
....KKsSSsKK....
"""
HEAD_LEFT = """
......KKKK......
....KKhhhhKK....
...KhhhHHHHHK...
..KhHHHHHHHHHK..
..KHHHHHHHHHHK..
..KSSSHHHHHHHK..
.KSSSSSHHHHHHK..
.KSESSSSHHHHK...
.KSESSSSsHHHK...
..KSSSSSSsHK....
...KKSSSsKK.....
"""
BODY_FRONT = """
...KrWKKKKWrK...
..KRRRWWWWRRRK..
.KSKRRRRRRRRKSK.
.KSKRRRRRRRRKSK.
.KsKrRRRRRRrKsK.
..KKPPPPPPPPKK..
...KPPPPPPPPK...
"""
LEGS_FRONT = ["""
...KPPPKKPPPK...
...KPPPKKPPPK...
...KBBBK.KPPK...
...KKKK..KBBK...
.........KKK....
""", """
...KPPPKKPPPK...
...KPPPKKPPPK...
...KBBBKKBBBK...
...KKKK..KKKK...
................
""", """
...KPPPKKPPPK...
...KPPPKKPPPK...
...KPPK.KBBBK...
...KBBK..KKKK...
....KKK.........
"""]
BODY_BACK = """
...KrRKKKKRrK...
..KRRRRRRRRRRK..
.KSKRRRRRRRRKSK.
.KSKRRRRRRRRKSK.
.KsKrRRRRRRrKsK.
..KKPPPPPPPPKK..
...KPPPPPPPPK...
"""
BODY_LEFT = """
....KrRRRrK.....
...KRRRRRRRK....
...KRRSSRRRK....
...KRRSSRRRK....
...KrRsKRRrK....
....KPPPPPK.....
....KPPPPPK.....
"""
LEGS_LEFT = ["""
...KPPK.KPPK....
..KPPK...KPPK...
..KBBK...KBBK...
.KBBBK..KBBBK...
.KKKK...KKKK....
""", """
....KPPPPPK.....
....KPPPPPK.....
....KBBBBK......
...KBBBBBK......
...KKKKKK.......
""", """
....KPPKPPK.....
...KPPK.KPPK....
...KBBK.KBBK....
..KBBBK.KBBBK...
..KKKK..KKKK....
"""]


def rows(s):
    r = [line for line in s.strip("\n").split("\n")]
    for line in r:
        if len(line) != 16:
            raise SystemExit(f"a row isn't 16 wide: {line!r}")
    return r


# Each sheet: the colours that differ from the first (PAL's keys). These are
# the looks a new game offers the player (game_region.json's "player" list);
# the townspeople are Tuxemon's own sheets, credited in tools/game_allow.tsv.
SHEETS = {
    "player": {},
    "player_green": {"H": (40, 32, 28), "h": (72, 60, 52), "R": (64, 150, 72), "r": (44, 110, 52),
                     "P": (120, 96, 64)},
    "player_orange": {"H": (28, 24, 32), "h": (60, 52, 70), "R": (232, 128, 40), "r": (184, 92, 24),
                      "P": (52, 52, 60)},
    "player_olive": {"H": (96, 56, 28), "h": (136, 84, 44), "R": (104, 112, 52), "r": (76, 84, 36),
                     "W": (200, 184, 120), "P": (92, 72, 48)},
    "player_violet": {"H": (192, 52, 40), "h": (232, 96, 72), "R": (112, 64, 160), "r": (80, 44, 120),
                      "W": (232, 200, 72), "P": (48, 40, 64), "B": (40, 36, 44)},
    "player_rose": {"H": (216, 176, 72), "h": (240, 212, 120), "R": (224, 88, 120), "r": (180, 60, 92),
                    "P": (72, 64, 96)},
}


def frame(head, body, legs, bob):
    r = rows(head) + rows(body) + rows(legs)
    pad = 32 - len(r)
    out = ["." * 16] * (pad - bob) + r + ["." * 16] * bob
    return out


def mirror(f):
    return [line[::-1] for line in f]


def sheet():
    front = [frame(HEAD_FRONT, BODY_FRONT, LEGS_FRONT[i], 1 if i != 1 else 0) for i in range(3)]
    back = [frame(HEAD_BACK, BODY_BACK, LEGS_FRONT[i], 1 if i != 1 else 0) for i in range(3)]
    left = [frame(HEAD_LEFT, BODY_LEFT, LEGS_LEFT[i], 1 if i != 1 else 0) for i in range(3)]
    right = [mirror(f) for f in left]
    return [front, left, right, back]


def draw(name="player"):
    from PIL import Image
    pal = dict(PAL, **SHEETS[name])
    im = Image.new("RGBA", (48, 128), (0, 0, 0, 0))
    for ry, row in enumerate(sheet()):
        for cx, f in enumerate(row):
            for y, line in enumerate(f):
                for x, ch in enumerate(line):
                    if ch not in pal:
                        raise SystemExit(f"no colour for {ch!r}")
                    if pal[ch]:
                        im.putpixel((cx * 16 + x, ry * 32 + y), pal[ch] + (255,))
    return im


def main():
    for name in SHEETS:
        im = draw(name)
        out = ART / f"{name}.png"
        if "--check" in sys.argv:
            from PIL import Image
            if not out.exists():
                raise SystemExit(f"{out} is missing (rerun without --check)")
            have = Image.open(out).convert("RGBA")
            if have.size != im.size or have.tobytes() != im.tobytes():
                raise SystemExit(f"{out} differs from the drawing (rerun without --check)")
            print(f"{out.name}: ok")
            continue
        ART.mkdir(exist_ok=True)
        im.save(out, optimize=True)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
