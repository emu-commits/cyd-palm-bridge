#!/usr/bin/env python3
"""Draw the drove road's gate: tools/game_art/gate.png, a 48 x 128 sheet.

Drawn for this project and given to the public domain (CC0). The gate stands
in the game as a person that's always the same picture (every frame of the
sheet is the gate), so it opens the way people do: by a flag
(BACKLOG.md, "the drove road"). Two leaves side by side close the two-cell
road; the left leaf (gate.png) has the chain and lock, the right
(gate_right.png) the hinge post.

The layout is Tuxemon's, as gen_player_sheet.py's: rows front, left, right,
back; columns step, stand, step; each frame 16 x 32, standing on its bottom.

    python3 tools/gen_gate_sheet.py            # writes both sheets
    python3 tools/gen_gate_sheet.py --check    # the PNGs match these drawings, pixel for pixel
"""
import sys
from pathlib import Path

ART = Path(__file__).resolve().parent / "game_art"

PAL = {
    ".": None,
    "K": (44, 30, 22),          # outline
    "W": (150, 100, 56),        # timber
    "w": (190, 136, 82),        # timber, lit
    "D": (110, 70, 38),         # timber, shaded
    "C": (150, 150, 160),       # chain
    "c": (204, 204, 214),       # chain, lit
    "L": (196, 156, 40),        # lock
}

# 16 x 20, standing on the frame's bottom: a post, three rails and a brace
LEFT = [
    "................",
    "................",
    "KK..............",
    "KwK.............",
    "KWKKKKKKKKKKKKKK",
    "KWKwwwwwwwwwwwwK",
    "KWKDDDDDDDDDDDDK",
    "KWKKKKKKKKKKKKKK",
    "KWK..c...KDK..KD",
    "KWKcC.CcKDK..KD.",
    "KWKKKLKKKKKKKKKK",
    "KWKwKLKwwwwwwwwK",
    "KWKDKKKDDDDDDDDK",
    "KWKKKKKKKKKKKKKK",
    "KWK...KDK.KDK...",
    "KWK..KDK.KDK....",
    "KWKKKKKKKKKKKKKK",
    "KWKwwwwwwwwwwwwK",
    "KDKDDDDDDDDDDDDK",
    "KKKKKKKKKKKKKKKK",
]
RIGHT = [
    "................",
    "................",
    "..............KK",
    ".............KwK",
    "KKKKKKKKKKKKKKWK",
    "KwwwwwwwwwwwwKWK",
    "KDDDDDDDDDDDDKWK",
    "KKKKKKKKKKKKKKWK",
    "DK..KDK...KDKKWK",
    ".DK..KDK.KDK.KWK",
    "KKKKKKKKKKKKKKWK",
    "KwwwwwwwwwwwwKWK",
    "KDDDDDDDDDDDDKWK",
    "KKKKKKKKKKKKKKWK",
    "...KDK.KDK...KWK",
    "....KDK.KDK..KWK",
    "KKKKKKKKKKKKKKWK",
    "KwwwwwwwwwwwwKWK",
    "KDDDDDDDDDDDDKDK",
    "KKKKKKKKKKKKKKKK",
]
SHEETS = {"gate": LEFT, "gate_right": RIGHT}


def draw(rows):
    from PIL import Image
    im = Image.new("RGBA", (48, 128), (0, 0, 0, 0))
    for fy in range(4):
        for fx in range(3):
            ox, oy = fx * 16, fy * 32 + 32 - len(rows)
            for y, r in enumerate(rows):
                for x, ch in enumerate(r):
                    if PAL[ch]:
                        im.putpixel((ox + x, oy + y), PAL[ch] + (255,))
    return im


def main(argv):
    from PIL import Image
    bad = [n for n, rows in SHEETS.items() if any(len(r) != 16 or set(r) - set(PAL) for r in rows)]
    if bad:
        print("bad drawings: " + ", ".join(bad))
        return 1
    for name, rows in SHEETS.items():
        im, p = draw(rows), ART / f"{name}.png"
        if "--check" in argv:
            if not p.exists() or list(Image.open(p).convert("RGBA").getdata()) != list(im.getdata()):
                print(f"{p} doesn't match the drawing")
                return 1
        else:
            im.save(p)
    print("gate sheets: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
