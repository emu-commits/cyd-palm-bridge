# CLAUDE.md — working notes for AI sessions

CYD Palm: a PalmOS-style PDA on the ESP32-2432S028R "Cheap Yellow Display".
The board has **no PSRAM**, so RAM is the constraint behind most decisions.
This file is loaded at session start and after context compaction. Read it
before touching anything.

## Where things are

- `firmware/`: ESP-IDF v5.5 app. `firmware/main/ui.c` (about 12k lines)
  is the whole UI. The device's DAV transport is
  `firmware/components/bridge/dav_esp.c`.
- `docs/ARCHITECTURE.md`: how the pieces fit, the sync engine, and how to
  build and test each part.
- `sim/`: the same UI built for desktop and wasm, with a device-sized LVGL
  pool and heap cap.
- `bridge/`: the Palm↔CalDAV/CardDAV codec and sync (MIT).
- `tests/`: host gates.
- `docs/BACKLOG.md`: open work. **Its "RESUME HERE" sections say what to do
  next.**
- `docs/BUILD_PROGRESS.md`: changelog, measured RAM/flash numbers, lessons.
- `docs/SRS_PLAN.md`: the built-in spaced-repetition app. **This is the
  current project; see "Current work" below.**
- `docs/APP_PLATFORM_PLAN.md`: native C apps loaded from the SD card.
  **On hold since 2026-09-29**; don't build any of it without the user's yes.

## Rules that always apply

- **Run every gate before a commit, not just the smoke:**
  - `make test` and `make ftest` (host; the programs build into `build/`);
  - `./tests/run_gates.sh` (the sync engine against a local Radicale; needs
    `pip install radicale`) whenever `bridge/` changes;
  - `sudo mkdir -p /sdcard && sudo chmod 777 /sdcard`, then `make -C sim`
    with the targets `poolparity nosecrets secretscan data graf mines wordie
    sudoku zip clock coach guru gurupool dash course srs smoke smoke32`;
  - `python3 tests/mkcourse_test.py` (the course builder; needs the Pillow
    in `tools/requirements-course.txt`, and downloads the pinned font);
  - the firmware build in the `espressif/idf:release-v5.5` container
    (`idf.py set-target esp32 && idf.py build` in `firmware/`);
  - `make -C sim wasm` if emsdk is available.

  `.github/workflows/ci.yml` is the authority for what CI runs.
- **Never tick a `[d]` (device) box in BACKLOG.md.** The user runs bench
  checks on the real board. `[s]` means the simulator ran and the screenshot
  was looked at.
- **One group of work per commit.** Put measured numbers in
  `BUILD_PROGRESS.md` with how they were measured.
- **Don't let versions float.** LVGL is pinned at 9.5.0 in
  `firmware/main/idf_component.yml`, and the sim pins v9.2.2 separately.
  The LVGL pool is 32 KB; `make -C sim poolparity` enforces it.
- **Watch static DRAM.** It is about 89 % used, with about 19.9 KB free. No
  new large static buffers; allocate when a screen opens and free when it
  closes.

## Cloud-session environment (learned 2026-09)

- Docker works. Use `espressif/idf:release-v5.5`, the same image CI uses. It
  includes Espressif QEMU for ESP32.
- **The Espressif component registry is blocked; GitHub is reachable.** For a
  local build, add LVGL as a local component from
  `github.com/lvgl/lvgl` at tag `v9.5.0`. Do this in a scratch copy only;
  don't commit the vendoring. (CI's own build reaches the registry.)
- A faithful baseline build is **1,582,695 B** (within 121 B of CI). Check
  this first, so later measurements are known to be comparable.
- **QEMU doesn't emulate the touch panel, the battery ADC or the SD card.**
  - The battery ADC read hangs forever; stub it for emulator builds only.
  - Skip touch calibration for emulator runs.
  - Estimated heap cost of a mounted SD card on real hardware: 6–8 KB.
- **Do spikes in a scratch copy of the repo** (in the session scratchpad) so
  the working branch isn't touched. The container is wiped between sessions:
  anything not committed and pushed is lost, including earlier spike code.

## Current work: the SRS app, S0–S5 done in the sim (no typing; S6 is the bench)

**The app platform is on hold** (`APP_PLATFORM_PLAN.md`, status block and
§11 decision 7). The user's main goal is one first-class SRS app
(WaniKani/Anki-style, with a large course on the SD card), and it's being
built into the firmware. The plan is `docs/SRS_PLAN.md`: §1 says why built
in, §10 has the phases, §11 the decisions. The checklist is the first "RESUME
HERE" in `BACKLOG.md`. These docs were written on branch
`claude/project-review-recommendations-x1ihdn`. If your branch lacks them,
`git fetch origin claude/project-review-recommendations-x1ihdn` and take them
from there, or ask the user whether it has been merged.

