#!/usr/bin/env python3
"""Gate for tools/mkgame.py (CI: the "Course builder" job, beside mkcourse).

  1. The fixture (tests/data/game/: source.py writes a small Tuxemon-shaped
     tree of our own) rebuilds to the committed game.bin, byte for byte,
     twice, and at 4 bits per pixel too; --check passes; ids.tsv is unchanged.
  2. ids.tsv: a creature without a number gets the next one; the others keep
     theirs.
  3. Bad regions are refused, each with a message that says what's wrong.
  4. The committed game (courses/critters/game.bin, built from the pinned
     Tuxemon with --strict) reads back clean, credits nothing unverified, the
     player's sheets (ours, CC0) match their drawing, and the region can be
     walked: from the start, through the warps, every area and every person
     is reachable once the people a flag sends away have gone.

The C reader's side (its dump equal to mkgame's) is `make -C sim monster`.
Needs Pillow and PyYAML at the pinned versions (tools/requirements-game.txt).
Tuxemon itself isn't needed.

    python3 tests/mkgame_test.py
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests" / "data" / "game"
MKGAME = ROOT / "tools" / "mkgame.py"
SOURCE = "the test fixture (tests/data/game, CC0)"
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + msg)


def build(tree, out, ids, region=FIX / "region.json", allow=FIX / "allow.tsv", bpp=8):
    return subprocess.run([sys.executable, str(MKGAME), "--tuxemon", str(tree), "--unpinned", SOURCE,
                           "--allow", str(allow), "--region", str(region), "--ids", str(ids),
                           "--bpp", str(bpp), "-o", str(out)], capture_output=True, text=True)


STEPS = [(0, -1), (1, 0), (0, 1), (-1, 0)]


def walk(areas, start):
    """Every (area, x, y) the player can reach from start, by the movement
    bytes, ledges and warps (GAME_FORMAT.md 3.11), with the people standing
    except those a flag sends away (hide_if): what the story's end looks like."""
    from collections import deque
    seen, q = {start}, deque([start])
    while q:
        ai, x, y = q.popleft()
        a = areas[ai]
        w, h = a["w"], a["h"]
        for d, (dx, dy) in enumerate(STEPS):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < w and 0 <= ny < h) or a["move"][y * w + x] & (16 << d) or \
                    a["move"][ny * w + nx] & (1 << ((d + 2) & 3)):
                continue
            if any((p[0], p[1]) == (nx, ny) and not p[7] for p in a["people"]):
                continue
            t = (ai, nx, ny)
            if a["tflags"][a["cells"][ny * w + nx]] & 1:
                mx, my = nx + dx, ny + dy
                if 0 <= mx < w and 0 <= my < h and not a["move"][my * w + mx] & (1 << ((d + 2) & 3)):
                    t = (ai, mx, my)
            for wp in a["warps"]:
                if wp[0] <= t[1] < wp[0] + wp[2] and wp[1] <= t[2] < wp[1] + wp[3] and wp[7] in (255, d):
                    t = (wp[4], wp[5], wp[6])
                    break
            if t not in seen:
                seen.add(t)
                q.append(t)
    return seen


