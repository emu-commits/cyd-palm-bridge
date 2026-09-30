# SRS plan — Study, a built-in spaced-repetition app

> **Status: S0 to S5 done in the simulator, 2026-09-30.** S4 was replanned
> the same day: no typed answers (§11 decision 9). S5 put the reviews due
> on the lock screen and the launcher, and a forecast on the Week screen
> (§11 decision 10). **S6, the bench, is next.** Study is on the
> launcher and runs the demo end to end; the owner has it on the device. The format is
> `COURSE_FORMAT.md` (awaiting the owner's review), the demo is
> `courses/demo-kanji/`, `tools/mkcourse.py` builds courses and
> `firmware/main/course.c` reads them. Approved 2026-09-29; the name, launcher
> slot, speaker and S0 proposals were settled the same day (§3, §11). This
> replaces the app platform (`APP_PLATFORM_PLAN.md`) as the current project.
> That plan is on hold, and why is in §1. Revised the same day after
> checking the design against a real course in progress (§4.6), and to add
> a demo kanji course that ships with the firmware (§9).

A first-class flashcard app in the style of Anki and WaniKani. It reads a
large, preformatted **course** from the SD card, such as about 9,000
radicals, kanji and words, or about 3,700 words of a language. It teaches new
items in lessons, schedules reviews, and keeps progress that survives a
battery pull.

## Contents

1. [Why built in, not an SD app](#1-why-built-in-not-an-sd-app)
2. [What it does](#2-what-it-does)
3. [Where things live](#3-where-things-live)
4. [The course file (draft; S0 finalises it)](#4-the-course-file)
5. [Progress and the schedulers](#5-progress-and-the-schedulers)
6. [Text and other scripts](#6-text-and-other-scripts)
7. [Memory and time budgets](#7-memory-and-time-budgets)
8. [Content and licences](#8-content-and-licences)
9. [The demo course](#9-the-demo-course)
10. [Phases](#10-phases)
11. [Decisions](#11-decisions)

---

## 1) Why built in, not an SD app

Decided 2026-09-29, after reviewing the app platform plan:

- **The framework is several times the work of the app.** Phases 0–4 of the
  platform plan (loader, SDK, simulator host, inspector, device guards) would
  have to land before the first review worked.
- **It wouldn't make development much quicker.** The loop is already fast:
  edit, `make -C sim`, look at the screenshots, flash. The framework saves
  only the full-firmware flash, which takes about a minute, and adds an
  inspection and an SD card copy per change.
- **Its limits would make this app worse.** They are 16 KB of data and a
  24 KB block, 1-bit bitmaps only, no kana font, 4 open files, a 2 KB stack
  and 50 ms per event. Built in, the app gets the free heap while its screen
  is open, full LVGL and the existing 38 px kana font.
- **Extending it is about content, not code.** New decks, new levels and
  other languages are new course files. A good course format covers most of
  the "add things without reflashing" benefit at no platform cost.
- **There's room for it.** The image is about 1.58 MB in a 3 MB app
  partition.

**What carries over from the platform plan:**
- the SRS backtest (§5.9 there);
- the pattern of appending changes to a log and folding the log into the
  main file on open (§5.8 there);
- read-only course data kept apart from progress;
- the measured heap numbers.

**When to bring the framework back:** someone other than the owner wants to
write apps, `ui.c` or flash becomes a real bottleneck, or apps need to be
shared with other people's devices. Keeping the SRS logic free of LVGL
(§3) means most of it would move into an SD app unchanged.

## 2) What it does

**v1 (phases S0–S3):**
- **Pick a course** from the ones on the card. The demo kanji course (§9)
  is there from the first run.
- **A dashboard:**
  - lessons available and reviews due now;
  - how many items are at each stage;
  - the current level, with its title and progress;
  - when the next reviews come due.
- **Lessons:**
  - New items come in small batches (5 by default). Each shows the item
    with its meanings, readings, mnemonics, examples and links (what it's
    built from, and its family). Then comes a short quiz on the batch.
  - Items unlock by level and by links, the way WaniKani does it: radicals,
    then the kanji built from them, then the words that use the kanji. The
    Albanian example works the same way: a root, then the words derived
    from it.
- **Reviews:**
  - Due items from **every level, mixed and shuffled**. Show the prompt,
    tap to reveal the answer, then grade it.
  - Grading is **Wrong / Right** (WaniKani-style) or **Again / Hard /
    Good / Easy** (Anki-style), whichever the course sets.
  - A wrong answer comes back later in the same session, and can be undone
    right after grading.
  - An item with a meaning and a reading counts as one review with two
    questions.
- **Crash-safe progress:** every grade is on the card before the next card
  shows.
- **Clock guard:** with no valid time (never synced), the app shows the
  course but doesn't schedule. It says to set the clock or HotSync.

**S4 (replanned 2026-09-30):**
- **The answer at twice the size**, in the Palm font doubled.
- **A Week screen**, as Coach and Guru have: the last seven days as a
  chart, this week against last, the streak, how many were right, where
  the items stand, and her advice.

**S5 (done 2026-09-30):**
- **The reviews due, at a glance:** a STUDY row on the lock screen ("42
  reviews due", or "caught up · next 9:40p") and a count on the launcher's
  Study icon, totalled across every course. Reviews only: lessons aren't
  counted.
- **A forecast** on the Week screen: the next 24 hours, and each of the
  next seven days.

**Not planned:**
- **typed answers** (§11 decision 9, 2026-09-30). Writing on a 2.8"
  touchscreen is slow and easy to get wrong, so every question is shown,
  revealed and self-graded: no romaji-to-kana on the Graffiti strip, no
  meaning matching, no "close enough" for a missing accent;
- syncing progress to a server;
- audio;
- editing cards on the device;
- unfreezing the Kana trainer (it stays at Tier 2; `BACKLOG.md` ▸ Decided).

## 3) Where things live

**On the card:**

```
/sdcard/study/
  <course-id>/
    course.srs        the course: read-only, replaced as a whole to update
    progress.dat      one 16 B record per started item, sorted by item id
    progress.log      grades since the last fold, appended
    history.dat       one 12 B record per day studied: the Week screen (S4)
    summary.bin       the next week's due times: the lock screen and launcher (S5)
  demo-kanji/         the demo course, written on first run (§9)
  last.txt            the course last opened
  .demo               marker: the demo has been installed once
```

**In the source:**

| File | What | Host-tested |
|---|---|---|
| `firmware/main/course.c/.h` | Reads and checks `course.srs`: header, sections, items, levels, text fields, links and bitmaps. No LVGL, no ESP-IDF. | Yes: reader test, damaged-file fuzz with ASan and UBSan |
| `firmware/main/srs.c/.h` | Both schedulers (§5), lesson unlocks, the progress file and log, folding, and remapping after a course update. Takes the time as an argument. | Yes: a simulated year of reviews, a battery pull at every byte of the log |
| `firmware/main/study_demo.c` | The demo course as a C array, generated from `courses/demo-kanji/course.srs` (the same way `guru_pool.c` is generated) | Checked to match the built file |
| Screens | Course picker, dashboard, lesson, review, stats | Sim smoke tour and screenshots |
| `tools/mkcourse.py` | Builds `course.srs` from a course source folder (§4.5); `--check` validates an existing file and prints its sizes | Yes: round-trip in a host gate |
| `courses/demo-kanji/` | The demo course: source, id table, README, and the built `course.srs` (§9) | Yes: `--check` and a byte-for-byte rebuild |
| `tests/data/study/` | Small test-only courses: `cards` (a generic front/back deck) and `features` (every field and link). The fuzz gate makes its damaged copies as it runs. | |

**Screens outside `ui.c`, if it's cheap.** Existing apps put their pure
logic in their own file and their screens in `ui.c` (for example `wordie.c`
and `show_wordie()`). S3 first checks which `ui.c` helpers the screens need
(title bar, button strip, game canvas, menus, alerts). If exposing them
through a small `ui_internal.h` is simple, the screens go in `study_ui.c`.
If not, they go in `ui.c` like the rest, and the logic stays separate
either way.

**Name, tile and speaker** (decided 2026-09-29):
- **Name: Study.**
- **Launcher: position 7 on the main 3×3 grid.** The grid is full today.
  The **Planner** merge of To Do and Memo (built 2026-09-29;
  `BACKLOG.md` ▸ RESUME HERE) frees a slot, and the grid becomes:

  | | | |
  |---|---|---|
  | Date Book | Address | **Planner** |
  | News | HotSync | Games |
  | **Study** | Guru | Coach |

  News moves up from 7 to 4, Games stays at 6, and Study takes 7.
- **Icon:** a textbook on a black circle, Palm style, 24×22 A8 like the
  other launcher icons, drawn with a small generator as the Guru and Zip
  icons are.
- **A speaker, like Coach and Guru:** a portrait in the right-hand margin
  with speech bubbles, for the greeting, the "reviews due" line, level-ups
  and the end-of-session summary.
  - The owner's source art is `docs/img/study_avatar_src.png` (1-bit,
    1008×1054).
  - It was reduced with `tools/gen_faces.py --from-image` at 60 px wide and
    ink coverage 96, which gives 60×63 with no blank rows to trim. The
    result is `docs/img/study_face.png`, a 1:1 black-on-white PNG that reads
    back pixel for pixel.
  - **The owner hand-finished it** (2026-09-29): brows, eyes, smile, an ear
    and the collar, 124 pixels in all. `docs/img/study_face.png` is now the
    finished art: 60×63, pure black and white, reading back pixel for pixel.
    **It is the artwork; never regenerate it from the source.** S3 adds it
    to `FACES` as `STUDY` through `--from-exact`, and emits it into
    `palm_icons.c`. The portrait is const flash, not the LVGL pool.

## 4) The course file

**This section is a draft.** S0 turns it into `docs/COURSE_FORMAT.md`, with
exact byte layouts and an authoring guide, and S1 implements it.

### 4.1 One file, read a piece at a time

A course is **one file**, so copying or updating it is a single step. The
device **never loads it whole**. It jumps to what it needs:

- **Each card is two small reads:** its fixed-size entry in the item table,
  then its text. A review that mixes item 12 from level 3 with item 3,000
  from level 480 costs the same as two neighbours.
- **One file beats a file per level on this board.** Opening one of
  hundreds of files in a folder is slow on the SD card's FAT file system,
  and only 6 files can be open at once. Seeking inside one file is the fast
  path. (Per-level files suit a website, which fetches a level at a time.
  Here the unit is the item, not the level.)
- **Size isn't a concern:** 1–20 MB for a real course (§4.6), and FAT32
  allows up to 4 GB per file. The cost of one file is that any change means
  copying the whole file again, which takes seconds.

### 4.2 Sections

A header and a table of sections, each with its own CRC32:

| Section | Contents |
|---|---|
| `META` | Course id, title, version, author, licence, language, the item kinds (§4.3), the scheduler and all its parameters (§5), lesson batch size, and the unlock rule |
| `LEVL` | One entry per level: number, title, theme (for example a root word, or the radical a level is named after), a short gloss, and "part *p* of *n*" when a large theme spans several levels |
| `ITEM` | Fixed 32 B records, sorted by level then order, so item *n* is a single seek. Each has: a **stable 32-bit id** (§4.4), kind, **level (16-bit; the Albanian example has 543)**, flags, frequency rank, and offsets into `TEXT`, `BMP` and `LINK`. |
| `TEXT` | For each item, a list of tagged fields (§4.3) |
| `BMP` | Pre-rendered bitmaps for text the Palm font can't draw (§6) |
| `LINK` | For each item, typed links to other items (§4.3) |
| `IDX` | Item ids sorted, each with its item number, for looking up progress and links |

**Rules the reader enforces:**
- Every offset and length is checked against its section before use.
- A bad CRC or an out-of-range value rejects the file with a message, never
  a crash.
- The fuzz gate proves this on truncated and damaged copies.

### 4.3 Kinds, fields and links

**Kinds are named by the course**, not fixed by the firmware:
- the Japanese courses use radical, kanji and word;
- the Albanian example uses root and derived word;
- an Anki-style deck uses one kind, "card".

The firmware only needs to know which fields a kind is quizzed on (for
example meaning and reading, or just meaning).

**Text fields, each tagged and repeatable in order:**
- the term (what's being learned) and its reading or readings (on'yomi,
  kun'yomi or a word reading, with the primary marked);
- the meaning, and other accepted meanings;
- **senses**: a part of speech with a gloss, repeated, for a word with
  several meanings;
- examples: a sentence and its translation, optionally tied to a sense;
- mnemonics (meaning and reading);
- etymology and notes.

**Typed links** (item to item, by id):
- **unlocks after:** the lesson isn't offered until the linked item reaches
  the course's "known" stage;
- **built from:** a component, with an optional role such as "stem" or
  "radical". The card shows "built from: 木 + 木", or "built from: meje";
- **family:** the root of a word family. The card can list the family;
- **related:** shown, but no effect on unlocking.

### 4.4 Stable ids

Progress is keyed by item id, not position. When a new version of a course
adds, removes or reorders items, the app remaps progress on open:
- removed items drop out;
- new ones start unlearned;
- the rest keep their progress.

Course sources can use **words or other strings as ids** (the Albanian
example uses the word itself, such as `bëj`). The course source folder keeps
**`ids.tsv`**, which maps each source id to a 32-bit number. `mkcourse.py`
adds new ids at the end and **never reuses or renumbers one**, so rebuilding
never scrambles anyone's progress. The file is part of the course source
and committed with it.

### 4.5 The source format (accepted: JSON Lines)

A course source folder, which is what an author writes and what
`mkcourse.py` reads:

```
course.json     title, language, licence, kinds, scheduler, levels
items.jsonl     one item per line: id, kind, level, fields, links
ids.tsv         source id -> number (maintained by mkcourse.py)
README.md       what the course is, where its content came from
```

**Why JSON Lines:**
- one item per line keeps diffs readable;
- it's what AI tools and scripts produce most reliably;
- it holds the nesting a real course needs (senses, examples, links).

**Also accepted:** a two-column TSV, front and back. An Anki export becomes
a course with no levels.

### 4.6 Checked against a real course in progress

The owner's Albanian course
([androidbot18/albcourse](https://github.com/androidbot18/albcourse)) is
still being built. **It's used only as a check on this design.** None of its
data is copied into this repo or used in tests. Measured 2026-09-29:

| | In that repo | As a course file |
|---|---|---|
| Words | 3,731 | 3,731 |
| Levels | 543 | 543 (so level numbers are 16-bit) |
| Size | 11 MB of JSON | About 1.3 MB: 1.14 MB of text the device would show (every sense with its part of speech, examples, etymology, components) plus about 150 KB of tables |
| Largest item | | 2.7 KB of text |
| Progress | Browser storage | 3,731 × 16 B = 60 KB |

**What it changed in this plan:**
1. a level table (`LEVL`) with titles, themes and parts;
2. ids that are strings, through `ids.tsv`;
3. **a second scheduler**, SM-2 style (§5), since that's what its web app
   uses;
4. typed links (built from, family) instead of prerequisites only;
5. the text rules in §6: Albanian's ë and ç are in the Palm font, but its
   etymologies quote Arabic, Greek and Old Albanian script;
6. "close enough" for letters typed without their accent (§2). Dropped
   with typed answers (§11 decision 9).

## 5) Progress and the schedulers

**The progress record: 16 B per item that has been started.**

| Bytes | Field |
|---|---|
| 4 | Item id |
| 4 | Next due time (UNIX seconds, `uint32`) |
| 2 | Current interval in hours (saturates at about 7 years) |
| 1 | Ease, stored as (ease − 1.30) / 0.05, so 1.30–14.05 (SM-2 only; changed in S2: the web app lets ease grow past 3.85) |
| 1 | Stage (stage scheduler) or step on the ladder (SM-2) |
| 1 | Flags (lesson done, burned, suspended) |
| 1 | Lapses (saturating) |
| 2 | Reviews (saturating) |

A 9,000-item course is 144 KB on the card, and nothing like that in RAM.

**Crash safety (the log pattern):**
- Each grade appends one 20 B entry to `progress.log`: the new record plus
  a CRC.
- On open, the app folds the log into `progress.dat` through `safefile`
  (write `.tmp`, then swap), then empties the log.
- A cut-off append loses only its own entry, which fails its CRC and is
  ignored.
- Coach and Guru already keep a `.sav` and a `.log`, so this follows the
  firmware's existing practice.

**Two schedulers. The course picks one in `META`, with all its constants
there too, so neither is hard-coded.**

**`stages` (WaniKani-style).** The default table:

| Stage | Interval |
|---|---|
| Apprentice 1–4 | 4 h, 8 h, 1 d, 2 d |
| Guru 1–2 | 1 w, 2 w |
| Master | 1 month |
| Enlightened | 4 months |
| Burned | Retired |

- A right answer moves up one stage.
- A wrong one moves down one stage, or two when the item is at Guru or
  above.
- A level's next level unlocks when a set share of its kanji (90 % by
  default) reach Guru.

**`sm2` (Anki-style).** Each item has an ease, an interval and a step on a
ladder of minimum intervals. The course sets:
- the ladder (for example 1, 2, 4, 8 and 16 days);
- the multiplier for Hard, Good and Easy;
- the starting and minimum ease;
- the relearn delay after Again (for example 10 minutes).

Those are exactly the constants the Albanian example's web scheduler uses,
so a course would behave the same on the device as on the web. A smarter
scheduler (FSRS) can be added later as a third option.

**Finding what's due:**
- On open, the app reads `progress.dat` and the course's `IDX` side by
  side. Both are sorted by id, so one sequential pass matches them up. It
  collects up to 500 due item numbers (2 B each), the stage counts and the
  next due time.
- With item numbers in hand, each card is a direct seek, so a mixed review
  never searches the file.
- For 9,000 items this reads about 216 KB, an estimated few hundred
  milliseconds. S6 measures it; if it's slow, the scan is split across
  timer ticks behind a "Loading" line.
- The dashboard's scan also writes `summary.bin` (S5): the count due, and
  the next week's due times to the minute. Study writes it again on close
  if there were grades since. The lock screen and launcher read it without
  opening the course, and the count rises by itself as times come due.

## 6) Text and other scripts

**The Palm fonts draw every character from U+0020 to U+00FF**, in both
regular and bold (checked in `lv_font_palm.c` and `lv_font_palm_bold.c`).
That covers English and the Latin-1 languages: French, German, Spanish,
Italian, Portuguese, Dutch, the Nordic languages, and **Albanian (ë, ç, Ë,
Ç)**. Such text is stored as UTF-8 and drawn directly.

**`mkcourse.py` handles everything else at build time:**
- **Typographic punctuation is folded:** curly quotes to `"` and `'`, en and
  em dashes to `-`, and `…` to `...`. The Albanian example uses these
  throughout.
- **Any other character makes that field a bitmap**, rendered with a font
  on the computer and word-wrapped at build time to the screen width. This
  rule covers kanji, Cyrillic and unaccented Greek. (Measured in S1: the
  pinned Noto Sans JP has no Arabic, Hebrew or accented Greek, so those go
  through the strip rule below; `COURSE_FORMAT.md` §4.)
- **For side fields, a course can choose to strip instead:** drop the runs
  the font can't draw and keep the rest. Wiktionary etymologies usually
  give a transliteration in brackets, so "Ottoman Turkish حال (hal,
  'situation')" still reads well stripped. Fields being learned are always
  rendered, never stripped.
- `--check` reports which fields became bitmaps, and how many bytes they
  cost.

**Japanese:**
- **Kana** can use the firmware's `lv_font_kana` (38 px) directly.
- **Kanji can't come from a font in RAM.** A 2,000-kanji font at 24 px is
  about 144 KB, more than the whole free heap. So kanji are always
  bitmaps, rendered with a Japanese TTF (for example Noto Sans JP, which is
  OFL), at fixed heights:

| Use | Height | Example size |
|---|---|---|
| The prompt: one kanji or radical | 64 px | 512 B |
| The prompt: a word | 40 px, shrunk to fit 232 px wide | About 800 B for 4 characters |
| Readings, sentences and lists | 24 px | About 300 B per reading |

**Size and drawing:**
- A full kanji course is about 10–20 MB, which is nothing on an SD card.
- The app reads one bitmap into a heap buffer and shows it with an LVGL
  image (A1 format), or draws it on the game canvas. A tall bitmap, such as
  a long wrapped etymology, is drawn in strips, so the buffer stays small.
- **Checked in S3:** whether 1 bit per pixel is readable for complex kanji
  at 24 px. If it isn't, try 2 bits per pixel (smoothed, 4 grey levels,
  twice the bytes) and compare screenshots.

## 7) Memory and time budgets

With the launcher showing there's about 114 KB of free heap (QEMU; about
106–108 KB estimated on the device with the SD card mounted).

| While the app is open | Budget |
|---|---|
| Static DRAM (always) | ≤ 256 B: a pointer to the session and a few flags. Static DRAM is 89 % used. |
| Heap for the session | ≤ 24 KB target, all freed on close: the due list (1 KB), the session queue, one item's text (≤ 4 KB, which `mkcourse.py` enforces; the Albanian example's largest item is 2.7 KB), bitmap strips (≤ 4 KB) and the file buffers |
| LVGL pool | Labels and images only, with no bars, sliders or arcs (the pool rule). `poolparity` and the smoke run confirm this. |
| Open files | At most 3: the course, `progress.dat` and `progress.log` (the mount has 6) |
| Showing the next card | ≤ 50 ms after the tap, which is one item entry, its text and a bitmap or two read from the card |
| Opening a course | ≤ 1 s on the device for 9,000 items, including the fold and the due scan |
| Flash for the demo course (§9) | ≤ 200 KB |

The sim's heap cap and `smoke32` catch the heap, pool and file limits. The
two times are `[d]` measurements. The flash figure is measured by
`idf.py size-components` against the baseline.

## 8) Content and licences

- **WaniKani content** (meanings, mnemonics, radical names and the order of
  items) belongs to Tofugu. A course built from it is for **personal use on
  your own card**. It is never committed to this repo or published, and the
  demo course (§9) doesn't borrow from it.
- **Open sources** for a course that can be shared:
  - KANJIDIC2 and JMdict (EDRDG, CC BY-SA 4.0);
  - KanjiVG (CC BY-SA 3.0) for radical breakdowns;
  - Wiktionary extracts such as Kaikki.org (CC BY-SA), as the Albanian
    example uses.
- **In this repo:**
  - the demo course (§9), written for this project;
  - the tiny test-only decks in `tests/data/study/`;
  - the converter.

  Other courses are built on the author's computer and copied to their
  card.
- **The rendering font** (Noto Sans JP, OFL) is used only on the computer by
  `mkcourse.py`. The course file holds pixels, not the font.
- **Each course states its licence** in `META`, and the app shows it in the
  course's About box.

## 9) The demo course

**Two WaniKani-style kanji levels, shipped with the firmware.** It has two
jobs:
1. **A demo:** a new user opens Study and can do real lessons and reviews
   straight away, before building or copying any course.
2. **The example for course authors:** `courses/demo-kanji/` is the
   reference for how a course source is laid out. Its README walks through
   every file, and `docs/COURSE_FORMAT.md` points to it. It uses every
   feature a Japanese course needs:
   - the three kinds;
   - both kinds of link;
   - meaning and reading questions;
   - mnemonics and examples;
   - rendered kanji.

**Content (accepted; written in S0):** 12 kanji and 20 words per level, with
the radicals they need: 8 in level 1 and 6 in level 2, 78 items in all. Kanji in level 2 are built from radicals of both levels,
so unlocking across levels is exercised:

| Level | Kanji |
|---|---|
| 1 | 一 二 三 十 人 口 大 山 日 月 木 本 |
| 2 | 上 下 中 目 田 力 男 休 体 林 森 明 |

For example, 男 is built from 田 and 力, 休 from 人 and 木, and 明 from 日
and 月.
- **Written for this project.** Meanings and readings are facts, checked
  against KANJIDIC2 and JMdict. Radical names, mnemonics and example
  sentences are original. Nothing comes from WaniKani.
- **Licence: CC0**, so anyone can copy it as the starting point
  for their own course.

**How it's built and shipped:**
- `courses/demo-kanji/` holds the source and the built `course.srs`. The
  built file is committed: 81,756 B, 82 % of it pictures (measured in S1).
- `tools/gen_study_demo.py` turns the built file into
  `firmware/main/study_demo.c`, as `guru_pool.c` is generated.
- **Rebuilds are reproducible.** `mkcourse.py` pins its font by SHA-256 and
  downloads it from GitHub if it's missing. It pins Pillow's version, and
  writes no timestamps. CI rebuilds the demo and fails if the result
  differs from the committed file by a single byte, and runs `--check` on
  it.
- **Installed once.** The first time Study opens with a card inserted, it
  writes the course to `/sdcard/study/demo-kanji/` and creates the `.demo`
  marker. Removing the course (from the app's menu) doesn't bring it back
  on the next boot, and a menu item can reinstall it.
- **Everywhere:** the simulator and the web emulator get it the same way,
  and the smoke tour uses it for the Study screenshots.

## 10) Phases

Each phase follows the project rules: every gate before a commit, one group
per commit, numbers in `BUILD_PROGRESS.md`, and `[d]` boxes for the owner
only.

**S0 — Course format and demo content.**
- Write `docs/COURSE_FORMAT.md` with:
  - exact byte layouts (little-endian, each section's offset, length and
    CRC32);
  - the source format (§4.5);
  - an authoring guide that uses the demo course as its example.
- Write the demo course source in `courses/demo-kanji/` (§9), and the
  test-only generic deck in `tests/data/study/`.
- Add an appendix to `COURSE_FORMAT.md` that maps the Albanian example's
  fields onto the format, to show it fits. The data stays in its own repo,
  so this is a paper check.
- **Exit:** the owner has reviewed the format and the demo content. The
  format is the one thing that's hard to change once real courses exist.

**S1 — Reader and builder.**
- `course.c` and `mkcourse.py` (build and `--check`), with host gates:
  - round-trip (source to file to reader, compared field by field);
  - damaged-file fuzz under ASan and UBSan, added to `make ftest`;
  - the reader compiled into `smoke32`, since the device is 32-bit;
  - the demo's `--check` and byte-for-byte rebuild.
- Build and commit the demo's `course.srs`.
- **Exit:** the gates are green in CI.

**S2 — Schedulers and progress.**
- `srs.c`, with host tests:
  - a simulated year of daily reviews under each scheduler, with fixed
    answers giving the expected stages, eases and due times;
  - the log truncated at every byte, which never loses more than the last
    grade;
  - remapping after a course update (items added, removed and reordered);
  - unlocks by level and by links.
- **Exit:** the tests are green in CI.

**S3 — Screens and the demo on the device.**
- Needs the Planner merge first, for the launcher slot (§3).
- The owner's finished portrait (`docs/img/study_face.png`, §3) added to
  `FACES`.
- The launcher tile and icon, the speaker portrait, the course picker, the dashboard, and the
  lesson and review screens with self-grading.
- `study_demo.c` and the first-run install, with its flash cost measured.
- Smoke tour steps and screenshots using the demo, looked at before ticking
  `[s]`.
- Heap and pool measured in the sim, and the 1-bpp versus 2-bpp kanji
  decision (§6).
- **Exit:** a new user can install, open the demo, do a lesson and a review
  round in the sim. The heap is back to its opening value after closing,
  and the budgets in §7 are met.

**S4 — Readability and the week** (replanned 2026-09-30, when typed
answers were dropped: §11 decision 9).
- The answer at twice the size: the Palm bold font pixel-doubled
  (`tools/gen_font_2x.py`), for the primary meaning, a reading in Latin
  letters, and a term that's words. 1× when it doesn't fit.
- **The Week screen**, built as Coach's and Guru's are:
  - a day-by-day history per course (`history.dat`, in `study.c`, with
    host tests), written as each item finishes and taken back by Undo;
  - the last seven days as a chart, this week against last, the streak
    and the best, how many were right, and the stage groups;
  - her advice from the strip, chosen in `study.c` (host-tested).
- **Exit:** smoke shots of both, looked at; the history's tests in CI.

**S5 — Glanceable** (done 2026-09-30, the layouts the owner chose from
mock-ups: §11 decision 10).
- `summary.bin` per course (`srs_scan_sum()`, `st_sum_write()`): the count
  due, when the next is due, how many in the next 24 hours, the next seven
  days by local day, and the 1,024 soonest due times to the minute. It's
  written by the dashboard's scan, and on closing with grades since.
- `st_glance()` totals every course's summary at the time asked.
- **The lock screen:** AHEAD gets a third row, STUDY. The 14 px came from
  the weather box: the air quality moved up beside the reading (and
  shortens, then goes, if the reading is long), and the rain bars top out
  at 20 px.
- **The launcher:** a black count at the Study icon's shoulder, 99+ at
  most, none at 0.
- **The Week screen's lower half is the week ahead:** "Coming up", the
  next 24 hours, and seven outlined bars under the week behind. It
  replaced the stage groups there, which the dashboard shows anyway.

**S6 — On the bench** `[d]`:
- the demo, then a full course on the real card: open time, due scan,
  next-card time, heap while open;
- a battery pull mid-review loses nothing but the current card;
- readability of kanji at each size.

## 11) Decisions

1. **Built in, not an SD app** (2026-09-29). The reasons are in §1; the app
   platform is on hold.
2. **Courses are data on the SD card**, one file each, read a piece at a
   time. `mkcourse.py` builds them on a computer. Progress is keyed by
   stable item id, through `ids.tsv`, and kept apart from the course.
3. **Text the Palm fonts can draw (U+0020–U+00FF) is drawn directly.
   Everything else is a pre-rendered bitmap**, including all kanji. The
   firmware ships no new fonts; the one addition (2026-09-30, from the
   bench) is the Palm bold font pixel-doubled for answers, generated from
   the existing one, the same glyphs at twice the size.
4. **Two schedulers**, stage-based (WaniKani) and SM-2 (Anki), chosen and
   tuned by the course.
5. **A demo kanji course of two levels ships with the firmware** and is the
   reference example for course authors (§9).
6. **The Albanian course is a design check only.** It's still being
   built, and none of its data enters this repo.
7. **Name, tile and speaker** (2026-09-29): "Study", position 7 on the
   main grid (made possible by the Planner merge), a textbook-on-a-black-
   circle icon, and the owner's portrait as its speaker (§3).
8. **Accepted 2026-09-29:** JSON Lines as the source format, CC0 for the
   demo content, and the demo's kanji list (§9). The written format
   (`COURSE_FORMAT.md`) still gets the owner's review at the end of S0.
9. **No typed answers** (2026-09-30, from the bench). Typing isn't
   practical on the 2.8" touchscreen, so every question is shown, revealed
   and self-graded. The old S4 (romaji to kana on the Graffiti strip,
   meaning matching with typo tolerance, "close enough" accents) is
   dropped, and `romaji.c` won't be written. The owner liked WaniKani's
   kana appearing as you type, but it doesn't suit this screen. S4 became
   the answer at twice the size and the Week screen.
10. **Glanceable counts** (2026-09-30, chosen from simulator mock-ups): the
    lock screen's AHEAD zone gets a STUDY row, made room for by moving the
    air quality beside the weather reading ("air quality can be minimized
    on the dashboard as necessary"); the launcher shows a count badge on
    Study's icon; the count is the total across every course, and reviews
    only.
