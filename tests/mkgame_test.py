#!/usr/bin/env python3
"""Gate for tools/mkgame.py (CI: the "Course builder" job, beside mkcourse).

  1. The fixture (tests/data/game/: source.py writes a small Tuxemon-shaped
     tree of our own) rebuilds to the committed game.bin, byte for byte,
     twice, and at 4 bits per pixel too; --check passes; ids.tsv is unchanged.
  2. ids.tsv: a creature without a number gets the next one; the others keep
     theirs.
  3. Bad regions are refused, each with a message that says what's wrong.
  4. The committed game (courses/critters/game.bin, built from the pinned
     Tuxemon with --strict) reads back clean, credits nothing unverified, and
     the player's sheet (ours, CC0) matches its drawing.

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

        # 3. refusals
        region = json.loads((FIX / "region.json").read_text())

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
    check(r.returncode == 0, f"tools/game_art/player.png: {r.stdout}{r.stderr}")

    print(f"mkgame_test: {'FAILED' if fails else 'OK'} ({fails} failure{'' if fails == 1 else 's'})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