def main():
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        tree = t / "tree"
        subprocess.run([sys.executable, str(FIX / "source.py"), str(tree)], check=True)

        # 1. byte for byte, twice; 4 bits; --check; ids.tsv untouched
        ids = t / "ids.tsv"
        shutil.copy(FIX / "ids.tsv", ids)
        for n in (1, 2):
            r = build(tree, t / f"g{n}.bin", ids)
            check(r.returncode == 0, f"build {n}: {r.stderr.strip()}")
            check((t / f"g{n}.bin").read_bytes() == (FIX / "game.bin").read_bytes(),
                  f"build {n} differs from tests/data/game/game.bin (rebuild it: see source.py)")
        check(ids.read_text() == (FIX / "ids.tsv").read_text(), "a rebuild changed ids.tsv")
        r = build(tree, t / "g4.bin", ids, bpp=4)
        check(r.returncode == 0, f"4-bit build: {r.stderr.strip()}")
        r = subprocess.run([sys.executable, str(MKGAME), "--check", str(t / "g1.bin"), str(t / "g4.bin")],
                           capture_output=True, text=True)
        check(r.returncode == 0 and r.stdout.count(": ok") == 2, f"--check: {r.stdout}{r.stderr}")

        # 2. ids: drop thornet's number; it gets the next one, the rest keep theirs
        lines = (FIX / "ids.tsv").read_text().splitlines()
        nums = [int(x.split("\t")[2]) for x in lines if x.startswith("creature\t")]
        ids2 = t / "ids2.tsv"
        ids2.write_text("\n".join(x for x in lines if x != next(y for y in lines if "\tthornet\t" in y)) + "\n")
        r = build(tree, t / "g5.bin", ids2)
        check(r.returncode == 0, f"build with a number missing: {r.stderr.strip()}")
        got = {x.split("\t")[1]: int(x.split("\t")[2]) for x in ids2.read_text().splitlines()
               if x.startswith("creature\t")}
        want = {x.split("\t")[1]: int(x.split("\t")[2]) for x in lines if x.startswith("creature\t")}
        check(got.get("thornet") == max(nums) + 1, f"thornet got {got.get('thornet')}, not {max(nums) + 1}")
        check(all(got[k] == v for k, v in want.items() if k != "thornet"), "another creature's number changed")

        # 3. refusals (the bad regions are written beside a copy of the layout they name)
        region = json.loads((FIX / "region.json").read_text())
        shutil.copy(FIX / "village.json", t / "village.json")

        def refused(change, words, what):
            reg = json.loads(json.dumps(region))
            change(reg)
            p = t / "bad.json"
            p.write_text(json.dumps(reg))
            r = build(tree, t / "bad.bin", ids, region=p)
            check(r.returncode != 0 and words in r.stderr, f"{what}: {r.returncode} {r.stderr.strip()}")

        refused(lambda r: r["encounters"]["fixture_meadow"].append(
            {"creature": "ghostling", "levels": [2, 3], "rate": 5}), "not in the allow-list",
            "an encounter with a creature not on the allow-list")
        refused(lambda r: r["start"].update(x=15, y=4), "isn't a cell one can stand on", "a start in the pond")
        refused(lambda r: r["start"].update(area="fixture_elsewhere"), "isn't in the region", "a start outside the region")
        refused(lambda r: r["encounters"]["fixture_meadow"][0].update(levels=[5, 2]), "levels",
                "an encounter's levels the wrong way round")
        refused(lambda r: r.update(player="nobody"), "isn't in the allow-list", "a player sheet not on the allow-list")
        refused(lambda r: r["areas"][0]["people"][0].update(flag=None), "a trainer needs",
                "a trainer with no flag to remember the win by")
        refused(lambda r: r["areas"][0]["people"][1].update(team=[["puddlop", 3]]), "only a trainer",
                "a creature given to someone who isn't a trainer")
        refused(lambda r: r["areas"][0]["people"][1].update(sheet="fixture_rover"), "player's looks",
                "a person wearing one of the player's looks")
        refused(lambda r: r["areas"][0]["people"][1].update(at=[5, 14]), "shares a cell", "two people in one cell")
        refused(lambda r: r["steps"][0].update(flag="never_set"), "never set", "a step on a flag no one sets")
        refused(lambda r: r["steps"][0].update(to="fixture_nowhere"), "encounters don't have", "a step to no table")
        refused(lambda r: r["steps"][0].update(to=r["steps"][0]["table"]), "to itself", "a step to the same table")
        refused(lambda r: r.update(keeper_bonus=50), "keeper_bonus", "a keeper bonus under 100 %")
        refused(lambda r: r["areas"][0]["people"][2].pop("show_if"), "shares a cell",
                "a rematch in the same cell that doesn't come as the first goes")
        refused(lambda r: r.update(starters=r["starters"][:2]), "starters", "only two starters")
        refused(lambda r: r["starters"].append("emberkit"), "starters", "a starter offered twice")
        refused(lambda r: r["starters"].append("ghostling"), "starters", "a starter not on the allow-list")
        refused(lambda r: r["areas"][0]["warps"][0].update(to="fixture_elsewhere"), "not in the region",
                "a warp to an area outside the region")
        refused(lambda r: r["areas"][0]["people"][0].update(team=[["ghostling", 3]]), "isn't in the allow-list",
                "a trainer's creature not on the allow-list")
        r = subprocess.run([sys.executable, str(MKGAME), "--tuxemon", str(t), "--unpinned", SOURCE,
                            "-o", str(t / "x.bin")], capture_output=True, text=True)
        check(r.returncode != 0 and "Tuxemon-shaped" in r.stderr, "--unpinned on a tree that isn't one")
        r = subprocess.run([sys.executable, str(MKGAME), "--tuxemon", str(tree), "-o", str(t / "x.bin")],
                           capture_output=True, text=True)
        check(r.returncode != 0, "a build that isn't --unpinned needs the pinned checkout")
        bad_allow = t / "allow_ours.tsv"
        bad_allow.write_text((FIX / "allow.tsv").read_text() +
                             "character\tstranger\tCC0 1.0\tus\tmods/tuxemon/sprites/stranger.png\tours\n")
        r = build(tree, t / "bad.bin", ids, allow=bad_allow)
        check(r.returncode != 0 and "tools/game_art/stranger.png" in r.stderr,
              f"an 'ours' sheet that isn't in tools/game_art: {r.stderr.strip()}")
        bad_allow.write_text((FIX / "allow.tsv").read_text().replace("\tconfirmed\n", "\tedited\n", 1))
        r = build(tree, t / "bad.bin", ids, allow=bad_allow)
        check(r.returncode != 0 and "-sheet.png" in r.stderr and "edited" in r.stderr,
              f"an 'edited' sheet that isn't in tools/game_art: {r.stderr.strip()}")

        # a creature of our own: its rules are checked (the fixture's own moves and types)
        own = t / "own"
        own.mkdir()
        good = {"name": "Stranger", "description": "Not from around here.", "types": ["fire"],
                "shape": "blob", "stage": "basic", "catch_rate": 100,
                "moveset": [{"level_learned": 1, "technique": "scratch"}], "evolutions": []}
        own_allow = t / "allow_own.tsv"
        own_allow.write_text((FIX / "allow.tsv").read_text() +
                             "creature\tstranger\tCC0 1.0\tus\ttools/game_art/stranger-sheet.png\tours\n")

        def own_refused(change, words, what, write=True):
            d = json.loads(json.dumps(good))
            change(d)
            if write:
                (own / "stranger.yaml").write_text(json.dumps(d))     # JSON is YAML
            r = subprocess.run([sys.executable, str(MKGAME), "--tuxemon", str(tree), "--unpinned", SOURCE,
                                "--allow", str(own_allow), "--region", str(FIX / "region.json"), "--ids", str(ids),
                                "--creatures", str(own), "-o", str(t / "bad.bin")], capture_output=True, text=True)
            check(r.returncode != 0 and words in r.stderr, f"{what}: {r.returncode} {r.stderr.strip()}")
        own_refused(lambda d: d.update(types=["plasma"]), "types", "a creature of our own with a type no one has")
        own_refused(lambda d: d.update(shape="cube"), "shape", "a creature of our own with an unknown shape")
        own_refused(lambda d: d.pop("name"), "name", "a creature of our own with no name")
        own_refused(lambda d: d.update(colour="red"), "want", "a creature of our own with a key no one reads")
        own_refused(lambda d: d["moveset"].append({"level_learned": 3, "technique": "moonbeam"}),
                    "technique moonbeam", "a creature of our own with a move Tuxemon doesn't have")
        own_refused(lambda d: d.update(evolutions=[{"at_level": 9, "monster_slug": "ghostling"}]),
                    "isn't in the allow-list", "a creature of our own evolving into one not listed")
        own_refused(lambda d: None, "stranger-sheet.png", "a creature of our own with no sheet")
        (own / "stranger.yaml").unlink()
        own_refused(lambda d: None, "doesn't exist", "a creature of our own with no rules", write=False)
        own_allow.write_text((FIX / "allow.tsv").read_text() +
                             "creature\tstranger\tCC0 1.0\tus\ttools/game_art/stranger.png\tours\n")
        own_refused(lambda d: None, "stranger-sheet.png", "a creature of our own whose sheet isn't <slug>-sheet.png")

    # 4. the committed game
    game = ROOT / "courses" / "critters" / "game.bin"
    r = subprocess.run([sys.executable, str(MKGAME), "--check", str(game)], capture_output=True, text=True)
    check(r.returncode == 0 and ": ok" in r.stdout, f"courses/critters/game.bin --check: {r.stdout}{r.stderr}")
    sys.path.insert(0, str(ROOT / "tools"))
    import mkgame
    allow = mkgame.load_allow()
    bad = [f"{k} {s}" for k, d in allow.items() for s, v in d.items() if v["status"] == "unverified"]
    check(not bad, f"tools/game_allow.tsv has unverified lines, so the committed game can't be rebuilt: {bad}")
    data = game.read_bytes()
    import struct
    cred = ""
    for i in range(struct.unpack_from("<H", data, 12)[0]):        # the section count
        tag, off, ln, _ = struct.unpack_from("<4sIII", data, 32 + 16 * i)
        if tag == b"CRED":
            cred = data[off:off + ln].decode("utf-8")
    check(cred.startswith(f"Game data from Tuxemon (GPLv3), commit {mkgame.TUXEMON_COMMIT[:12]}."),
          "the committed game wasn't built from the pinned Tuxemon")
    check("player: CYD Palm (CC0 1.0)" in cred, "the committed game's player isn't our sheet")
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_player_sheet.py"), "--check"],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"tools/game_art/player*.png: {r.stdout}{r.stderr}")
    check("interior: CYD Palm (CC0 1.0)" in cred, "the committed game's rooms aren't our tileset")
    check("Fluttaflap: Spalding004, CYD Palm (back) (CC-BY-SA 4.0)" in cred,
          "the committed game's Fluttaflap isn't the sheet with the owner's back")
    import yaml
    for s, v in allow["creature"].items():
        if v["status"] in ("edited", "ours"):
            from PIL import Image
            im = Image.open(ROOT / v["source"])
            check(im.size == (128, 88) and im.mode == "RGBA", f"{v['source']}: {im.size} {im.mode}")
        if v["status"] == "ours":       # the sheet is what the drawing builds, to the byte
            d = yaml.safe_load((ROOT / "tools" / "game_creatures" / f"{s}.yaml").read_text())
            check(f"{d['name']}: {v['authors']} ({v['licence']})" in cred, f"the credits don't name {d['name']}")
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "mksheet.py"), "--check",
                                str(ROOT / d["drawing"]), str(ROOT / v["source"])], capture_output=True, text=True)
            check(r.returncode == 0, f"{v['source']}: {r.stdout}{r.stderr}")
    sys.path.insert(0, str(ROOT / "tools"))     # the eyes step: palette only, no file no sheet
    import mksheet
    from PIL import Image
    pal = [(10, 10, 10), (120, 120, 120), (240, 240, 240)]
    im = Image.new("RGBA", (7, 7), pal[1] + (255,))
    im.putpixel((3, 3), pal[0] + (255,))
    im.putpixel((0, 0), pal[0] + (255,))
    im.putpixel((6, 0), (0, 0, 0, 0))
    im.putpixel((5, 0), pal[0] + (255,))
    eyed = mksheet.apply_eyes(im, [{"wipe": [0, 0, 6, 6], "at": [1, 4], "draw": ["DH", ".D"]}], pal, "test")
    check(eyed.getpixel((3, 3)) == pal[1] + (255,), "the eyes step left the old eye")
    check(eyed.getpixel((5, 0)) == pal[0] + (255,), "the eyes step wiped an outline, a pixel beside the clear")
    check([eyed.getpixel(p)[:3] for p in ((1, 4), (2, 4), (2, 5), (1, 5))] == [pal[0], pal[2], pal[0], pal[1]],
          "the eyes step drew the wrong thing")
    check({c[:3] for c in eyed.get_flattened_data() if c[3]} <= set(pal), "the eyes step added a colour")
    eyed = mksheet.apply_eyes(im, [{"wipe": [0, 0, 0, 0], "at": [1, 4], "draw": ["Y"], "colors": {"Y": [130, 130, 130]}}],
                              pal, "test")
    check(eyed.getpixel((1, 4))[:3] == pal[1], "an eye's named colour isn't the palette's nearest")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "bare.jpg"
        shutil.copy(ROOT / "tools" / "game_art" / "src" / "cinderkit.jpg", src)
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "mksheet.py"), "--check", str(src),
                            str(ROOT / "tools" / "game_art" / "cinderkit-sheet.png")], capture_output=True, text=True)
        check(r.returncode != 0 and "eyes.json is missing" in r.stderr, f"--check took a drawing with no eyes file: {r.stderr}")
        check(not src.with_suffix(".eyes.json").exists(), "--check wrote an eyes file")
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "mksheet.py"), str(src), "-o", str(Path(tmp) / "o.png")],
                           capture_output=True, text=True)
        check(r.returncode == 0 and "DRAFT" in r.stdout and src.with_suffix(".eyes.json").exists(),
              f"a first build wrote no draft of the eyes: {r.stdout}{r.stderr}")
    with tempfile.TemporaryDirectory() as tmp:      # a transparent picture, and a scale kept with the eyes
        pic = Image.new("RGBA", (960, 600), (0, 0, 0, 0))
        for x0, size in ((60, 240), (560, 240)):
            pic.paste((10, 10, 10, 255), (x0, 60, x0 + size, 60 + size))
            pic.paste((250, 250, 250, 255), (x0 + 30, 90, x0 + size - 30, 30 + size))  # white fur: not background
        src = Path(tmp) / "clear.png"
        pic.save(src)
        (Path(tmp) / "clear.eyes.json").write_text(
            '{"scale": 1, "front": [], "back": [], "icon1": [], "icon2": []}')
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "mksheet.py"), str(src), "-o", str(Path(tmp) / "o.png")],
                           capture_output=True, text=True)
        check(r.returncode == 0 and "the background: transparent" in r.stdout and "scale: 1/1 as set" in r.stdout,
              f"a transparent drawing with a set scale: {r.stdout}{r.stderr}")
        if r.returncode == 0:
            sheet = Image.open(Path(tmp) / "o.png").convert("RGBA")
            check(max(c[0] for c in sheet.get_flattened_data() if c[3]) > 200, "a transparent drawing lost its white")
            check(sheet.crop((0, 0, 64, 64)).getchannel("A").getbbox() is not None, "a transparent drawing has no front")
    rows = [[(200, 200, 200)] * 6 for _ in range(6)]                    # the eyes file's "ink": a speck needs half a block
    rows[2][2] = (0, 0, 0)
    blacks = lambda im: sum(c[:3] == (0, 0, 0) for c in im.get_flattened_data())
    check(blacks(mksheet.shrink(rows, 2)) == 1, "1/2: a speck of ink didn't hold")
    check(blacks(mksheet.shrink(rows, 2, None, 2)) == 0, "1/2, ink 2: a speck darkened the block")
    tall = __import__("PIL.Image").Image.new("RGBA", (4, 6))
    tall.putpixel((1, 0), (9, 9, 9, 255))                               # a row over: the emptier edge goes, not a middle row
    for x in range(4):
        tall.putpixel((x, 5), (9, 9, 9, 255))
    trimmed = mksheet.fit(tall, 4, 5, "tall", [])
    check(trimmed.size == (4, 5) and trimmed.getpixel((0, 4))[3] == 255, f"a row over: {trimmed.size}")
    rows = [[(0, 0, 0)] + [(200, 200, 200)] * 3 for _ in range(4)]      # one cell of ink in a 4 x 4 block isn't a line
    check(mksheet.shrink(rows, 4).getpixel((0, 0))[:3] == (200, 200, 200), "1/4: a corner of ink darkened the block")
    rows = [[(0, 0, 0), (200, 200, 200)] for _ in range(2)]             # a line at 1/2 still holds
    check(mksheet.shrink(rows, 2).getpixel((0, 0))[:3] == (0, 0, 0), "1/2: a line didn't survive")
    ours = [s for s, v in allow["creature"].items() if v["status"] == "ours"]
    check(len(ours) >= 3, f"the owner's fire fox line isn't in the game: {ours}")
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_interior_sheet.py"), "--check"],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"tools/game_art/interior.png: {r.stdout}{r.stderr}")
    check(not [ln for ln in cred.split("\n") if "unverified" in ln], "the credits name something unverified")

    # 5. the region can be walked end to end
    g = mkgame.Game(data)
    areas = g.areas()
    sa, sx, sy, _ = struct.unpack_from("<HBBB", g.meta()[mkgame.K_START])
    seen = walk(areas, (sa, sx, sy))
    check({a for a, _, _ in seen} == set(range(len(areas))), "areas the walk can't reach: "
          f"{sorted(set(a['name'] for a in areas) - {areas[i]['name'] for i, _, _ in seen})}")
    for ai, a in enumerate(areas):
        for p in a["people"]:
            check(any((ai, p[0] + dx, p[1] + dy) in seen for dx, dy in STEPS),
                  f"no one can reach {p[9]} in {a['name']}")

    print(f"mkgame_test: {'FAILED' if fails else 'OK'} ({fails} failure{'' if fails == 1 else 's'})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
