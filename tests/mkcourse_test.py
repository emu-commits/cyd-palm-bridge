#!/usr/bin/env python3
"""Gate for tools/mkcourse.py (CI: the "Course builder" job).

  1. Every committed course.srs rebuilds from its source to the same bytes, and
     the dump of its source equals the dump of the built file. (The C reader's
     side of the round trip is `make -C sim course`.)
  2. ids.tsv: a rebuild keeps every number; a new item gets the next one; a
     deleted item's number is retired, never reused.
  3. Bad sources are refused, each with a message that says what's wrong.

Needs Pillow at the pinned version (tools/requirements-course.txt) and the
pinned font, which mkcourse downloads; or COURSE_FONT=/path/to/the/font.

    python3 tests/mkcourse_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkcourse  # noqa: E402

FONT = os.environ.get("COURSE_FONT")
COURSES = ["courses/demo-kanji", "tests/data/study/cards", "tests/data/study/features",
           "tests/data/study/remap-v1", "tests/data/study/remap-v2"]
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + msg)


def rebuild_all():
    for rel in COURSES:
        d = ROOT / rel
        c, data, added = mkcourse.build(d, font=FONT, write_ids=False)
        committed = (d / "course.srs").read_bytes()
        check(added == 0, "%s: ids.tsv is missing %d ids (build it and commit ids.tsv)" % (rel, added))
        if data != committed:
            first = next((i for i in range(min(len(data), len(committed))) if data[i] != committed[i]), None)
            check(False, "%s: rebuilds to different bytes (%d vs %d B, first difference at %s). Rebuild with "
                  "`python3 tools/mkcourse.py %s` and commit, or check the Pillow version and font."
                  % (rel, len(data), len(committed), first, rel))
        check(mkcourse.dump_source(c, data) == mkcourse.dump_file(committed),
              "%s: the source's dump differs from the file's" % rel)
        print("  %-30s %7d B, rebuilt identically" % (rel, len(data)))


def ids_rules():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "c"
        shutil.copytree(ROOT / "tests/data/study/features", d)
        (d / "course.srs").unlink()
        before = dict(l.split("\t") for l in (d / "ids.tsv").read_text().splitlines() if l and l[0] != "#")
        top = max(int(v) for v in before.values())
        lines = (d / "items.jsonl").read_text().splitlines()
        # drop "long" (nothing links to it), add a new one
        kept = [l for l in lines if json.loads(l)["id"] != "long"]
        kept.append(json.dumps({"id": "comensal", "kind": "word", "level": 7, "term": "comensal",
                                "meanings": ["dinner guest"], "readings": ["ko-men-SAL"], "family": "comer"}))
        (d / "items.jsonl").write_text("\n".join(kept) + "\n")
        c, data, added = mkcourse.build(d, font=FONT)
        after = dict(l.split("\t") for l in (d / "ids.tsv").read_text().splitlines() if l and l[0] != "#")
        check(added == 1, "one new id, got %d" % added)
        check(after["comensal"] == str(top + 1), "the new item gets the next number")
        check(all(after[k] == v for k, v in before.items()), "every old number is kept, including the deleted item's")
        ids = {it["id"] for it in c.items}
        check(int(before["long"]) not in ids, "the deleted item's number isn't in the course")
        # a second build changes nothing
        c2, data2, added2 = mkcourse.build(d, font=FONT)
        check(added2 == 0 and data2 == data, "a second build is identical")
        # the next new item doesn't reuse the retired number
        kept.append(json.dumps({"id": "comilón", "kind": "word", "level": 7, "term": "comilón",
                                "meanings": ["big eater"], "readings": ["ko-mee-LON"]}))
        (d / "items.jsonl").write_text("\n".join(kept) + "\n")
        c3, _, _ = mkcourse.build(d, font=FONT)
        after3 = dict(l.split("\t") for l in (d / "ids.tsv").read_text().splitlines() if l and l[0] != "#")
        check(after3["comilón"] == str(top + 2), "numbers are never reused")
    print("  ids.tsv rules")


def refused(tweak, want, items=None):
    """Build the features deck with one thing broken; it must fail saying `want`."""
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "c"
        shutil.copytree(ROOT / "tests/data/study/features", d)
        cfg = json.loads((d / "course.json").read_text())
        lines = [json.loads(l) for l in (d / "items.jsonl").read_text().splitlines() if l.strip()]
        tweak(cfg, lines)
        (d / "course.json").write_text(json.dumps(cfg, ensure_ascii=False))
        (d / "items.jsonl").write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in lines) + "\n")
        try:
            mkcourse.build(d, font=FONT, write_ids=False)
            check(False, "not refused (wanted %r)" % want)
        except mkcourse.CourseError as e:
            check(want in str(e), "refused, but with %r (wanted %r)" % (str(e), want))


def refusals():
    def item(i, **kw):
        def t(cfg, lines):
            lines[i].update(kw)
        return t
    refused(item(0, colour="red"), "unknown field 'colour'")
    refused(item(1, built_from=["nope"]), "isn't an item in this course")
    refused(item(2, built_from=["comida"]), "go round in a loop")
    refused(item(1, id="comer"), "is also used at")
    refused(item(0, etymology="x" * 5000), "over the 4096 B limit")
    refused(item(1, meanings=["Ελλάδα"]), "has no glyph for")
    refused(item(1, readings=[{"text": "a", "primary": True}, {"text": "b", "primary": True}]), "exactly one reading")
    refused(item(3, level=99), "isn't in course.json")
    refused(item(1, meanings=[]), "needs \"meanings\"")
    refused(item(1, term="Čapek"), "has no glyph for 'Č'")      # a TERM is never altered
    refused(item(1, built_from=[{"id": "comer", "role": "prefix"}]), "isn't in course.json roles")

    def cfg(**kw):
        def t(c, lines):
            c.update(kw)
        return t
    refused(cfg(title="Ελλάδα"), "can't be drawn by the Palm font")
    refused(cfg(id="Bad Id"), "\"id\" must be")
    refused(cfg(format=2), "\"format\" must be 1")

    def sched(**kw):
        def t(c, lines):
            c["scheduler"].update(kw)
        return t
    refused(sched(intervals=["4x", None]), "a duration is")
    refused(sched(intervals=["4h", "8h"]), "the last null")
    refused(sched(known=9), "\"known\" must be a stage")

    def empty_level(c, lines):
        c["levels"].append({"level": 9})
    refused(empty_level, "has no items")
    print("  refusals")


def text_rules():
    """Folding and stripping, on a copy of the features deck."""
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "c"
        shutil.copytree(ROOT / "tests/data/study/features", d)
        cfg = json.loads((d / "course.json").read_text())
        cfg["render"]["strip"] = ["etymology", "example"]
        (d / "course.json").write_text(json.dumps(cfg, ensure_ascii=False))
        with open(d / "items.jsonl", "a") as f:
            f.write(json.dumps({"id": "t", "kind": "word", "level": 7, "term": "t", "meanings": ["m"], "readings": ["r"],
                                "examples": [{"text": "Ἑλλάς", "translation": "goes with its example"},
                                             {"text": "Greek Ἑλλάς (Hellas)", "translation": "stays"}]},
                               ensure_ascii=False) + "\n")
        c, data, _ = mkcourse.build(d, font=FONT, write_ids=False)
        dump = mkcourse.dump_file(data)
        check("goes with its example" not in dump, "a dropped example takes its translation with it")
        check('"Greek (Hellas)"' in dump and '"stays"' in dump, "a stripped example keeps what's readable")
    print("  text rules")


def reading_font():
    """The firmware's reading font is drawn from the pinned picture font: it
    must be exactly what tools/gen_kana_font.py draws."""
    r = subprocess.run([sys.executable, str(ROOT / "tools/gen_kana_font.py"), "--check"],
                       capture_output=True, text=True)
    check(r.returncode == 0, "lv_font_kana_20.c is what gen_kana_font.py draws: " + (r.stdout + r.stderr).strip())
    print("  reading font")


def main():
    print("mkcourse_test:")
    rebuild_all()
    ids_rules()
    refusals()
    text_rules()
    reading_font()
    print("mkcourse_test: %s" % ("FAILED" if fails else "OK"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
