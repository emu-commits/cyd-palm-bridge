#!/usr/bin/env python3
"""The Dex's checks that need the file read a second way (make -C sim dex).

    dex_check.py GAME WHERE LINES

WHERE is `dex_test where GAME`: the C search's answer for every critter in every
act. Here the same answers are worked out independently, from mkgame.py's reader
of the file (zones and encounter tables, through the steps), and must match.

LINES is `dex_test lines GAME`: every line the speaker can say. Each must wrap to
at most five lines of the lower screen (216 px of the Palm font), and every
description to three with the two lines above it. Standard library only.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkgame                       # noqa: E402
from palm_font import PALM          # noqa: E402

WIDTH, SPEECH, DESC = 216, 5, 3
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + msg)


def main(game, where, lines):
    g = mkgame.Game(Path(game).read_bytes())
    m = g.meta()
    steps = []
    if mkgame.K_STEPS in m:
        steps = [struct.unpack_from("<HHH", m[mkgame.K_STEPS], 8 * i) for i in range(len(m[mkgame.K_STEPS]) // 8)]
    flags = []
    for f, _, _ in steps:
        if f not in flags:
            flags.append(f)
    b = g.sec[b"ENCT"]
    n = struct.unpack_from("<H", b)[0]
    rows0 = (4 + 4 * n + 3) & ~3
    tables = []
    for i in range(n):
        first, cnt = struct.unpack_from("<HH", b, 4 + 4 * i)
        tables.append([struct.unpack_from("<HBBH", b, rows0 + 8 * j) for j in range(first, first + cnt)])
    areas = g.areas()
    nmons = len(g.records(b"MONS", 48))

    expect = []
    for act in range(len(flags) + 1):
        on = set(flags[:act])
        def now(t):
            for f, a, to in steps:
                if a == t and f in on:
                    t = to
            return t
        for mon in range(nmons):
            names, lo, hi = [], 255, 0
            for a in areas:
                here = False
                for z in a["zones"]:
                    for c, l, h, rate in tables[now(z[4])]:
                        if c == mon and rate:
                            here = True
                            lo, hi = min(lo, l), max(hi, h)
                if here:
                    names.append(a["name"])
            total = len(names)
            line = f"{act} {mon} {0 if lo == 255 else lo} {hi} {total}" + "".join("|" + s for s in names[:8])
            expect.append(line)
    got = Path(where).read_text().splitlines()
    check(len(got) == len(expect), f"{game}: {len(got)} Where answers, {len(expect)} worked out")
    for e, c in zip(expect, got):
        check(e == c, f"{game}: Where differs:\n  worked out {e}\n  the search {c}")

    said = sorted(set(Path(lines).read_text().splitlines()))
    check(said, f"{game}: he said nothing")
    worst = 0
    for s in said:
        k = len(PALM.wrap(s, WIDTH))
        worst = max(worst, k)
        check(k <= SPEECH, f"{game}: {k} lines on the lower screen: {s}")
    for r in g.records(b"DEXE", 24):
        d = g.s(r, 16)
        check(len(PALM.wrap(d, WIDTH)) <= DESC, f"{game}: a description past {DESC} lines: {d}")
    print(f"  dex_check {Path(game).parent.name}: {len(expect)} Where answers match; "
          f"{len(said)} lines, the longest {worst} of {SPEECH}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
    if fails:
        print(f"dex_check: FAILED ({fails})")
        sys.exit(1)
