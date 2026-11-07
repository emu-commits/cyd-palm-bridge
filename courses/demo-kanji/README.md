# Kanji demo — the course that ships with Study

Two WaniKani-style levels of Japanese: kanji, each learned by its meaning
and its sound (reading), then words that use them. There are no radicals:
the course teaches the kanji themselves. The firmware carries this course and
installs it on the SD card the first time Study opens (`SRS_PLAN.md` §9).
It also serves as **the worked example for writing a course**: copy this
folder to start your own. The format itself is `docs/COURSE_FORMAT.md`.

| Level | Kanji | Words |
|---|---|---|
| 1 "First strokes" | 一 二 三 十 人 口 大 山 日 月 木 本 | 20, from 一つ to 二日 |
| 2 "Trees into forests" | 上 下 中 目 田 力 男 休 体 林 森 明 | 20, from 上 to 日中 |

That's 64 items: 24 kanji and 40 words. Some level 2 words use level 1
kanji (明日 is 明 + 日), so unlocking across levels gets exercised.

The first version had 14 radicals in front of the kanji. They were taken out
on 2026-11-07 (the owner: learn only the sound and meaning of the kanji).
Their lines stay in `ids.tsv`, so their numbers are never reused.

## Licence and sources

**CC0-1.0**: no rights reserved. Copy it, change it, and build your own course
on it without asking.

- **Readings and meanings are facts, checked against KANJIDIC2 and JMdict**
  (the EDRDG's dictionaries). A script compared every kanji reading with
  KANJIDIC2 and every word's written form and reading with JMdict; all of
  them match. Meanings are the dictionaries' where they fit a flashcard, with
  a few extra accepted answers (such as "woods" for 林).
- **Mnemonics and example sentences were written for this course.** None
  of it comes from WaniKani, whose content belongs to Tofugu:
  - The stage names are the course's own (Learning, Known, Strong, Deep,
    Retired). Using WaniKani's would also clash with the Guru app on this
    device.
- **The kanji pictures** are drawn from Noto Sans JP (SIL Open Font
  Licence), which `mkcourse.py` uses on the computer. The font itself isn't
  in the course; only the pixels are.

## The files

### `course.json`: the course

- **`id`**, `demo-kanji`: the folder on the card (`/sdcard/study/demo-kanji/`)
  and where progress is kept.
- **`kinds`**: `kanji` and `word`, both quizzed on meaning and reading. `"level_up": true` on `kanji` means the next level
  opens when 90 % of this level's kanji are known (`unlock.level_percent`).
- **`scheduler`**: `stages`, WaniKani's intervals (4 h, 8 h, 1 day, 2 days,
  1 week, 2 weeks, a month, 4 months, then retired) **with a 2-hour stage in
  front**. A wrong answer drops an item one stage, or two once it's at stage
  6 (a week) or above. `known: 2` is the stage at which an item counts as
  known, which is what opens the lessons built on it: here, one right
  answer.
- **`unlock.by_links`**: a word's lesson waits until its kanji are known. The `built_from` links in `items.jsonl`
  say which.
- **`grading: "two"`**: Wrong / Right, as WaniKani does it.
- **`levels`**: a title and theme for each.

**The demo is faster than WaniKani, so it can be tried in an afternoon.**  A
level's kanji are all open from the start; a word's lesson opens as soon as
its kanji have been answered right once, 2 hours after their lesson. Level
2 opens once 11 of level 1's 12 kanji (90 %, rounded up) have been answered
right once. At WaniKani's own pace (its intervals without the 2-hour stage,
and `known: 5`, which are the format's defaults), each of those steps takes
three and a half days. A real course would normally use that; the demo's
job is to show every kind of lesson on the first day.

### `items.jsonl`: the items, one per line

Lessons follow the file within a level: kanji first, then words. One line
of each kind:

```json
{"id": "kanji:本", "kind": "kanji", "level": 1, "term": "本", "meanings": ["book", "origin", "main", "true"], "readings": [{"text": "ほん", "type": "on", "primary": true}, {"text": "もと", "type": "kun"}], "mnemonic_meaning": "A tree with a line across its root: ...", "mnemonic_reading": "Bring the books home: hon.", "rank": 10}
```

- **`id`** is any text unique in the course. The kind is put in front
  (`kanji:`, `word:`) because 上 is a kanji and a word here.

- **`meanings`**: the first is shown; the rest are accepted answers.
- **`readings`**: every reading, typed `on` or `kun`. The **primary** one is
  taught and asked for, as WaniKani does it.
- **Readings inside mnemonics are in romaji** ("hon"), not kana. A field
  that mixes English and kana would have to be stored as a picture (about
  1.7 KB for a two-line mnemonic; written that way, 24 mnemonics and 6
  notes took the course from 82 KB to 142 KB). The card shows the reading in kana right
  beside the mnemonic anyway.
- **`rank`**: KANJIDIC2's frequency rank (1 = the most common kanji in
  newspapers).

```json
{"id": "word:明日", "kind": "word", "level": 2, "term": "明日", "meanings": ["tomorrow"], "readings": [{"text": "あした", "type": "word", "primary": true}], "mnemonic_meaning": "The next bright day: tomorrow.", "examples": [{"text": "明日またね。", "translation": "See you tomorrow."}], "notes": ["A special reading of the whole word: ashita. Myounichi is the formal one."], "built_from": ["kanji:明", "kanji:日"]}
```

- **`examples`**: a sentence and its translation. Sentences use the course's
  kanji and otherwise kana, so they're readable at this stage.
- **`notes`**: short and practical. This one points out a special reading.
- A word's `built_from` lists its kanji, so its lesson waits for them.

### `ids.tsv`: ids to numbers

Written by `mkcourse.py` the first time the course is built, and committed.
Progress on the card is stored by these numbers, so they never change: a
rebuilt or extended course keeps everyone's progress. Don't edit it by hand;
if you delete an item, its line stays and its number is never reused.

### `course.srs`: the built course

What goes on the card: 79,264 bytes, and **83 % of it is pictures** (88 of
them, 66,156 B). There's one for each kanji and word written in kanji (the
kanji 上 and the word 上 share one), and one for each example sentence. Everything else is text: meanings, mnemonics and notes in the Palm
font, readings in the kana font. `mkcourse.py --check` prints the breakdown.

## Building it

```
pip install -r tools/requirements-course.txt     # the pinned Pillow
python3 tools/mkcourse.py courses/demo-kanji
python3 tools/mkcourse.py --check courses/demo-kanji/course.srs
```

The first build downloads the pinned font into `~/.cache/cyd-palm/`. **The
build is reproducible:** CI rebuilds this course and fails if a single byte
differs from the committed `course.srs`. So after editing `items.jsonl`,
rebuild and commit `course.srs` and `ids.tsv` together with it.

## Making your own from this

1. Copy this folder to `courses/<your-id>/`, and delete `ids.tsv` and
   `course.srs`.
2. In `course.json`, change `id`, `title`, `author`, `description`,
   `source` and `licence` first, then the levels.
3. Replace `items.jsonl`. Keep the fields you need and drop the rest; only
   `id`, `kind`, `level` and `term` are required, plus what each kind is
   quizzed on.
4. Build, check, and copy `course.srs` to `/sdcard/study/<your-id>/`.

`docs/COURSE_FORMAT.md` §8 has the rest of the advice: keep text short for a
240 × 320 screen, list every acceptable answer, check facts against open
dictionaries, and state where the content came from.
