# Course format — `course.srs`, version 1

> **Status: written 2026-09-30 (SRS phase S0) and implemented in S1**
> (`tools/mkcourse.py` builds it, `firmware/main/course.c` reads it). The
> owner's review of this document is still open. The format is the one
> thing that's hard to change once real courses exist, so read §2 and §3
> with that in mind. The design and the reasons behind it are in
> `SRS_PLAN.md` §4.

A **course** is what Study teaches: a few hundred to about 10,000 items
(radicals, kanji and words; or the words of a language; or plain
front/back cards), in levels, with everything the app shows about each one.

An author writes a **course source**: a folder of JSON Lines and a little
JSON (§2). `tools/mkcourse.py` turns it into **one binary file**,
`course.srs` (§3), which goes on the SD card at
`/sdcard/study/<course-id>/course.srs`. The device never loads the file
whole; it reads the piece it needs.

The worked example throughout is the demo course that ships with the
firmware, **`courses/demo-kanji/`**. Its README walks through every file.

## Contents

1. [Building a course](#1-building-a-course)
2. [The source format](#2-the-source-format)
3. [The binary format](#3-the-binary-format)
4. [Text: what is drawn how](#4-text-what-is-drawn-how)
5. [Limits](#5-limits)
6. [What the reader checks](#6-what-the-reader-checks)
7. [Changing the format](#7-changing-the-format)
8. [Authoring guide](#8-authoring-guide)
- [Appendix A: the Albanian course, mapped (a paper check)](#appendix-a-the-albanian-course-mapped-a-paper-check)

---

## 1) Building a course

```
python3 tools/mkcourse.py courses/demo-kanji                # writes courses/demo-kanji/course.srs
python3 tools/mkcourse.py courses/demo-kanji -o /tmp/x.srs  # somewhere else
python3 tools/mkcourse.py --check courses/demo-kanji/course.srs   # validate a built file, print its sizes
```

- It needs **Python 3.10+ and Pillow** (the version pinned in
  `tools/requirements-course.txt`), and only when a course has text that
  must be drawn as a picture (§4): **the Noto Sans JP font**. `mkcourse.py`
  downloads the font once, checks its SHA-256, and keeps it in
  `~/.cache/cyd-palm/`. `--font PATH` uses a copy you already have, but it
  must match the pinned hash too, so that every build of a course is
  identical to the byte.
- It **writes no timestamps** and nothing else that changes between runs.
  CI rebuilds the demo and fails if a single byte differs from the
  committed file.
- It **maintains `ids.tsv`** (§2.4). A build that adds items appends to it,
  so commit it with the course.
- It **refuses** anything the device would have to guess about: an unknown
  field, a link to a missing item, text over the size limit, a character
  the font can't draw. The message names the file, the line and the field.

## 2) The source format

A course source is a folder:

```
course.json     the course: title, licence, kinds, scheduler, levels
items.jsonl     one item per line (or cards.tsv, §2.5)
ids.tsv         source id -> number; maintained by mkcourse.py, committed
README.md       what the course is and where its content came from
course.srs      the built file (mkcourse.py writes it)
```

All text is UTF-8. Every field not marked *required* can be left out.

### 2.1 `course.json`

```json
{
  "format": 1,
  "id": "demo-kanji",
  "title": "Kanji demo",
  "version": "1.0",
  "author": "The CYD Palm project",
  "licence": "CC0-1.0",
  "language": "ja",
  "description": "Two WaniKani-style levels: radicals, then the kanji ...",
  "source": "Written for this project. Readings and meanings checked ...",
  "kinds": [
    {"name": "radical", "quiz": ["meaning"]},
    {"name": "kanji",   "quiz": ["meaning", "reading"], "level_up": true},
    {"name": "word",    "quiz": ["meaning", "reading"]}
  ],
  "roles": [],
  "scheduler": { "type": "stages", "...": "see 2.2" },
  "grading": "two",
  "lessons": {"batch": 5},
  "unlock": {"level_percent": 90, "by_links": true},
  "levels": [
    {"level": 1, "title": "First strokes", "theme": "Numbers, people and nature"},
    {"level": 2, "title": "Trees into forests", "theme": "Up, down, inside, and more trees"}
  ]
}
```

| Key | Required | What it is |
|---|---|---|
| `format` | yes | Always `1` for this version |
| `id` | yes | The course's folder name on the card: 1–31 of `a-z 0-9 . _ -` |
| `title` | yes | Shown in the course picker |
| `version`, `author`, `licence`, `language`, `description`, `source` | | Shown in the course's About box. `licence` is an SPDX id where there is one (`CC0-1.0`, `CC-BY-SA-4.0`). `language` is a BCP 47 tag (`ja`, `sq`). |
| `kinds` | yes | 1–16 item kinds, in the order the dashboard lists them. `quiz` is what a review asks: `meaning`, `reading`, or both. `level_up: true` marks the kinds that count toward unlocking the next level (§2.2). |
| `roles` | | Names for the `role` of a `built_from` link, such as `"stem"` or `"radical"` (up to 15) |
| `scheduler` | yes | §2.2 |
| `grading` | | `"two"` (Wrong / Right, the default for `stages`) or `"four"` (Again / Hard / Good / Easy, the default for `sm2`) |
| `lessons.batch` | | New items per lesson, 1–20, default 5 |
| `unlock.level_percent` | | The share of a level's `level_up` items that must be known before the next level's lessons open. Default 90; `0` opens every level at once. |
| `unlock.by_links` | | `true` (default): an item's lesson waits until everything it's `built_from` is known, as WaniKani does it |
| `render.strip` | | Side fields to strip rather than draw as a picture (§4) |
| `levels` | yes | One entry per level, §2.3 |

Everything in `course.json` except the level titles must be text the Palm
font can draw (§4), because it's shown in lists and title bars.

### 2.2 The two schedulers

**`stages`** (WaniKani-style). WaniKani's pace, which is also the default
(the demo speeds it up with a 2-hour first stage and `"known": 2`, so it can be
tried in an afternoon):

```json
"scheduler": {
  "type": "stages",
  "intervals": ["4h", "8h", "1d", "2d", "7d", "14d", "30d", "120d", null],
  "groups": [
    {"from": 1, "name": "Learning"}, {"from": 5, "name": "Known"},
    {"from": 7, "name": "Strong"},   {"from": 8, "name": "Deep"},
    {"from": 9, "name": "Retired"}
  ],
  "drop": 1, "drop_high": 2, "high_from": 5,
  "known": 5
}
```

- `intervals`: how long each stage waits before the item is due again,
  stage 1 first. `null` marks the last stage, where an item is retired
  and never reviewed again. Durations are a number and a unit: `m`, `h`,
  `d` or `w`.
- A right answer moves up one stage. A wrong one moves down `drop` stages,
  or `drop_high` from stage `high_from` and above, but never below 1.
- `known`: the stage from which an item counts as known, for unlocking.
  Stage 5 here: three and a half days of right answers.
- `groups`: names for runs of stages, for the dashboard. The demo names its
  own; WaniKani's stage names are theirs, and "Guru" is already an app on
  this device.

**`sm2`** (Anki-style). These are the values the Albanian course's web
app uses (Appendix A):

```json
"scheduler": {
  "type": "sm2",
  "ladder": ["1d", "2d", "4d", "8d", "16d"],
  "multiplier": {"hard": 0.6, "good": 1.0, "easy": 1.6},
  "ease": {"start": 2.5, "min": 1.3},
  "ease_change": {"again": -0.2, "hard": -0.15, "good": 0, "easy": 0.15},
  "relearn": "10m",
  "day_aligned": true,
  "known": 1,
  "groups": [
    {"from": "1d", "name": "Learning"}, {"from": "3d", "name": "Familiar"},
    {"from": "7d", "name": "Solid"},    {"from": "14d", "name": "Strong"},
    {"from": "21d", "name": "Mastered"}
  ]
}
```

- `ladder`: the minimum interval for each successive pass. Again returns
  an item to the first rung, due again after `relearn`.
- A pass sets the interval to the larger of the rung's and (interval ×
  ease), times the grade's `multiplier`, and changes the ease by
  `ease_change`, never below `ease.min`.
- `day_aligned: true`: an item falls due at the start of a day rather than
  to the minute.
- `known`: the rung (1 = the first) from which an item counts as known.
- `groups` go by interval here, not by stage.

Multipliers are stored in hundredths (§3.2), so a value with more than two
decimals is refused. **Eases and ease changes go in steps of 0.05** (ease
1.30 to 14.05), because progress keeps an item's ease in one byte. Anki's
values and the web app's are all multiples of 0.05. Left out, `drop` is 1,
`drop_high` 2, and `high_from` and `known` are 5, or the last stage in a
course with fewer.

`firmware/main/srs.c` implements both, in integer arithmetic. It matches the
web app's scheduler review for review over a simulated year (the `srs`
gate). The one difference is that the web app's ease is a JavaScript float
that drifts (1.35 + 0.15 is 1.4999999999999998 there), so an interval that
lands exactly on half a day can round down there and up here.

### 2.3 Levels

```json
{"level": 2, "title": "jam 2/3", "theme": "jam", "gloss": "to be", "part": 2, "parts": 3}
```

- `level` (required): 1–65535. Levels are listed in increasing order, and
  there are no empty levels.
- `title`: shown on the dashboard. It may contain any character; one the
  Palm font can't draw makes the title a picture (§4).
- `theme` and `gloss`: what the level is about, such as a root word and its
  meaning. `part`/`parts`: "part 2 of 3" when a big theme spans several
  levels.
- A course with no levels (an Anki-style deck) still has one: level 1.
  `mkcourse.py` makes it when `levels` is left out.

### 2.4 `items.jsonl`

One item per line, in the order lessons teach them within a level. A
kanji from the demo:

```json
{"id": "kanji:明", "kind": "kanji", "level": 2, "term": "明",
 "meanings": ["bright", "light"],
 "readings": [{"text": "めい", "type": "on", "primary": true},
              {"text": "みょう", "type": "on"}, {"text": "あか", "type": "kun"}],
 "mnemonic_meaning": "The sun and the moon side by side: ...",
 "mnemonic_reading": "...",
 "built_from": ["radical:日", "radical:月"],
 "rank": 67}
```

| Field | What it is |
|---|---|
| `id` (required) | Any string, unique in the course. It's how progress finds the item after the course changes, so **never change an item's id** once people are learning it. |
| `kind` (required) | One of the names in `course.json` `kinds` |
| `level` (required) | One of the levels in `course.json` |
| `term` (required) | What's being learned: 山, a word, the front of a card |
| `meanings` | The meaning, then other accepted answers. The first is shown. |
| `readings` | `{"text", "type", "primary"}`. `type` is `on`, `kun`, `nanori` or `word` (the default). One reading is `primary`: the one taught and asked for. |
| `senses` | For a word with several meanings: `{"pos", "gloss", "tags"}` (`tags` a list), in order |
| `examples` | `{"text", "translation", "sense"}`: a sentence, its translation, and optionally which sense (1 = the first) it shows |
| `mnemonic_meaning`, `mnemonic_reading` | Memory aids |
| `etymology` | Where the word comes from |
| `notes` | A list of short notes |
| `forms` | `{"tag", "form"}`: inflected forms, such as `{"tag": "plural", "form": "shoferë"}` |
| `built_from` | What it's made of: ids, or `{"id", "role"}` with a role from `course.json` `roles`. The card shows "built from"; with `unlock.by_links` it also gates the lesson. |
| `unlock_after` | Ids whose items must be known before this one's lesson, without being shown as parts |
| `family` | The id of the root of this item's word family |
| `related` | Ids to show as "see also"; no effect on unlocking |
| `rank` | Frequency rank, 1 = most common (0 or absent = none) |

A link must name an item in the same course. An item can't link to
itself, and `built_from`/`unlock_after` must not form a loop.

### 2.5 `cards.tsv` instead: plain front/back decks

A folder with `cards.tsv` instead of `items.jsonl` is a deck of one kind,
`card`, with no levels (Anki's plain export):

```
front<TAB>back
front<TAB>back<TAB>id
```

The front is the `term`, the back the one `meaning`. Without the third
column, the front is the id, so two cards can't share a front. Blank lines
and lines starting with `#` are skipped. `course.json` is still needed for
the title and scheduler, and its `kinds` may be left out.

### 2.6 `ids.tsv`

```
# source id<TAB>number   (maintained by mkcourse.py; commit it)
radical:一	1
radical:丨	2
```

Each item's id gets a 32-bit number the first time the course is built.
Progress on the card is stored by that number. `mkcourse.py` appends new
ids at the end, numbered from one past the highest ever used. It **never
renumbers or reuses a number**, even when an item is deleted, so a rebuilt
course keeps everyone's progress. A line of an id that no longer exists
stays in the file, and that number stays retired.

## 3) The binary format

**All integers are little-endian.** Offsets are counted from the start of
the file (sections) or the start of their section (everything inside
one). Every section starts on a multiple of 4. The bytes between sections,
and after a bitmap, are zero.

CRC-32 is the common one (zlib's, polynomial `0xEDB88320`, initial and
final XOR `0xFFFFFFFF`).

### 3.1 The file

```
header         32 B
section table  16 B x section count
META LEVL ITEM TEXT BMP LINK IDX   (in this order, each 4-aligned)
```

**Header (32 B, at 0):**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `SRSC` |
| 4 | 2 | Format major version: `1`. A reader refuses any other. |
| 6 | 2 | Format minor version: `0`. Additions an older reader may ignore (§7). |
| 8 | 4 | File size in bytes: a truncated copy is refused at once |
| 12 | 2 | Section count (5–16) |
| 14 | 2 | Reserved, 0 |
| 16 | 4 | Item count |
| 20 | 4 | Level count |
| 24 | 4 | CRC-32 of the section table. This is also the course's **fingerprint**: it changes when anything in the course changes, which tells the app to remap progress (`SRS_PLAN.md` §4.4). |
| 28 | 4 | CRC-32 of bytes 0–27 |

**Section table (at 32, 16 B per section):**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Tag: `META`, `LEVL`, `ITEM`, `TEXT`, `BMP␠`, `LINK` or `IDX␠` (␠ is a space) |
| 4 | 4 | Offset in the file |
| 8 | 4 | Length in bytes |
| 12 | 4 | CRC-32 of the section's bytes |

`META`, `LEVL`, `ITEM`, `TEXT` and `IDX ` are required; `BMP ` and `LINK`
may be missing or empty. A reader skips a tag it doesn't know.

### 3.2 `META`: the course's settings

A list of entries, one after another, to the end of the section:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Key |
| 2 | 2 | Length *n* of the value |
| 4 | *n* | Value |

A value is **text** (UTF-8, no NUL, every character in U+0020–U+00FF),
**numbers** (*n*/4 unsigned or signed 32-bit integers), or a small record.
A key that appears once may appear only once; a reader skips a key it
doesn't know.

| Key | Value | From `course.json` |
|---|---|---|
| `0x0001` | text | `id` |
| `0x0002` | text | `title` |
| `0x0003` | text | `version` |
| `0x0004` | text | `author` |
| `0x0005` | text | `licence` |
| `0x0006` | text | `language` |
| `0x0007` | text | `description` |
| `0x0008` | text | `source` |
| `0x0010` | u8 quiz (bit 0 meaning, bit 1 reading), u8 flags (bit 0 `level_up`), then the name as text. **Repeated, once per kind, in order: the first is kind 0.** | `kinds` |
| `0x0011` | text. Repeated: the first is role 1 (0 means no role). | `roles` |
| `0x0012` | u32 from, then the name as text. Repeated, in order. `from` is a stage (stages) or an interval in hours (sm2). | `scheduler.groups` |
| `0x0020` | u32: 1 = stages, 2 = sm2 | `scheduler.type` |
| `0x0021` | u32: 2 or 4 buttons | `grading` |
| `0x0022` | u32 | `lessons.batch` |
| `0x0023` | u32 percent | `unlock.level_percent` |
| `0x0024` | u32: 1 = yes | `unlock.by_links` |
| `0x0025` | u32 | `scheduler.known` |
| `0x0030` | u32 seconds per stage, stage 1 first; 0 = retired (the last) | `intervals` (stages) |
| `0x0031` | u32 × 3: drop, drop_high, high_from | stages |
| `0x0040` | u32 seconds per rung | `ladder` (sm2) |
| `0x0041` | u32 × 3, hundredths: hard, good, easy | `multiplier` |
| `0x0042` | u32 × 2, hundredths, multiples of 5: start, min (130–1405) | `ease` |
| `0x0043` | i32 × 4, hundredths, multiples of 5: again, hard, good, easy | `ease_change` |
| `0x0044` | u32 seconds | `relearn` |
| `0x0045` | u32: 1 = yes | `day_aligned` |
| `0x0050` | u32 × 4: wrap width, then the prompt, word and text heights, in pixels | how the pictures were drawn (§4) |

Required: `0x0001`, `0x0002`, at least one `0x0010`, `0x0020`, and the
scheduler's own keys (`0x0030` for stages; `0x0040`–`0x0042` for sm2). The
rest have the defaults given in §2.

### 3.3 `LEVL`: the levels (24 B each)

Level count entries, in increasing level number:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Level number (1–65535) |
| 2 | 1 | Part (1 if none) |
| 3 | 1 | Parts (1 if none) |
| 4 | 4 | First item: the level's items are items *first* to *first* + *count* − 1 |
| 8 | 4 | Item count (≥ 1) |
| 12 | 4 | Offset of its text in `TEXT` |
| 16 | 2 | Length of its text |
| 18 | 2 | Reserved, 0 |
| 20 | 4 | CRC-32 of its text |

The levels cover the item table in order: level 1 starts at item 0, and
each level starts where the one before ended. The text holds the level's
`title`, `theme` and `gloss` as fields (§3.5).

### 3.4 `ITEM`: the items (32 B each)

Item count entries, sorted by level and then source order, so item *n* is
at `ITEM + 32n`, a single seek:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Id (the number from `ids.tsv`; 1 or more, unique) |
| 4 | 2 | Level number |
| 6 | 1 | Kind (0 = the first `0x0010` entry in `META`) |
| 7 | 1 | Flags, 0 (reserved) |
| 8 | 4 | Offset of its text in `TEXT` |
| 12 | 2 | Length of its text (1–4096) |
| 14 | 2 | Number of links (0–64) |
| 16 | 4 | Offset of its first link in `LINK` (a multiple of 8) |
| 20 | 2 | Frequency rank (0 = none; ranks over 65535 are stored as 65535) |
| 22 | 2 | Reserved, 0 |
| 24 | 4 | CRC-32 of its text followed by its links |
| 28 | 4 | Reserved, 0 |

One card is two reads: this entry, then its text and links, which sit
side by side in the read buffer, checked by this CRC.

### 3.5 `TEXT`: fields

Every item's and level's text is a run of **fields**:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Tag |
| 1 | 1 | Attributes |
| 2 | 2 | Length *n* of the text |
| 4 | 4 | *Only when attribute bit 7 is set:* offset of the field's picture in `BMP` |
| 4 or 8 | *n* | The text, UTF-8, no NUL |

**Attributes:**

| Bits | Meaning |
|---|---|
| 7 | **Picture:** draw the bitmap, not the text (§4) |
| 6 | **Primary:** the meaning or reading that's shown and asked for |
| 5 | **Kana:** the text is all kana; draw it with the 38 px kana font |
| 0–4 | A number that depends on the tag (below) |

The text is always there, even when a picture is drawn, so answers can
be checked against it and it can be searched.

**Tags:**

| Tag | Name | Repeats | Bits 0–4 | Text |
|---|---|---|---|---|
| `0x01` | TERM | exactly one, first | | What's being learned |
| `0x02` | MEANING | yes | | A meaning; the primary one is shown |
| `0x03` | READING | yes | 0 word, 1 on'yomi, 2 kun'yomi, 3 nanori | A reading |
| `0x04` | SENSE | yes | sense number (1–31, 0 past 31) | `pos` U+001F `gloss`, and U+001F `tags` (comma-separated) when there are tags |
| `0x05` | EXAMPLE | yes | the sense it shows (0 = any) | A sentence |
| `0x06` | TRANSLATION | after an EXAMPLE | | The example's translation |
| `0x07` | MNEMONIC_MEANING | once | | |
| `0x08` | MNEMONIC_READING | once | | |
| `0x09` | ETYMOLOGY | once | | |
| `0x0A` | NOTE | yes | | |
| `0x0B` | FORM | yes | | `tag` U+001F `form` |
| `0x20` | LEVEL_TITLE | level text only | | |
| `0x21` | LEVEL_THEME | level text only | | |
| `0x22` | LEVEL_GLOSS | level text only | | |

A reader skips a tag it doesn't know. U+001F is the only character below
U+0020 allowed, and only in SENSE and FORM.

### 3.6 `BMP `: pictures

Each picture starts on a multiple of 4:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Width in pixels (1–240) |
| 2 | 2 | Height in pixels (1–1024) |
| 4 | 1 | Bits per pixel: 1 (v1 writes only 1; 2 is reserved for S3's readability check) |
| 5 | 1 | Flags, 0 |
| 6 | 2 | Bytes per row: (width × bits per pixel + 7) / 8 |
| 8 | 4 | CRC-32 of the pixel rows |
| 12 | rows | Height × bytes per row. Rows top to bottom, the leftmost pixel in the high bit, 1 = ink. This is LVGL's A1 layout, so a row strip can be shown as it is read. |

A picture is cropped to its ink. The app draws a tall one in strips, so
its buffer stays small. Fields may share a picture: the radical 一 and the
kanji 一 point at the same one.

### 3.7 `LINK`: links (8 B each)

An item's links are consecutive, in the order written in the source:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | The linked item's number (its position in `ITEM`, not its id) |
| 4 | 1 | Type: 1 unlock after, 2 built from, 3 family, 4 related |
| 5 | 1 | Role (0 = none, else a `0x0011` entry in `META`) |
| 6 | 2 | Reserved, 0 |

### 3.8 `IDX `: ids to items (8 B each)

Item count entries, **sorted by id**: u32 id, then u32 item number. The
app finds an item from its progress record's id by binary search here, and
reads progress and `IDX` side by side (both sorted by id) to find what's
due (`SRS_PLAN.md` §5).

## 4) Text: what is drawn how

The device draws three kinds of text; `mkcourse.py` decides which for
each field and records it in the attributes (§3.5):

1. **The Palm fonts**, when every character is in U+0020–U+00FF: English,
   and the Latin-1 languages (French, German, Spanish, Italian, Portuguese,
   Dutch, the Nordic languages, and Albanian with ë, ç, Ë and Ç, whose
   glyphs were checked in both the regular and bold font). Before deciding,
   two things happen:
   - **Folding**, in every field: curly quotes to `"` and `'`, en and em
     dashes to `-`, `…` to `...`, a no-break space to a space, the
     fraction slash to `/`, subscript and superscript digits to digits
     (CO₂ to CO2), arrows to `->` and `<-`, and line breaks and tabs to a
     space. A field is one flowing paragraph.
   - **Latin fallback**, in every field except TERM and READING: a Latin
     letter outside Latin-1 becomes the letters it's built on, as a Palm
     did it: č to c, ā to a, ſ to s, ʰ to h (á, ë and the rest of Latin-1
     stay as they are). A gloss that mentions "Dvořák" shows "Dvorák". TERM
     and READING must be exact, so there such a letter makes a picture, or
     fails the build if the font lacks it.
2. **The kana font** (38 px, attribute bit 5), when every character is
   hiragana or katakana (U+3041–U+3096, U+309B–U+309E, U+30A1–U+30FE):
   readings, and words written in kana.
3. **A picture** (attribute bit 7) for anything else: kanji, radicals,
   Greek, Cyrillic, Arabic. It's drawn with Noto Sans JP on the computer:

   | Field | Size | Width |
   |---|---|---|
   | TERM of one character | 64 px | |
   | TERM of more | 40 px, made smaller until it fits | 232 px |
   | Everything else | 24 px, wrapped | 232 px |

   The sizes and wrap width are recorded in `META` (`0x0050`). A
   character the font doesn't have fails the build.

   **What the font covers** (measured): Japanese (kana, kanji, full-width
   forms), Cyrillic, and Greek **without accents** (Α–ω, but not ά), as
   Japanese character sets have them. It has **no Arabic or Hebrew**, and
   drawing those needs text shaping the build deliberately turns off. So a
   course with Arabic, Hebrew or accented Greek puts those fields in
   `render.strip`, which suits etymologies (below). A second pinned font
   can be added later if a course needs one; that's a builder change, not
   a format change.

**Stripping instead of a picture.** A course can list side fields in
`render.strip` (`etymology`, `note`, `example`, `translation`). In those,
runs the Palm font can't draw are dropped, along with the brackets or
quotes around them if nothing else is left inside, and the rest stays
text: `"Ottoman Turkish حال (hal, 'situation')"` becomes `"Ottoman Turkish
(hal, 'situation')"`. Wiktionary gives a transliteration next to foreign
script, which is why this reads well. TERM, MEANING and READING are never
stripped. **A stripped field with no letter or digit left is dropped**, and
a dropped example takes its translation with it; the build says how many.

## 5) Limits

| | Limit | Why |
|---|---|---|
| Items | 65,535 | The app keeps due lists as 2-byte item numbers |
| Levels | 65,535 | 2-byte level numbers (the Albanian example has 592) |
| One item's text | 4,096 B | One item is read whole into a heap buffer (`SRS_PLAN.md` §7) |
| Links per item | 64 | 512 B beside the text in the same buffer |
| Kinds | 16 | |
| Roles, groups | 15 each | |
| Picture | 240 × 1024 px | The screen's width. A longer text is wrapped, not widened. |
| `META` | 4,096 B | Read whole on open |
| The file | 2 GB | FAT32's limit is 4 GB; offsets are u32 |

`mkcourse.py` refuses a course over any limit and says which item.

## 6) What the reader checks

`firmware/main/course.c` treats the file as untrusted. A damaged or
hostile file gets a message, never a crash or a read outside a buffer; the
fuzz gate proves this on thousands of damaged copies under ASan and UBSan.

**On open** (cheap: a few KB, plus `IDX`, which the due scan reads
anyway):
- the magic, the major version, the header's CRC and the file size;
- the section table: its CRC, every section inside the file, 4-aligned,
  in order and not overlapping, the required ones present, every entry
  size a whole number of records;
- the CRCs of `META`, `LEVL` and `IDX`, and their contents: required keys,
  values in range, levels increasing and covering the items exactly, ids
  sorted and unique, item numbers in range.

**On each item** (the two reads for one card):
- its entry's offsets and lengths against their sections;
- the CRC of its text and links, before anything is shown;
- each field's length against the text, its text (strict UTF-8, and only
  characters the way it's drawn allows), and its picture's offset against
  `BMP`;
- each link's target against the item count.

**On each picture**, when it's drawn: its header (size, bits per pixel, row
length) and that its rows fit in `BMP`. Its CRC can be checked only once all
its rows are read, so drawing strips doesn't wait for it; `course_verify()`
checks every picture's.

**The whole file** (`course_verify()`, which is `mkcourse.py --check` on
the device, and a "Check course" menu item in S3): every section's CRC,
every item and level as above, every picture's CRC, zero padding, and
that `ITEM` and `IDX` agree. `ITEM`, `TEXT` and `BMP` are too big to CRC
on every open (a real course is 1–20 MB), which is why each item and
picture carries its own CRC.

## 7) Changing the format

- **Minor version (still `1.x`):** new `META` keys, new field tags, new
  sections, and meanings for reserved bits that an older reader can safely
  ignore. An older reader skips what it doesn't know, and still shows
  the course correctly without it.
- **Major version:** anything an older reader would misread. It refuses
  the file with "This course needs a newer Study", and `mkcourse.py`
  keeps being able to write the older version for as long as devices in
  the field need it.
- **Never:** reusing a tag, key or bit for something else.

## 8) Authoring guide

**Start from the demo.** Copy `courses/demo-kanji/` to
`courses/<your-id>/`, delete `ids.tsv` and `course.srs`, and edit
`course.json` (a new `id` and `title` first). Then replace `items.jsonl`.

**Write items in teaching order.** Within a level, lessons follow the file.
The demo lists radicals, then the kanji built from them, then words, as
WaniKani does. With `unlock.by_links`, order matters less: a kanji isn't
offered until its radicals are known, wherever it sits in the file.

**Keep it short.** The screen is 240 × 320 and shows about 12 lines of
text. A meaning of one to four words; a mnemonic of two or three
sentences; examples short enough to fit on two lines. `--check` prints the
largest items.

**Give answers as a list.** `meanings` is what a typed answer (S4) will be
matched against. Put the one to show first and the other acceptable ones
after it: `["bright", "light"]`.

**Check facts against open sources.** For Japanese, KANJIDIC2 and JMdict
(EDRDG, CC BY-SA 4.0); for other languages, Wiktionary (CC BY-SA). The
demo's readings and meanings were checked against KANJIDIC2 and JMdict;
its radical names, mnemonics and sentences are its own.

**Mind the licence.** A course built from WaniKani, a textbook or another
app's deck is for your own card. Don't publish it. If you share a course,
say where its content came from in `source` and in the README, and give
it a licence that allows sharing.

**Build and check:**

```
python3 tools/mkcourse.py courses/<your-id>
python3 tools/mkcourse.py --check courses/<your-id>/course.srs
```

Then copy the folder's `course.srs` to `/sdcard/study/<your-id>/` on the
card. Updating a course is the same copy; progress follows the ids.

---

## Appendix A: the Albanian course, mapped (a paper check)

The owner's Albanian course,
[androidbot18/albcourse](https://github.com/androidbot18/albcourse), is
still being built. It's used **only to check that this format fits a real
course**: none of its data is in this repo or in any test. Read on
2026-09-30 at commit `82ac18d`: **4,087 words in 592 levels** (it was
3,731 in 543 on 2026-09-29; the format's limits don't come close).

**Its levels** (`data/index.json`: `level`, `title`, `root`,
`root_gloss`, `part`, `parts`):

| Its field | Here |
|---|---|
| `level` | `levels[].level` |
| `title`, such as `"jam 2/3"` | `levels[].title` |
| `root`, `root_gloss` | `levels[].theme`, `levels[].gloss` |
| `part`, `parts` | `levels[].part`, `levels[].parts` |

**Its words** (`data/levels/level_NNN.json`, `words[]`):

| Its field | Here | Notes |
|---|---|---|
| `id` (the word itself, `"bëj"`) | `id` | String ids are what `ids.tsv` is for |
| `sq` | `term` | Latin-1: drawn with the Palm font |
| `en` | `meanings` | `glosses` repeats the senses' glosses, so it isn't stored twice |
| `sense_detail[]`: `pos_label`, `gloss`, `tags` | `senses[]`: `pos`, `gloss`, `tags` | |
| `sense_detail[].examples[]`: `sq`, `en` | `examples[]`: `text`, `translation`, `sense` | The `offsets` that underline the word in the sentence have no field. The app can find the term in the sentence itself. |
| `corpus_example` (`sq`, `en`) | one more `examples[]` entry, with no `sense` | |
| `etymology` | `etymology` | Quotes accented Greek, Old Albanian and Arabic script, none of which the picture font has, so it goes in `render.strip` (§4) |
| `forms[]`: `form`, `tag` | `forms[]`: `form`, `tag` | |
| `family`, `is_root` | `family` (the root's id); the root has none | |
| `components[]`: `word`, `role` | `built_from[]`: `id`, `role` | Its roles (such as `prefix`) become the course's `roles` |
| `parents` | a `note`, or nothing | They're English words (the dictionary's etymology parents), not items in the course |
| `derived` | nothing to store: it's the other direction of `family` and `built_from`, which the app can list | |
| `related` | `related` | Only the words that are items in the course: a link must name an item, so a converter drops the rest |
| `rank`, `count` | `rank` | `count` (corpus frequency) has no field |
| `pos`, `pos_labels` | covered by `senses[].pos` | |
| `sense_detail[].cats`, `links`, `etymology_class`, `source_lang`, `level` | not needed on the device | |

**Its scheduler** (`app/js/srs.js`) is the `sm2` example in §2.2, value
for value: a ladder of 1, 2, 4, 8 and 16 days; ×0.6, ×1.0 and ×1.6 for
Hard, Good and Easy; ease from 2.5, never below 1.3, with −0.2, −0.15, 0
and +0.15; a relearn after 10 minutes; due dates at the start of a day;
and the same bands at 1, 3, 7, 14 and 21 days. Grading is four buttons.

**What doesn't fit, and why that's all right:**
- the sentence underline (`offsets`), said above;
- `related` and `derived` words that aren't in the course, which have
  nothing to link to;
- corpus counts and the dictionary's categories, which the app doesn't
  show;
- its web app keeps progress in the browser, so there's nothing to carry
  over: a learner starts fresh on the device.

**Sizes, encoded as above:** the largest word, `marr` with 24 senses, is
3,439 B, inside the 4,096 B item limit. (Storing `glosses` as extra
meanings as well would have taken it to 4.8 KB, over the limit, which is
why the mapping doesn't.) No word has more than 24 senses, inside the 31
that bits 0–4 number.

The paper check found no reason to change the format.

**Then a real build** (2026-09-30, in a scratch folder that was deleted
afterwards; nothing entered this repo). A converter written to the mapping
above turned the whole course into a source, and `mkcourse.py` built it:
- **4,087 items in 592 levels, 1,394,508 B, with not one picture.** Every
  field is Palm-font text. `course_verify()` passed on it, and the C and
  Python readers dumped it identically. The largest item is 3,343 B.
- **Albanian's own letters (ë, ç, Ë, Ç) are all Latin-1.** No character in
  any word, form, level title or root is outside the Palm font.
- What isn't Latin-1 is quoted from other languages:
  - Greek, Arabic, Cyrillic and Old Albanian scripts in etymologies, and in
    a few historical examples;
  - č, ć and š in a few glosses;
  - CO₂-style subscripts and a fraction slash in two meanings;
  - 370 line breaks in etymologies.
  Folding and the Latin fallback (§4) handle the glosses, the meanings and
  the line breaks.
- The settings it needs: `"render": {"strip": ["etymology", "example",
  "translation"]}`. With those, 7 fields had nothing readable left and were
  dropped: examples written wholly in the Greek alphabet, and their
  translations.
- The converter has to leave out examples whose translation is empty (the
  builder refuses empty text).