**Decisions already made (don't reopen them):** see `SRS_PLAN.md` §11.
1. The SRS app is **built in**, not an SD-loaded app.
2. **Courses are data on the card** (`/sdcard/study/<id>/course.srs`), one
   file each, read a piece at a time (never loaded whole). They're built on
   a computer by `tools/mkcourse.py`. Progress is keyed by stable item id
   (string ids map to numbers through the course's `ids.tsv`) and kept apart
   from the course, so a course update keeps progress.
3. **Text in U+0020–U+00FF is drawn with the Palm fonts** (they include ë,
   ç and the rest of Latin-1). **Anything else is a pre-rendered bitmap** in
   the course file, including all kanji. Readings may use the existing 38 px
   `lv_font_kana`.
4. **Two schedulers**, stage-based (WaniKani) and SM-2 (Anki), chosen and
   tuned by each course in its `META`.
5. **A demo course of two WaniKani-style kanji levels ships with the
   firmware** (`courses/demo-kanji/`, installed to the card on first run).
   It's also the reference example for course authors. Its content is
   written for this project: facts checked against KANJIDIC2/JMdict,
   original radical names, mnemonics and sentences.
6. **WaniKani content never enters the repo.** It belongs to Tofugu.
7. **The user's Albanian course** (github.com/androidbot18/albcourse) is
   still in progress and is **only a design check**: never copy its data
   into this repo or use it in tests. `SRS_PLAN.md` §4.6 has what it
   showed.
8. **Name "Study"**, at **position 7** on the main launcher grid, with a
   textbook-on-a-black-circle icon and the user's portrait as its speaker,
   like Coach and Guru (`SRS_PLAN.md` §3). **`docs/img/study_face.png`
   (60×63, 1:1) is the user's hand-finished art**: use it as it is, through
   `gen_faces.py --from-exact`, and never regenerate it from the source.
9. **The Planner is approved:** To Do and Memo become one launcher app
   (design in `BACKLOG.md`'s RESUME HERE). Storage and sync don't change.
   The grid becomes Date Book, Address, Planner / News, HotSync, Games /
   Study, Guru, Coach.
10. **Accepted:** JSON Lines as the course source format, CC0 for the demo
    content, and the demo's kanji list. The written `COURSE_FORMAT.md` still
    gets the user's review at the end of S0.
11. **No typed answers on the device** (2026-09-30, from the bench; it's
    `SRS_PLAN.md` §11 decision 9). The 2.8" touchscreen is too small to
    type on, so every question is shown, revealed and self-graded. The old
    S4 (romaji to kana, meaning matching, "close enough" accents) is
    dropped for good: don't build it or `romaji.c`.
12. **Glanceable counts** (2026-09-30, `SRS_PLAN.md` §11 decision 10): a
    STUDY row in the lock screen's AHEAD zone (the air quality may shrink
    to make room), a count badge on the launcher's Study icon, the total
    across every course, reviews only.

**Measured already (QEMU, launcher showing):** 114,352 B free heap (largest
block 90,112 B); about 106–108 KB estimated on the device with the SD card
mounted. LVGL pool free: 11,024 B.

### The order of work

**Done 2026-09-29** (details in `BUILD_PROGRESS.md`): passwords out of
resident RAM (with the `secretscan` gate), the firmware version in Settings ▸
About, and the Planner (To Do and Memo in one launcher tile, position 7 held
open for Study). Their bench checks are `[d]` items in `BACKLOG.md`.

**Done 2026-09-30:** the password editor's peek and Show/Hide; then **SRS S0
and S1**, which the owner asked for together:
- `docs/COURSE_FORMAT.md` is the format (byte layouts, source format,
  authoring guide, and Appendix A, the Albanian paper check).
- `courses/demo-kanji/` is the demo (78 items; facts checked against
  KANJIDIC2 and JMdict, the rest original). `tests/data/study/` holds two
  test decks: `cards` (TSV, sm2) and `features` (every field and link).
- `tools/mkcourse.py` builds and checks courses, reproducibly (pinned font
  by SHA-256, Pillow by version, layout engine BASIC).
  `firmware/main/course.c` reads them.
- Gates: `make -C sim course` (C checks plus a three-way dump round trip),
  `make ftest` (`course_fuzz`: truncations, every bit flip caught, and
  CRC-fixed hostile files under ASan and UBSan), `smoke32` (the checks
  32-bit), and `tests/mkcourse_test.py` (byte-for-byte rebuilds, the
  `ids.tsv` rules, refusals).
- The owner approved the format and the demo on 2026-09-30.
- Then, the same day, the owner asked for a faster demo (a 2-hour first
  stage, `known: 2`), a check that Albanian's letters draw (they do: see
  `BUILD_PROGRESS.md`), and **S2**:
  - `firmware/main/srs.c` holds both schedulers and the progress files;
  - the `srs` gate runs a year per scheduler, and sm2 matches the Albanian
    web app review for review, bar its float drift;
  - it also cuts the log at every byte and checks remapping and unlocks.

- **S3, the Study screens**, followed once the owner approved the format
  ("format and demo look good, start S3"):
  - the tile at launcher position 7, and the owner's portrait through
    `gen_faces.py` (it round-trips `docs/img/study_face.png` exactly);
  - `study.c` holds a round's question queue and the card functions (the
    demo installed once, courses listed, the last one remembered);
  - `study_demo.c` is generated from the demo (`gen_study_demo.py --check`
    runs in `make -C sim course`);
  - the screens are one section of `ui.c` ("Study: the spaced-repetition
    app"), in a single heap block freed when the screen leaves Study;
  - the smoke tour's `B`/`E` commands prove the heap is given back, and `A`
    moves Study's clock on (UI_DEVTOOLS).

- **From the bench (2026-09-30), after S3 was flashed:** Study looks good,
  the portrait reads well on the small screen, and Wi-Fi join and sync work
  after the password change. Asked for: the answer text at 2x, no typing
  (decision 11), and a Week screen like Coach's and Guru's.
  - The 2x answer is done: `lv_font_palm_bold_2x.c`, generated by
    `tools/gen_font_2x.py` (checked in `make -C sim course`), used through
    `st_big()` in `ui.c`.
  - S4 was replanned to "readability and the week" (`SRS_PLAN.md` §10).
  - **The Week screen is done:** `history.dat` per course, written by
    `st_hist_add()` as each item finishes (Undo takes it back), read by
    `st_hist_week()`, with `st_advise()` choosing her line; all in
    `study.c`, tested in `make -C sim srs`. The page is Coach's and Guru's
    (`wk_page`, `wk_chart`, `wk_row`), reached from the dashboard's Week
    button and the menu's This week (`study_week` shot).

- **S5, glanceable due counts, is done** (the owner chose the layouts from
  simulator mock-ups: `SRS_PLAN.md` §11 decision 10). `srs_scan_sum()`
  gathers the week ahead; `study.c` writes it as `summary.bin` and
  `st_glance()` totals every course's. The lock screen's AHEAD zone has a
  STUDY row (the weather box gave up 14 px: the air quality is beside the
  reading now), the launcher shows a badge on Study, and the Week screen's
  lower half is the forecast.

- **From the bench, after S5:** the lesson cards' readings were in the
  38 px kana font and ran off the screen. They're now a line per type in
  `lv_font_kana_20` (generated by `tools/gen_kana_font.py` from the pinned
  picture font; `tests/mkcourse_test.py` checks it), and a single kanji
  has its meaning beside it (`st_card()`).
- **Then, from the bench: full screen.** Lessons, questions and the Card
  hide the Graffiti strip (`ui_full_screen()`; `st_screen_full()`), and Home
  and Menu move to the title bar's right end, made only while it's on.
  `content_clear()` turns it off for everyone else; the smoke tour taps
  Home at (200,12), Menu at (226,12) and Study's buttons at y 295 there.

- **Then (2026-10-01): Guru full screen too.** Her list and a habit call
  `ui_full_screen(1)`; her week keeps the strip for her verdict. Then Home
  in the title bar was made as wide as fits (`tb_home_x()`: from the
  clock's widest to Menu), outlined.

**Where things stand (2026-10-01):** all of the above is merged to `main`
(PR #66, `79272fd`). The owner's bench checks are the open `[d]` items in
`BACKLOG.md`'s first RESUME HERE; still untested there: a bigger iCloud
sync and loading a second course into Study. Since the merge: the README
and `docs/ARCHITECTURE.md`, comments without internal ticket codes, and the
host build in `build/`.

**Now: the sync engine for a real account** (the owner, 2026-10-01: "start
with the external sort, the window and pruning rule, batched downloads.
Focus on a very tight time window"). The plan is `BACKLOG.md` §Engine,
items 1 and 5. The rule that makes a window safe: **an object leaving the
window is pruned on the device and never deleted on the server.**

1. **S6, the bench**, is the owner's (`[d]` items in `BACKLOG.md`). Any
   further work follows `SRS_PLAN.md` §10, keeping to its §7 budgets:
   - the screens use `course.c`, `srs.c` and `study.c` as they are; logic
     belongs there, not in `ui.c`;
   - new host gates added to `ci.yml` and to the gate list above;
   - screens checked by smoke screenshots before any `[s]`;
   - Keep `mkcourse.py`'s dump and `course_test.c`'s dump in step if the
     format grows: the round trip compares them byte for byte.

**Gate-running notes from 2026-09-29:**
- Run `make -C sim clean` before the sim gates if `smoke32` ran last: it
  leaves 32-bit objects in `sim/build`, and the next 64-bit link fails.
- Clear the simulator's card (`find /sdcard -mindepth 1 -delete`) before a
  smoke run whose screenshots you're going to judge. The walk expects a fresh
  card, and a previous run leaves demo data removed and Wi-Fi saved.
- `smoke32` needs `gcc-multilib` (`sudo apt-get update` first in a fresh
  container).

When a phase is done, tick its `[s]` items in `BACKLOG.md` and move the
finished text to `BUILD_PROGRESS.md`.
