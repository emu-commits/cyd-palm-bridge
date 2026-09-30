# Kanji demo — the course that ships with Study

Two WaniKani-style levels of Japanese: radicals, then the kanji built from
them, then words that use the kanji. The firmware carries this course and
installs it on the SD card the first time Study opens (`SRS_PLAN.md` §9).
It also serves as **the worked example for writing a course**: copy this
folder to start your own. The format itself is `docs/COURSE_FORMAT.md`.

| Level | Radicals | Kanji | Words |
|---|---|---|---|
| 1 "First strokes" | 一 十 人 口 山 日 月 木 | 一 二 三 十 人 口 大 山 日 月 木 本 | 20, from 一つ to 二日 |
| 2 "Trees into forests" | 丨 卜 目 田 力 亻 | 上 下 中 目 田 力 男 休 体 林 森 明 | 20, from 上 to 日中 |

That's 78 items: 14 radicals, 24 kanji and 40 words. Every radical is used by
a kanji, and some level 2 kanji are built from level 1 radicals (休 is 亻 +
木, 明 is 日 + 月), so unlocking across levels gets exercised.

## Licence and sources

**CC0-1.0**: no rights reserved. Copy it, change it, and build your own course
on it without asking.

- **Readings and meanings are facts, checked against KANJIDIC2 and JMdict**
  (the EDRDG's dictionaries). A script compared every kanji reading with
  KANJIDIC2 and every word's written form and reading with JMdict; all of
  them match. Meanings are the dictionaries' where they fit a flashcard, with
  a few extra accepted answers (such as "woods" for 林).
- **Radical names, mnemonics and example sentences were written for this
  course.** None of it comes from WaniKani, whose content belongs to Tofugu:
  - Radicals are named for what they mean or look like. 卜 is "signpost"
    (its old meaning, "divination", is also accepted), and 亻 is "standing
    person".
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
- **`kinds`**: `radical` is quizzed on its meaning; `kanji` and `word` on
  meaning and reading. `"level_up": true` on `kanji` means the next level
  opens when 90 % of this level's kanji are known (`unlock.level_percent`).
- **`scheduler`**: `stages`, WaniKani's intervals (4 h, 8 h, 1 day, 2 days,
  1 week, 2 weeks, a month, 4 months, then retired). A wrong answer drops an
  item one stage, or two once it's at stage 5 or above. `known: 5` is the
  stage at which an item counts as known, which is what opens the lessons
  built on it.
- **`unlock.by_links`**: a kanji's lesson waits until its radicals are known,
  and a word's until its kanji are. The `built_from` links in `items.jsonl`
  say which.
- **`grading: "two"`**: Wrong / Right, as WaniKani does it.
- **`levels`**: a title and theme for each.

This is WaniKani's pace: a kanji's lesson opens three and a half days after
its radicals' lesson at the soonest (4 h + 8 h + 1 day + 2 days of right
answers take them to stage 5). That's slow for trying the app out, but
it's what the demo is showing. A course that wants to move faster lowers
`known`.

### `items.jsonl`: the items, one per line

Lessons follow the file within a level: radicals first, then kanji, then
words. Three lines, one of each kind:

```json
{"id": "radical:木", "kind": "radical", "level": 1, "term": "木", "meanings": ["tree"], "mnemonic_meaning": "A trunk, two branches spreading down like roots: a tree."}
```

- **`id`** is any text unique in the course. The kind is put in front
  (`radical:`, `kanji:`, `word:`) because 木 is a radical, a kanji and a
  word here.
- A radical is quizzed only on its meaning, so it needs no reading.

```json
{"id": "kanji:本", "kind": "kanji", "level": 1, "term": "本", "meanings": ["book", "origin", "main", "true"], "readings": [{"text": "ほん", "type": "on", "primary": true}, {"text": "もと", "type": "kun"}], "mnemonic_meaning": "A tree with a line across its root: ...", "mnemonic_reading": "Bring the books home: hon.", "built_from": ["radical:木", "radical:一"], "rank": 10}
```

- **`meanings`**: the first is shown; the rest are accepted answers.
- **`readings`**: every reading, typed `on` or `kun`. The **primary** one is
  taught and asked for, as WaniKani does it.
- **Readings inside mnemonics are in romaji** ("hon"), not kana. A field
  that mixes English and kana would have to be stored as a picture (about
  1.7 KB for a two-line mnemonic; written that way, 24 mnemonics and 6
  notes took the course from 82 KB to 142 KB). The card shows the reading in kana right
  beside the mnemonic anyway.
- **`built_from`**: the radicals, in the order they're written. The card
  shows them, and with `unlock.by_links` they gate the lesson.
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

What goes on the card: 81,756 bytes, and **82 % of it is pictures** (91 of
them, 66,784 B). There's one for each kanji, radical and word written in
kanji (the radical 一 and the kanji 一 share one), and one for each example
sentence. Everything else is text: meanings, mnemonics and notes in the Palm
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
