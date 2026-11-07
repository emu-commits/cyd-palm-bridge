#!/usr/bin/env python3
"""Turn a KnowledgeQuest source (a graph of reviewed concept cards) into a
Study course source, which tools/mkcourse.py then builds into course.srs.

    python3 tools/mkquest.py courses/kq-numbers            # write course.json + items.jsonl
    python3 tools/mkquest.py courses/kq-numbers --drafts   # the same, allowing unreviewed cards
    python3 tools/mkquest.py courses/kq-numbers --check    # fail if they're out of date (CI)

This is K0 of docs/KNOWLEDGEQUEST_PLAN.md: the pilot is an ordinary Study
course, opened by the Knowledge tile. The source folder holds:

  quest.json    the course: id, title, licence, regions (Study's levels) and
                the scheduler, in mkcourse.py's own terms (COURSE_FORMAT.md)
  nodes.jsonl   one concept a line, IN ORDER: a node may only come after
                every node it builds on, so the file's order is the order the
                steps are numbered in on the device

A node:

  id        stable, never changed once people are learning it (it's how Study
            finds their progress): 1-40 of a-z 0-9 -
  region    one of quest.json's regions
  after     the ids it builds on (hard prerequisites), all earlier in the file
  review    "draft" (written, by a model or a person, not yet checked) or
            "reviewed" (a person has read it and stands behind it)
  title     the concept's name: the card's heading and Study's term
  idea      the one-line idea: the card's big line
  explain   two to four sentences: the card's explanation
  question  one question that checks the idea, self-graded on the device
  answer    its answer, short enough to show big

How it maps (KNOWLEDGEQUEST_PLAN.md section 4): a node is an item of the
kind "concept"; title -> term, idea -> meanings[0], explain ->
mnemonic_meaning, question/answer -> examples[0] text/translation, after ->
built_from (with unlock.by_links, so a step's lesson waits for what it builds
on). Regions become levels, all open at once (level_percent 0): the links do
the gating. One concept a lesson (lessons.batch 1).

The build refuses unreviewed cards unless --drafts is given, and then says so
in the course's version ("0.1-draft", which the About box shows) and its
description, so a drafted course can't pass for a reviewed one.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
NODE_KEYS = {"id", "region", "after", "review", "title", "idea", "explain", "question", "answer"}
# what fits the screens without falling back to small print (240 px, Palm font):
# the title and the answer are drawn twice size when they fit two lines
LIMITS = {"title": 26, "idea": 100, "explain": 420, "question": 140, "answer": 24}
DRAFT_NOTE = " Draft: not yet reviewed by a person."


class QuestError(Exception):
    pass


def load(folder):
    folder = Path(folder)
    try:
        q = json.loads((folder / "quest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise QuestError("quest.json: %s" % e)
    if q.get("quest") != 1:
        raise QuestError('quest.json: "quest" must be 1')
    regions = {}
    for r in q.get("regions", []):
        if not isinstance(r.get("region"), int) or not r.get("title"):
            raise QuestError("quest.json: a region needs a number and a title")
        regions[r["region"]] = r
    if not regions:
        raise QuestError("quest.json: no regions")
    nodes, seen = [], {}
    try:
        lines = (folder / "nodes.jsonl").read_text(encoding="utf-8").splitlines()
    except OSError as e:
        raise QuestError("nodes.jsonl: %s" % e)
    for ln, line in enumerate(lines, 1):
        if not line.strip():
            continue
        where = "nodes.jsonl:%d" % ln
        try:
            n = json.loads(line)
        except ValueError as e:
            raise QuestError("%s: %s" % (where, e))
        if set(n) != NODE_KEYS:
            missing, extra = NODE_KEYS - set(n), set(n) - NODE_KEYS
            raise QuestError("%s: %s%s" % (where, "missing %s" % sorted(missing) if missing else "",
                                            " unknown %s" % sorted(extra) if extra else ""))
        if not ID_RE.match(n["id"]) or n["id"] in seen:
            raise QuestError("%s: bad or repeated id %r" % (where, n["id"]))
        if n["region"] not in regions:
            raise QuestError("%s: no region %r" % (where, n["region"]))
        if n["review"] not in ("draft", "reviewed"):
            raise QuestError('%s: review is "draft" or "reviewed"' % where)
        for a in n["after"]:
            if a not in seen:
                raise QuestError("%s: %r builds on %r, which isn't earlier in the file" % (where, n["id"], a))
        for k, lim in LIMITS.items():
            if not isinstance(n[k], str) or not n[k].strip():
                raise QuestError("%s: %s is empty" % (where, k))
            if len(n[k]) > lim:
                raise QuestError("%s: %s is %d characters, the limit is %d" % (where, k, len(n[k]), lim))
        if nodes and n["region"] < nodes[-1]["region"]:
            raise QuestError("%s: regions must not go backwards (Study numbers steps by level)" % where)
        seen[n["id"]] = n
        nodes.append(n)
    if not nodes:
        raise QuestError("nodes.jsonl: no nodes")
    return q, regions, nodes


def is_linear(nodes):
    """Each step builds on exactly the one before it, and the first on nothing."""
    return not nodes[0]["after"] and all(n["after"] == [p["id"]] for p, n in zip(nodes, nodes[1:]))


def convert(q, regions, nodes, drafts=False):
    unreviewed = [n["id"] for n in nodes if n["review"] != "reviewed"]
    if unreviewed and not drafts:
        raise QuestError("%d unreviewed card(s), such as %s: review them, or build with --drafts"
                         % (len(unreviewed), ", ".join(unreviewed[:3])))
    desc = q.get("description", "") + (DRAFT_NOTE if unreviewed else "")
    course = {"format": 1}
    for k in ("id", "title", "version", "author", "licence", "language"):
        if k in q:
            course[k] = q[k]
    if unreviewed:                  # the About box shows the version, not the description
        course["version"] = course.get("version", "0") + "-draft"
    course["description"] = desc
    if "source" in q:
        course["source"] = q["source"]
    course["kinds"] = [{"name": "concept", "quiz": ["meaning"], "level_up": True}]
    course["scheduler"] = q["scheduler"]
    course["grading"] = "two"
    course["lessons"] = {"batch": 1}
    course["unlock"] = {"level_percent": 0, "by_links": True}
    used = sorted({n["region"] for n in nodes})
    course["levels"] = [{"level": r, "title": regions[r]["title"], **({"theme": regions[r]["theme"]}
                                                                      if regions[r].get("theme") else {})}
                        for r in used]
    items = []
    for n in nodes:
        it = {"id": n["id"], "kind": "concept", "level": n["region"], "term": n["title"],
              "meanings": [n["idea"]], "mnemonic_meaning": n["explain"],
              "examples": [{"text": n["question"], "translation": n["answer"]}]}
        if n["after"]:
            it["built_from"] = list(n["after"])
        items.append(it)
    cj = json.dumps(course, ensure_ascii=False, indent=2) + "\n"
    ij = "".join(json.dumps(i, ensure_ascii=False) + "\n" for i in items)
    return cj, ij, unreviewed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--drafts", action="store_true", help="allow unreviewed cards (the course says so)")
    ap.add_argument("--check", action="store_true", help="fail if course.json/items.jsonl are out of date")
    a = ap.parse_args()
    folder = Path(a.folder)
    try:
        q, regions, nodes = load(folder)
        # --check only asks whether the files are current; what they say about
        # drafts is in the description either way
        cj, ij, unreviewed = convert(q, regions, nodes, a.drafts or a.check)
    except (QuestError, OSError) as e:
        sys.exit("mkquest: %s" % e)
    summary = "%d nodes in %d regions, %s, %d unreviewed" % (
        len(nodes), len(regions), "linear" if is_linear(nodes) else "branching", len(unreviewed))
    if a.check:
        for name, text in (("course.json", cj), ("items.jsonl", ij)):
            p = folder / name
            if not p.exists() or p.read_text(encoding="utf-8") != text:
                sys.exit("mkquest: %s is out of date; run tools/mkquest.py %s%s"
                         % (p, folder, " --drafts" if unreviewed else ""))
        print("mkquest: %s matches its source (%s)" % (folder, summary))
        return
    (folder / "course.json").write_text(cj, encoding="utf-8")
    (folder / "items.jsonl").write_text(ij, encoding="utf-8")
    print("mkquest: wrote %s/course.json and items.jsonl (%s)" % (folder, summary))


if __name__ == "__main__":
    main()
