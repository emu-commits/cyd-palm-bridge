#!/usr/bin/env python3
"""Every next task has at least two sources (the owner, 2026-11-04).

game_region.json's "objectives" lists what the player has to do next, between
two of the story's flags ("story_order"). Each must be named by at least two
separate people or signs who can be read while it's open: their words contain
one of its "says". A sign counts always; a person from their show_if to their
hide_if (a keeper's "after" only from their own flag on). Each line must also
fit the box: LINE_MAX characters. Run by tests/mkgame_test.py. Standard library only.
"""
import json
import sys
from pathlib import Path

REGION = Path(__file__).resolve().parent / "game_region.json"
LINE_MAX = 160
NEED = 2


def check(region):
    """A list of what's wrong; empty is good. Also a {objective: sources} map."""
    order = region["story_order"]
    pos = {f: i + 1 for i, f in enumerate(order)}
    end = len(order) + 1
    bad, found = [], {}

    def at(flag, default):
        return pos.get(flag, default) if flag else default

    sources = []    # (who, words, from, to)
    for a in region["areas"]:
        for s in a.get("signs", []):
            sources.append((f"sign in {a['name']}", s["text"], 0, end))
        for p in a.get("people", []):
            lo, hi = at(p.get("show_if"), 0), at(p.get("hide_if"), end)
            who = f"{p.get('name', '?')} ({a['name']})"
            sources.append((who, p["text"], lo, hi))
            if p.get("after"):
                sources.append((who, p["after"], max(lo, at(p.get("flag"), 0)), hi))
            for k in ("text", "after"):
                if len(p.get(k, "")) > LINE_MAX:
                    bad.append(f"{who}: {len(p[k])} characters, the box takes {LINE_MAX}: {p[k]}")
    for o in region["objectives"]:
        for f in (o.get("after"), o.get("until")):
            if f and f not in pos:
                bad.append(f"{o['name']}: {f} isn't in story_order")
        lo, hi = at(o.get("after"), 0), at(o.get("until"), end)
        who = sorted({w for w, words, a, b in sources if a < hi and b > lo and any(s in words for s in o["says"])})
        found[o["name"]] = who
        if len(who) < NEED:
            bad.append(f"{o['name']}: named by {len(who)} source(s), wants {NEED}: {who}")
    return bad, found


def main():
    bad, found = check(json.loads(REGION.read_text()))
    for name, who in found.items():
        print(f"  {len(who):2}  {name}: {', '.join(who[:4])}{' ...' if len(who) > 4 else ''}")
    for b in bad:
        print("FAIL: " + b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
