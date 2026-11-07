#!/usr/bin/env python3
"""Draw Critters' status icons: one 9 x 9 picture per status, in at most four
colours (index 0 is clear), shown as the badge on the battle cards
(docs/EFFECTS_PLAN.md section 5).

Drawn for this project and given to the public domain (CC0). Tuxemon has a
9 x 9 icon for each status (gfx/ui/icons/status/), but its ATTRIBUTIONS.md
doesn't list them, so their licence is unknown and the game takes clean
licences only. mkgame.py imports ICONS and builds them into the game file's
STEX section (GAME_FORMAT.md 3.19); nothing is read from disk.

    python3 tools/gen_status_icons.py --png out.png    # a sheet to look at, 8x
    python3 tools/gen_status_icons.py --check          # every icon is 9 x 9 in its colours

Each picture is nine strings of nine characters: '.' clear, '#' the outline,
'o' the main colour, '+' the light one. A status with no picture of its own
here can't be built (mkgame.py says which).
"""
import sys

OUTLINE = (40, 32, 36)

# slug: (main colour, light colour, rows)
ICONS = {
    "poison": ((150, 70, 190), (214, 160, 236), [
        "...###...",
        "..#ooo#..",
        ".#o+ooo#.",
        ".#ooooo#.",
        ".#o#o#o#.",
        "..#ooo#..",
        "...#o#...",
        "..#o#o#..",
        "...#.#...",
    ]),
    "burn": ((226, 92, 32), (252, 206, 72), [
        "....#....",
        "...#o#...",
        "..#oo#...",
        "..#o+o#..",
        ".#o++o#..",
        ".#o+++o#.",
        ".#o+++o#.",
        "..#ooo#..",
        "...###...",
    ]),
    "recover": ((52, 160, 76), (168, 230, 150), [
        "...###...",
        "...#o#...",
        "...#o#...",
        "####o####",
        "#ooo+ooo#",
        "####o####",
        "...#o#...",
        "...#o#...",
        "...###...",
    ]),
    "exhausted": ((120, 120, 140), (196, 196, 210), [
        ".........",
        ".#######.",
        ".#ooooo#.",
        ".#o+ooo#.",
        ".#######.",
        "..#.#.#..",
        "...#.#...",
        "....#....",
        ".........",
    ]),
    "blinded": ((60, 60, 72), (200, 200, 210), [
        ".........",
        "..#####..",
        ".#+++++#.",
        "#+#ooo#+#",
        "#+o###o+#",
        "#+#ooo#+#",
        ".#+++++#.",
        "..#####..",
        ".........",
    ]),
    "enraged": ((214, 40, 40), (252, 150, 120), [
        "#.......#",
        ".#.....#.",
        "..#...#..",
        ".#o#.#o#.",
        "#ooo#ooo#",
        "#o+o#o+o#",
        "#ooo#ooo#",
        ".###.###.",
        ".........",
    ]),
    "hardshell": ((168, 120, 64), (228, 196, 132), [
        "..#####..",
        ".#+++++#.",
        "#+ooooo+#",
        "#o#ooo#o#",
        "#oo###oo#",
        "#o#ooo#o#",
        ".#ooooo#.",
        "..#ooo#..",
        "...###...",
    ]),
    "sniping": ((40, 120, 200), (170, 212, 248), [
        "...###...",
        "..#+++#..",
        ".#+.#.+#.",
        "#+..#..+#",
        "#+######+",
        "#+..#..+#",
        ".#+.#.+#.",
        "..#+++#..",
        "...###...",
    ]),
    "slow": ((90, 150, 90), (180, 220, 160), [
        ".........",
        "...###...",
        "..#+o+#..",
        ".#ooooo#.",
        ".#o+o+o#.",
        "#########",
        "#o#...#o#",
        "###...###",
        ".........",
    ]),
    "softened": ((210, 120, 160), (248, 200, 220), [
        ".........",
        "..#####..",
        ".#+++++#.",
        "#+ooooo+#",
        "#ooooooo#",
        "#ooooooo#",
        ".##ooo##.",
        "...###...",
        ".........",
    ]),
    "focused": ((232, 184, 32), (252, 236, 150), [
        "....#....",
        "...#+#...",
        "...#o#...",
        "####o####",
        "#+ooooo+#",
        "####o####",
        "...#o#...",
        "...#+#...",
        "....#....",
    ]),
    "chargedup": ((240, 200, 20), (255, 248, 170), [
        ".....###.",
        "....#++#.",
        "...#++#..",
        "..#ooo###",
        ".####oo#.",
        "...#oo#..",
        "..#o+#...",
        ".#o##....",
        ".##......",
    ]),
    "charging": ((90, 150, 230), (190, 220, 252), [
        "...###...",
        "..#+++#..",
        ".#+...+#.",
        "#o.....o#",
        "#o..#..o#",
        "#o.....o#",
        ".#o...o#.",
        "..#ooo#..",
        "...###...",
    ]),
    "noddingoff": ((70, 90, 190), (170, 190, 250), [
        "#####....",
        "#ooo#....",
        "..#o#....",
        ".#o#.####",
        "#o#..#+o#",
        "#####.#+#",
        "....##+#.",
        "....#oo##",
        "....#####",
    ]),
    "stuck": ((150, 110, 70), (210, 180, 140), [
        ".........",
        "#.#.#.#.#",
        "o#o#o#o#o",
        "#########",
        "#oo+oo+o#",
        "#########",
        "o#o#o#o#o",
        "#.#.#.#.#",
        ".........",
    ]),
    "grabbed": ((200, 150, 110), (240, 210, 180), [
        ".#.#.#...",
        "#o#o#o#..",
        "#o#o#o##.",
        "#ooooo#o#",
        "#o+ooooo#",
        "#oooooo#.",
        ".#ooooo#.",
        "..#####..",
        ".........",
    ]),
    "flinching": ((240, 160, 40), (252, 226, 150), [
        "....#....",
        "...#+#...",
        "...#o#...",
        "...#o#...",
        "...#o#...",
        "...###...",
        "...#+#...",
        "...###...",
        ".........",
    ]),
    "confused": ((160, 90, 200), (220, 180, 248), [
        ".#####...",
        "#ooooo#..",
        "#o###o#..",
        "...#oo#..",
        "..#oo#...",
        "..#o#....",
        "..###....",
        "..#+#....",
        "..###....",
    ]),
    "harpooned": ((110, 120, 140), (190, 200, 216), [
        "......###",
        ".....#++#",
        "....#o##.",
        "...#o#...",
        "..#o#....",
        "##o#.....",
        "#+##.....",
        "#o#......",
        "###......",
    ]),
    "festering": ((120, 140, 40), (200, 210, 110), [
        ".........",
        "..#...#..",
        ".#o#.#+#.",
        "..#.#o#..",
        ".#.#ooo#.",
        "#+#.#o#..",
        ".#.#...#.",
        "..#o#.#o#",
        "...#...#.",
    ]),
    "lifeleech": ((190, 30, 50), (250, 140, 150), [
        "....#....",
        "...#o#...",
        "...#o#...",
        "..#oo+#..",
        ".#ooo+o#.",
        ".#ooooo#.",
        ".#ooooo#.",
        "..#ooo#..",
        "...###...",
    ]),
    "wasting": ((130, 100, 80), (200, 176, 150), [
        "#########",
        "#+++++++#",
        ".#+++++#.",
        "..#ooo#..",
        "...#o#...",
        "..#.o.#..",
        ".#.ooo.#.",
        "#ooooooo#",
        "#########",
    ]),
    "elementalshield": ((60, 170, 200), (180, 236, 248), [
        ".#######.",
        "#+++++++#",
        "#+ooooo+#",
        "#+o+o+o+#",
        "#+ooooo+#",
        ".#+ooo+#.",
        "..#+o+#..",
        "...#+#...",
        "....#....",
    ]),
    "prickly": ((60, 140, 60), (160, 220, 130), [
        "#...#...#",
        ".#.#o#.#.",
        "..#ooo#..",
        ".#o+o+o#.",
        "#ooooooo#",
        ".#ooooo#.",
        "..#ooo#..",
        ".#.#o#.#.",
        "#...#...#",
    ]),
    "spiky": ((150, 150, 160), (220, 220, 230), [
        "....#....",
        "...#o#...",
        "#..#o#..#",
        ".##o+o##.",
        "#ooo+ooo#",
        ".##o+o##.",
        "#..#o#..#",
        "...#o#...",
        "....#....",
    ]),
    "diehard": ((200, 60, 60), (250, 170, 160), [
        ".##...##.",
        "#oo#.#+o#",
        "#ooo#+oo#",
        "#oooooo#.",
        ".#oooo#..",
        "..#ooo#..",
        "...#o#...",
        "....#....",
        "....#....",
    ]),
    "revenge": ((190, 70, 30), (250, 180, 120), [
        "...#.....",
        "..#o#....",
        ".#o+o####",
        "#ooooooo#",
        ".#o+o####",
        "..#o#..#.",
        "...#..#o#",
        ".....#o#.",
        "......#..",
    ]),
    "retaliate": ((200, 110, 40), (252, 200, 130), [
        ".....#...",
        "....#o#..",
        "####o+o#.",
        "#ooooooo#",
        "####o+o#.",
        "....#o#..",
        ".#...#...",
        "#o#......",
        ".#.......",
    ]),
}


def check():
    bad = []
    for slug, (main, light, rows) in ICONS.items():
        if len(rows) != 9 or any(len(r) != 9 or set(r) - set(".#o+") for r in rows):
            bad.append(slug)
    return bad


def png(path):
    from PIL import Image
    n, k = len(ICONS), 8
    im = Image.new("RGB", (n * 11 * k, 11 * k), (244, 241, 228))
    for i, (slug, (main, light, rows)) in enumerate(sorted(ICONS.items())):
        col = {"#": OUTLINE, "o": main, "+": light}
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                if ch in col:
                    for dy in range(k):
                        for dx in range(k):
                            im.putpixel(((i * 11 + 1 + x) * k + dx, (1 + y) * k + dy), col[ch])
    im.save(path)


def main(argv):
    if argv[:1] == ["--png"] and len(argv) == 2:
        png(argv[1])
        return 0
    bad = check()
    if bad:
        print("bad icons: " + ", ".join(bad))
        return 1
    print(f"{len(ICONS)} status icons: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
