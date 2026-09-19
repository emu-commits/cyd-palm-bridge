#!/usr/bin/env python3
"""Generate firmware/main/guru_pool.c from firmware/main/guru_pool.txt.

The habit pool used to be a C table, which meant changing a line of user-facing
copy was a code edit and a reflash. It now lives in a text file, and this script
compiles that file into the const table the firmware falls back on when the SD
card has no pool of its own.

    tools/gen_guru_pool.py              # regenerate guru_pool.c
    tools/gen_guru_pool.py --check      # fail if it is out of date (CI)

Two source-of-truth rules make that safe:

  * The .txt is the source, the .c is a build product. Never hand-edit the .c --
    the --check gate in CI regenerates it and diffs, so an edit there is a build
    failure, not a silent divergence.
  * The .txt is ALSO what ships to /sdcard/guru.txt, and gurupool.c parses the
    same grammar at runtime. One format, parsed twice, tested against this one
    file -- so if the two parsers ever disagree, the gurupool test says so.

Validation lives here rather than in the C: a bad pool should fail the build,
not the device. Everything guru_test.c asserts about the table (unique ids in
range, names short enough for the row, every category populated) is checked
here first, where the error message can name the line number.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware" / "main" / "guru_pool.txt"
OUT = ROOT / "firmware" / "main" / "guru_pool.c"

# file key -> (C enum symbol, sort order). The order here is the order the
# categories appear on screen; the enum VALUES are persisted in the log and are
# guru.h's business, not ours.
CATS = [
    ("gut", "GU_CAT_GUT"),
    ("metabolic", "GU_CAT_METAB"),
    ("mind", "GU_CAT_COGN"),
    ("strength", "GU_CAT_STRUCT"),
    ("recovery", "GU_CAT_RECOV"),
]
CAT_KEYS = {k: i for i, (k, _) in enumerate(CATS)}

ID_MAX = 64      # GU_TASK_MAX -- id-1 indexes a bit in the saved state
NAME_MAX = 30    # past this the table row clips on the device
# The detail screen scrolls, so a long why is fine -- this only catches a
# runaway line (a file saved with no line endings, say), where the useful thing
# is an error naming the habit rather than one enormous unreadable screen.
WHY_MAX = 600


class PoolError(Exception):
    pass


def parse(text, origin="guru_pool.txt"):
    """-> list of (id, cat_key, name, why), in file order. Raises PoolError."""
    out = []
    seen = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue

        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 4:
            raise PoolError(
                "%s:%d: expected 4 fields separated by '|' (id | category | "
                "name | why), found %d" % (origin, lineno, len(parts)))
        sid, cat, name, why = parts

        if not re.fullmatch(r"\d+", sid):
            raise PoolError("%s:%d: id %r is not a number" % (origin, lineno, sid))
        tid = int(sid)
        if not 1 <= tid <= ID_MAX:
            raise PoolError("%s:%d: id %d is outside 1..%d -- ids index a bit in "
                            "the saved state" % (origin, lineno, tid, ID_MAX))
        if tid in seen:
            raise PoolError("%s:%d: id %d already used on line %d. Ids are "
                            "permanent and the log stores them; assign the next "
                            "free one instead." % (origin, lineno, tid, seen[tid]))
        seen[tid] = lineno

        if cat not in CAT_KEYS:
            raise PoolError("%s:%d: unknown category %r -- expected one of %s"
                            % (origin, lineno, cat, ", ".join(CAT_KEYS)))
        if not name:
            raise PoolError("%s:%d: task %d has no name" % (origin, lineno, tid))
        if len(name) > NAME_MAX:
            raise PoolError("%s:%d: name %r is %d characters; the row clips past "
                            "%d" % (origin, lineno, name, len(name), NAME_MAX))
        if not why:
            raise PoolError("%s:%d: task %d (%s) has no why line. Every habit has "
                            "to say what it is for." % (origin, lineno, tid, name))
        if len(why) > WHY_MAX:
            raise PoolError("%s:%d: the why line for %r is %d characters; the cap "
                            "is %d. The detail screen scrolls, so this is almost "
                            "always a line that lost its newline."
                            % (origin, lineno, name, len(why), WHY_MAX))

        out.append((tid, cat, name, why))

    if not out:
        raise PoolError("%s: the pool is empty" % origin)

    missing = [k for k in CAT_KEYS if not any(t[1] == k for t in out)]
    if missing:
        raise PoolError("%s: no habits in %s. Every category needs at least one, "
                        "or the week analysis has a silent hole."
                        % (origin, " or ".join(missing)))
    return out


def cstr(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def render(tasks):
    """Emit the table grouped by category, so the list's headings come out right.

    gu_build_list() walks the pool in order and starts a new heading whenever the
    category changes, so a pool interleaved by category would print "Gut" four
    times. Sorting here means hand-editing the .txt cannot cause that -- a habit
    can be written down anywhere in the file and still lands under its heading.
    """
    by_cat = sorted(tasks, key=lambda t: (CAT_KEYS[t[1]], tasks.index(t)))

    L = []
    L.append("/* guru_pool.c -- GENERATED by tools/gen_guru_pool.py. DO NOT EDIT.")
    L.append(" *")
    L.append(" * Source: firmware/main/guru_pool.txt -- edit that, then run")
    L.append(" * `make -C sim gurupool`. CI regenerates this file and diffs it, so a")
    L.append(" * hand edit here is a build failure rather than a quiet divergence.")
    L.append(" *")
    L.append(" * This is the pool the firmware falls back on. /sdcard/guru.txt, when")
    L.append(" * present, overrides it at runtime -- see gurupool.c. */")
    L.append('#include "guru.h"')
    L.append("")
    L.append("const GuruTask GU_POOL_BUILTIN[] = {")

    last = None
    for tid, cat, name, why in by_cat:
        if cat != last:
            L.append(" /* --- %s %s */" % (cat, "-" * max(0, 68 - len(cat))))
            last = cat
        L.append(" { %2d, %-14s %s," % (tid, CATS[CAT_KEYS[cat]][1] + ",", cstr(name)))
        L.append("      %s }," % cstr(why))
    L.append("};")
    L.append("")
    L.append("const int GU_POOL_BUILTIN_N ="
             " (int)(sizeof(GU_POOL_BUILTIN) / sizeof(GU_POOL_BUILTIN[0]));")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if guru_pool.c is out of date instead of writing it")
    args = ap.parse_args()

    try:
        tasks = parse(SRC.read_text(encoding="utf-8"))
    except PoolError as e:
        print("guru pool: FAIL -- %s" % e)
        return 1

    text = render(tasks)

    if args.check:
        have = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if have != text:
            print("guru pool: FAIL -- %s is out of date.\n"
                  "       guru_pool.txt changed without regenerating. Run:\n"
                  "           tools/gen_guru_pool.py" % OUT.relative_to(ROOT))
            return 1
        print("guru pool: OK -- %d habits, guru_pool.c matches guru_pool.txt"
              % len(tasks))
        return 0

    OUT.write_text(text, encoding="utf-8")
    print("guru pool: wrote %s (%d habits)" % (OUT.relative_to(ROOT), len(tasks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
