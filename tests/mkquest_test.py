#!/usr/bin/env python3
"""tools/mkquest.py: the committed Knowledge course is current, and a bad
graph is refused (stdlib only).

    python3 tests/mkquest_test.py
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkquest  # noqa: E402

fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", msg)


def refused(q, nodes, drafts=True, want=""):
    with tempfile.TemporaryDirectory() as d:
        Path(d, "quest.json").write_text(json.dumps(q))
        Path(d, "nodes.jsonl").write_text("".join(json.dumps(n) + "\n" for n in nodes))
        try:
            mkquest.convert(*mkquest.load(d), drafts=drafts)
        except mkquest.QuestError as e:
            return want in str(e)
        return False


def node(i, after, **kw):
    n = {"id": i, "region": 1, "after": after, "review": "reviewed", "title": i.title(),
         "idea": "An idea.", "explain": "An explanation.", "question": "A question?", "answer": "Yes"}
    n.update(kw)
    return n


def main():
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "mkquest.py"), str(ROOT / "courses" / "kq-numbers"),
                        "--check"], capture_output=True, text=True)
    check(r.returncode == 0, "courses/kq-numbers is current: " + r.stdout + r.stderr)
    q, regions, nodes = mkquest.load(ROOT / "courses" / "kq-numbers")
    check(len(nodes) == 40 and mkquest.is_linear(nodes), "the pilot is 40 steps in one line")

    Q = {"quest": 1, "id": "t", "title": "T", "version": "1", "regions": [{"region": 1, "title": "R"}],
         "scheduler": {"type": "stages", "intervals": ["1d", None]}}
    ok = [node("a", []), node("b", ["a"])]
    cj, ij, unrev = mkquest.convert(Q, {1: Q["regions"][0]}, ok)
    c = json.loads(cj)
    items = [json.loads(x) for x in ij.splitlines()]
    check(c["version"] == "1" and not unrev and c["lessons"]["batch"] == 1, "a reviewed course: no draft mark")
    check(items[1]["built_from"] == ["a"] and items[1]["examples"][0]["translation"] == "Yes",
          "after -> built_from, question/answer -> the example")
    cj, _, unrev = mkquest.convert(Q, {1: Q["regions"][0]}, [node("a", [], review="draft")], drafts=True)
    check(json.loads(cj)["version"] == "1-draft" and unrev == ["a"], "drafts say so in the version")
    check(refused(Q, [node("a", [], review="draft")], drafts=False, want="unreviewed"), "drafts refused by default")
    check(refused(Q, [node("b", ["a"]), node("a", [])], want="isn't earlier"), "a step after what it builds on")
    check(refused(Q, [node("a", []), node("a", [])], want="repeated"), "a repeated id")
    check(refused(Q, [node("a", [], region=2)], want="no region"), "an unknown region")
    check(refused(Q, [node("a", [], answer="x" * 25)], want="answer"), "an answer too long to show big")
    check(refused(Q, [{k: v for k, v in node("a", []).items() if k != "idea"}], want="missing"), "a missing field")
    check(not mkquest.is_linear([node("a", []), node("b", ["a"]), node("c", ["a"])]), "a branch isn't linear")

    print("mkquest_test: %s" % ("OK" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
