# Counting to percent — Knowledge's pilot course

Forty steps in one line, from counting to percent change. Every step builds
on the one before it: step 2 opens when step 1's check is passed, and so on
to step 40. It's the K0 pilot of `docs/KNOWLEDGEQUEST_PLAN.md`, and it is what
the launcher's tree (Knowledge, position 6) opens. The firmware carries it and
writes it to `/sdcard/study/kq-numbers/` the first time Knowledge opens.

**Status: draft, not yet reviewed by a person.** The cards were written by
Claude (the model) on 2026-11-07 and every answer was checked by hand, but no
person has read them as a learner. The build says so: the course's version
is `0.1-draft` (the About box shows it) and its description ends "Draft: not
yet reviewed by a person." To review a card, read it on the device or in
`nodes.jsonl`, fix what needs fixing, and set its `"review"` to `"reviewed"`.
When all 40 are reviewed, rebuild without `--drafts`.

| Part | Steps |
|---|---|
| 1 Counting and place value | 1 Counting · 2 Zero · 3 The number line · 4 Adding · 5 Subtracting · 6 Tens · 7 Place value · 8 Adding by place · 9 Equal groups · 10 Sharing equally |
| 2 Fractions | 11 Remainders · 12 Fractions · 13 Top and bottom · 14 Fractions on the line · 15 Whole ones · 16 Same amount, new name · 17 Simplest form · 18 Comparing fractions · 19 Adding fractions · 20 A fraction of an amount |
| 3 Ratios and rates | 21 What fraction is it? · 22 Ratio · 23 Ratio to fraction · 24 Equivalent ratios · 25 Simplest ratio · 26 Sharing in a ratio · 27 Unit rate · 28 Using a unit rate · 29 Speed · 30 Distance from speed |
| 4 Proportion and percent | 31 Proportional · 32 The constant · 33 Is it proportional? · 34 The graph of a proportion · 35 Map scales · 36 Scale factor · 37 Percent · 38 Fraction to percent · 39 Percent of an amount · 40 Percent change |

## A step

Each line of `nodes.jsonl` is one concept: a **title**, the **idea** in one
line, an **explanation** of two to four sentences that starts from the step
before, and one **question** with a short **answer**. On the device the card
shows the title, the idea, "Builds on: <the step before>", the explanation,
and "Try it" with the question and its answer. The check after it asks the
same question; the answer is self-graded (Wrong / Right), as Study's are.
Explored steps come back as reviews a day, three days, a week, three weeks
and two months later, then retire. They're light on purpose: one question each.

## Building it

```
python3 tools/mkquest.py courses/kq-numbers --drafts   # nodes.jsonl + quest.json -> course.json + items.jsonl
python3 tools/mkcourse.py courses/kq-numbers           # -> course.srs (needs the pinned Pillow)
python3 tools/gen_study_demo.py                        # -> firmware/main/study_demo.c
```

`course.json`, `items.jsonl` and `course.srs` are build products, committed so
the firmware and the gates don't need the tools; `make -C sim course` and CI
fail if any of them is out of date with `nodes.jsonl`. `ids.tsv` keeps each
step's number for good: **never change a node's `id`** once people are
learning it, or their progress on it is lost.

## Licence and sources

**CC0-1.0**: no rights reserved. The path (number, fractions, ratios,
proportion) is the one the plan picked from the `withmarbleapp/os-taxonomy`
graph's Mathematics, but no text, structure or ids were taken from the
taxonomy, so its ODbL / CC BY-SA terms don't apply. A course built from the
taxonomy itself would have to carry them (`KNOWLEDGEQUEST_PLAN.md` §3).
