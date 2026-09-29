# CLAUDE.md — working notes for AI sessions

CYD Palm: a PalmOS-style PDA on the ESP32-2432S028R "Cheap Yellow Display".
The board has **no PSRAM**, so RAM is the constraint behind most decisions.
This file is loaded at session start and after context compaction. Read it
before touching anything.

## Where things are

- `firmware/`: ESP-IDF v5.5 app. `firmware/main/ui.c` (about 10.6k lines)
  is the whole UI.
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
  - `make test` and `make ftest` (host);
  - `sudo mkdir -p /sdcard && sudo chmod 777 /sdcard`, then `make -C sim`
    with the targets `poolparity nosecrets secretscan data graf mines wordie
    sudoku zip clock coach guru gurupool dash smoke smoke32`;
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

## Current work: the SRS app, S0 next (approved 2026-09-29)

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

**Measured already (QEMU, launcher showing):** 114,352 B free heap (largest
block 90,112 B); about 106–108 KB estimated on the device with the SD card
mounted. LVGL pool free: 11,024 B.

### The order of work

**Done 2026-09-29** (details in `BUILD_PROGRESS.md`): passwords out of
resident RAM (with the `secretscan` gate), the firmware version in Settings ▸
About, and the Planner (To Do and Memo in one launcher tile, position 7 held
open for Study). Their bench checks are `[d]` items in `BACKLOG.md`.

1. **SRS S0: the course format and demo content** (`SRS_PLAN.md` §4, §9,
   §10). **This is next.**
   - Turn §4 into `docs/COURSE_FORMAT.md`: exact byte layouts
     (little-endian, each section's offset, length and CRC32), the source
     format, and an authoring guide that uses the demo as its example.
   - Write the demo course source in `courses/demo-kanji/`: two levels,
     about 10 radicals, 12 kanji and 20 words each, with a README walking
     through every file.
   - Write the test-only generic front/back deck in `tests/data/study/`.
   - Add an appendix mapping the Albanian course's fields onto the format
     (a paper check; read its repo, copy nothing).
   - **Then stop for the user's review** of the format and the demo content
     before S1. The format is the one thing that's hard to change once real
     courses exist.
2. **S1 onward** as `SRS_PLAN.md` §10 describes, keeping to its §7 budgets:
   - pure-C `course.c` and `srs.c` in `firmware/main/`, with no LVGL and the
     time passed in, tested the way `wordie.c` and `coach.c` are;
   - new host gates added to `ci.yml` and to the gate list above;
   - the reader fuzzed under ASan and UBSan;
   - screens checked by smoke screenshots before any `[s]`;
   - Study's tile goes in the empty launcher slot (`APPS[6]` is `NULL`).

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
