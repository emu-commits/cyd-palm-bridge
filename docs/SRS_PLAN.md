# SRS plan — a built-in spaced-repetition app (working name "Study")

> **Status: approved 2026-09-29; S0 is next.** Nothing is built yet. This
> replaces the app platform (`APP_PLATFORM_PLAN.md`) as the current project.
> That plan is on hold, and why is in §1.

A first-class flashcard app in the style of Anki and WaniKani. It reads a
large, preformatted **course** from the SD card, such as about 9,000
radicals, kanji and words. It teaches new items in lessons, schedules
reviews, and keeps progress that survives a battery pull.

## Contents

1. [Why built in, not an SD app](#1-why-built-in-not-an-sd-app)
2. [What it does](#2-what-it-does)
3. [Where things live](#3-where-things-live)
4. [The course file (draft; S0 finalises it)](#4-the-course-file)
5. [Progress and the scheduler](#5-progress-and-the-scheduler)
6. [Japanese text](#6-japanese-text)
7. [Memory and time budgets](#7-memory-and-time-budgets)
8. [Content and licences](#8-content-and-licences)
9. [Phases](#9-phases)
10. [Decisions](#10-decisions)

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
- **Pick a course** from the ones on the card.
- **A dashboard:**
  - lessons available and reviews due now;
  - how many items are at each stage;
  - the current level and its progress;
  - when the next reviews come due.
- **Lessons:** new items in small batches (5 by default). Each shows the item
  with its meaning, reading and mnemonic, then a short quiz on the batch.
  Items unlock by level and prerequisites, the way WaniKani does it:
  radicals, then the kanji that use them, then the words that use the kanji.
- **Reviews:**
  - Show the prompt, tap to reveal the answer, then grade it: **Wrong** or
    **Right** (WaniKani-style), or **Again / Hard / Good / Easy**
    (Anki-style), whichever the course sets.
  - Reviews are shuffled, and a wrong answer comes back later in the same
    session.
  - An item with a meaning and a reading counts as one review with two
    questions.
  - Wrong answers can be undone right after grading.
- **Crash-safe progress:** every grade is on the card before the next card
  shows.
- **Clock guard:** with no valid time (never synced), the app shows the
  course but doesn't schedule. It says to set the clock or HotSync.

**Later (S4–S5):**
- **Typed answers:** readings drawn in romaji on the Graffiti strip and
  converted to kana, and meanings matched against synonyms with typo
  tolerance.
- **Due counts on the lock screen and launcher**, for example "42 reviews".
- **A forecast and stats screen.**

**Not planned:**
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
  last.txt            the course last opened
```

**In the source:**

| File | What | Host-tested |
|---|---|---|
| `firmware/main/course.c/.h` | Reads and checks `course.srs`: header, sections, items, text fields, bitmaps. No LVGL, no ESP-IDF. | Yes: reader test, damaged-file fuzz with ASan and UBSan |
| `firmware/main/srs.c/.h` | Scheduler, lesson unlocks, progress file and log, folding, remapping after a course update. Takes the time as an argument. | Yes: a simulated year of reviews, a battery pull at every byte of the log |
| `firmware/main/romaji.c/.h` (S4) | Romaji to kana, and meaning matching | Yes |
| Screens | Course picker, dashboard, lesson, review, stats | Sim smoke tour and screenshots |
| `tools/mkcourse.py` | Builds `course.srs` from a documented TSV/JSON source and a Japanese TTF; `--check` validates an existing file | Yes: round-trip in a host gate |
| `tests/data/study/` | A small sample course source and its built file | |

**Screens outside `ui.c`, if it's cheap.** Existing apps put their pure
logic in their own file and their screens in `ui.c` (for example `wordie.c`
and `show_wordie()`). S3 first checks which `ui.c` helpers the screens need
(title bar, button strip, game canvas, menus, alerts). If exposing them
through a small `ui_internal.h` is simple, the screens go in `study_ui.c`.
If not, they go in `ui.c` like the rest, and the logic stays separate
either way.

**Launch point: its own launcher tile**, like Coach and Guru, with a new
24×22 icon. It's a daily habit app, not a game.

## 4) The course file

**This section is a draft.** S0 turns it into `docs/COURSE_FORMAT.md`, with
exact byte layouts, and S1 implements it.

**One file per course**, so copying and updating it is a single step. It
holds a header and a table of sections, each with its own CRC32:

| Section | Contents |
|---|---|
| `META` | Course id, title, version, author, licence, language, grading style (pass/fail or 4-button), stage interval table, lesson batch size, how many items of each level must reach the "guru" stage to unlock the next level |
| `ITEM` | Fixed 32 B records, sorted by level then order, so item *n* is a single seek. Each has: a **stable 32-bit id**, kind (radical, kanji, word or a generic card), level, flags, and offsets into `TEXT`, `BMP` and `DEPS`. |
| `TEXT` | For each item, a small list of tagged UTF-8 fields: primary meaning, other meanings, readings (on'yomi, kun'yomi or word reading, with the primary marked), meaning and reading mnemonics, part of speech, a context sentence and its translation. Each Japanese field points at its bitmaps (§6). |
| `BMP` | Pre-rendered bitmaps of every Japanese string, in the sizes listed in §6 |
| `DEPS` | For each item, the ids it needs before it unlocks (for example, a kanji's radicals) |
| `IDX` | A sorted map from item id to item number, for looking up progress and prerequisites |

**Rules the reader enforces:**
- Every offset and length is checked against its section before use.
- A bad CRC or an out-of-range value rejects the file with a message, never
  a crash.
- The fuzz gate proves this on truncated and damaged copies.

**Why a stable id:** progress is keyed by item id, not by position. When a
new version of a course adds, removes or reorders items, the app remaps
progress on open. Removed items drop out, new ones start unlearned, and the
rest keep their stage.

**Generic decks too:** kind "card" with just a front and a back, no levels,
no prerequisites and the Anki-style grading. That covers an Anki deck
exported to TSV.

## 5) Progress and the scheduler

**The progress record: 16 B per item that has been started.**

| Bytes | Field |
|---|---|
| 4 | Item id |
| 4 | Next due time (UNIX seconds, `uint32`) |
| 1 | Stage |
| 1 | Flags (lesson done, burned, suspended) |
| 1 | Lapses |
| 1 | Current streak |
| 2 | Times correct |
| 2 | Times wrong |

A full 9,000-item course is 144 KB on the card, and nothing like that in
RAM.

**Crash safety (the log pattern):**
- Each grade appends one 20 B entry to `progress.log`: the new record plus
  a CRC.
- On open, the app folds the log into `progress.dat` through `safefile`
  (write `.tmp`, then swap), then empties the log.
- A cut-off append loses only its own entry, which fails its CRC and is
  ignored.
- Coach and Guru already keep a `.sav` and a `.log`, so this follows the
  firmware's existing practice.

**Scheduler: stage-based, with the course's interval table.** The WaniKani
default:

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
- Anki-style courses use the same machinery with different intervals.
  "Hard" repeats the current interval, "Easy" skips a stage, and "Again"
  goes back to the first stage.
- A smarter scheduler (FSRS) can come later as a course option. It isn't
  in v1.

**Finding what's due:**
- On open, the app streams `progress.dat` once. It collects up to 500 due
  items (2 B each), the stage counts and the next due time.
- 144 KB at SD speed is an estimated few hundred milliseconds. S6 measures
  it; if it's slow, the scan is split across timer ticks behind a
  "Loading" line.
- It also writes a tiny `summary.bin` (the due count and next due times) on
  close. The lock screen and launcher read that in S5 without scanning.

## 6) Japanese text

**Kana:** the firmware already has `lv_font_kana` (38 px, 1 bpp, hiragana
and katakana), which can draw readings directly at that size.

**Kanji can't come from a font in RAM.** A 2,000-kanji font at 24 px is
about 144 KB, more than the whole free heap. So **`mkcourse.py` pre-renders
every Japanese string to a bitmap** with a Japanese TTF (for example Noto
Sans JP, which is OFL), at fixed heights:

| Use | Height | Example size |
|---|---|---|
| The prompt: one kanji or radical | 64 px | 512 B |
| The prompt: a word | 40 px, shrunk to fit 232 px wide | About 800 B for 4 characters |
| Readings, sentences and lists | 24 px | About 300 B per reading |

- **Size:** about 10–20 MB for a full course, which is nothing on an SD
  card.
- **Drawing:** the app reads one bitmap into a heap buffer and shows it
  with an LVGL image (A1 format), or draws it on the game canvas. Either
  way, it takes no LVGL pool beyond the widget.
- **English text** uses the Palm font, with accents folded to ASCII, as
  the rest of the firmware does.
- **Checked in S3:** whether 1 bit per pixel is readable for complex kanji
  at 24 px. If it isn't, try 2 bits per pixel (smoothed, 4 grey levels,
  twice the bytes) and compare screenshots.

## 7) Memory and time budgets

With the launcher showing there's about 114 KB of free heap (QEMU; about
106–108 KB estimated on the device with the SD card mounted).

| While the app is open | Budget |
|---|---|
| Static DRAM (always) | ≤ 256 B: a pointer to the session and a few flags. Static DRAM is 89 % used. |
| Heap for the session | ≤ 24 KB target: the due list (1 KB), the session queue, one item's text fields (≤ 4 KB), bitmap buffers (≤ 4 KB) and the file buffers. All of it is freed on close. |
| LVGL pool | Labels and images only, with no bars, sliders or arcs (the pool rule). `poolparity` and the smoke run confirm this. |
| Open files | At most 3: the course, `progress.dat` and `progress.log` (the mount has 6) |
| Showing the next card | ≤ 50 ms after the tap, which is one record and one or two bitmaps read from the card |
| Opening a course | ≤ 1 s on the device for 9,000 items, including the fold and the due scan |

The sim's heap cap and `smoke32` catch the first four. The last two are
`[d]` measurements.

## 8) Content and licences

- **WaniKani content** (meanings, mnemonics, the order of items) belongs to
  Tofugu. A course built from it is for **personal use on your own card**.
  It is never committed to this repo or published.
- **Open sources** for a course that can be shared:
  - KANJIDIC2 and JMdict (EDRDG, CC BY-SA 4.0);
  - KanjiVG (CC BY-SA 3.0) for radical breakdowns.
- **In the repo:** only a small, self-written sample course (about 30
  items) for the tests and the smoke tour, plus the converter. The converter
  runs on the owner's computer and writes to their card.
- **The rendering font** (Noto Sans JP, OFL) is used only on the computer by
  `mkcourse.py`. The course file holds pixels, not the font.

## 9) Phases

Each phase follows the project rules: every gate before a commit, one group
per commit, numbers in `BUILD_PROGRESS.md`, and `[d]` boxes for the owner
only.

**S0 — Course format.**
- Write `docs/COURSE_FORMAT.md` with exact byte layouts, and the TSV/JSON
  source format it's built from.
- Write the sample course source (about 30 items: a few radicals, kanji and
  words, and one generic card deck).
- **Exit:** the owner has reviewed the format. It's the one thing that's
  hard to change once real courses exist.

**S1 — Reader and builder.**
- `course.c`, `mkcourse.py` (build and `--check`), and host gates:
  - round-trip (source to file to reader, compared field by field);
  - damaged-file fuzz under ASan and UBSan, added to `make ftest`;
  - the reader compiled into `smoke32`, since the device is 32-bit.
- **Exit:** the gates are green in CI. The sample course and a converted
  real dataset (on the owner's machine) both pass `--check`.

**S2 — Scheduler and progress.**
- `srs.c`, with host tests:
  - a simulated year of daily reviews, with fixed answers giving the
    expected stages and due times;
  - the log truncated at every byte, which never loses more than the last
    grade;
  - remapping after a course update (items added, removed and reordered);
  - lesson unlocks by level and prerequisites.
- **Exit:** the tests are green in CI.

**S3 — Screens.**
- The launcher tile and icon, the course picker, the dashboard, and the
  lesson and review screens with self-grading.
- Smoke tour steps and screenshots, looked at before ticking `[s]`.
- Heap and pool measured in the sim.
- The 1-bpp versus 2-bpp kanji decision (§6).
- **Exit:** a lesson and a review round work end to end in the sim. The
  heap is back to its opening value after closing, and the budgets in §7
  are met.

**S4 — Typed answers.**
- Romaji to kana with Graffiti in the strip, meaning matching with typo
  tolerance, and the answer checked with a "close enough" warning, as
  WaniKani does it.

**S5 — Glanceable.**
- `summary.bin`, due counts on the lock screen and the launcher tile, and
  a forecast and stats screen.

**S6 — On the bench** `[d]`:
- a full course on the real card: open time, due scan, next-card time,
  heap while open;
- a battery pull mid-review loses nothing but the current card;
- readability of kanji at each size.

## 10) Decisions

1. **Built in, not an SD app** (2026-09-29). The reasons are in §1; the app
   platform is on hold.
2. **Courses are data on the SD card**, built on a computer by
   `mkcourse.py`. Progress is keyed by stable item id and kept apart from
   the course.
3. **Kanji and other Japanese text are pre-rendered bitmaps** in the course
   file. The firmware ships no kanji font.
4. **Proposed, open to change until S0 is reviewed:**
   - the working name "Study";
   - its own launcher tile;
   - the stage scheduler with per-course intervals.
