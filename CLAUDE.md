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
    with the targets `poolparity nosecrets data graf mines wordie sudoku zip
    clock coach guru gurupool dash smoke smoke32`;
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

## Current work: the SRS app, and two firmware items (approved 2026-09-29)

**The app platform is on hold** (`APP_PLATFORM_PLAN.md`, status block and
§11 decision 7). The user's main goal is one first-class SRS app
(WaniKani/Anki-style, with a large course on the SD card), and it's being
built into the firmware. The plan is `docs/SRS_PLAN.md`: §1 says why built
in, §9 has the phases, §10 the decisions. The checklist is the first "RESUME
HERE" in `BACKLOG.md`. These docs were written on branch
`claude/project-review-recommendations-x1ihdn`. If your branch lacks them,
`git fetch origin claude/project-review-recommendations-x1ihdn` and take them
from there, or ask the user whether it has been merged.

**Decisions already made (don't reopen them):**
1. The SRS app is **built in**, not an SD-loaded app.
2. **Courses are data on the card** (`/sdcard/study/<id>/course.srs`), built
   on a computer by `tools/mkcourse.py`. Progress is keyed by stable item id
   and kept apart from the course, so a course update keeps progress.
3. **Japanese text is pre-rendered bitmaps** in the course file. The
   firmware ships no kanji font; readings may use the existing 38 px
   `lv_font_kana`.
4. **WaniKani content never enters the repo.** It belongs to Tofugu, so it's
   for personal use on the user's card. The repo holds only a small
   self-written sample course and the converter.
5. **Proposed, open until the user reviews S0:** the working name "Study",
   its own launcher tile, and the stage scheduler with per-course intervals.

**Measured already (QEMU, launcher showing):** 114,352 B free heap (largest
block 90,112 B); about 106–108 KB estimated on the device with the SD card
mounted. LVGL pool free: 11,024 B.

### The order of work

1. **Passwords out of resident RAM** (was platform §8.2 item 1). Worth doing
   on its own; do it first.
   - `Config` (`bridge/config.h`, `g_cfg` in `firmware/main/appcfg.c`) stops
     holding passwords and keeps only "has password" flags.
   - `hotsync.c` reads each password with `secret_get()` into a short-lived
     buffer (the DAV `d.pass` and `dcard.pass` copies, and `wifi_config_t wc`)
     and wipes it with `mbedtls_platform_zeroize()` after use.
   - Settings password fields write straight to `secretstore` and show
     `********`.
   - Call `esp_wifi_set_storage(WIFI_STORAGE_RAM)`.
   - Add a sim gate (for example `make -C sim secretscan`) that searches heap
     and static memory for a test password after a stubbed sync and after
     closing Settings. Add it to `ci.yml` and to the gate list above.
   - Leave the `[d]` bench item for the user: Wi-Fi join and an iCloud
     HotSync still work.
2. **Firmware version in About.**
   - Set it from `git describe --tags --always --dirty` (`PROJECT_VER`);
     check what ESP-IDF already does by default.
   - Show it in Settings ▸ About. The About text is in `ui.c`, near the
     Settings row list with "About". The sim needs an
     `esp_app_get_description` shim in `sim/include`.
   - Make `tools/package_firmware.py` and `ci.yml` use the same string.
3. **SRS S0: the course format.** Turn `SRS_PLAN.md` §4 into
   `docs/COURSE_FORMAT.md` with exact byte layouts (little-endian, with the
   offsets and CRC32 of each section), plus the TSV/JSON source format
   `mkcourse.py` reads. Write the sample course source in
   `tests/data/study/` (about 30 self-written items: a few radicals, kanji
   and words, and a small generic front/back deck). **Then stop for the
   user's review of the format** before S1; it's the one thing that's hard
   to change once real courses exist.
4. **S1 onward** as `SRS_PLAN.md` §9 describes, keeping to its §7 budgets:
   - pure-C `course.c` and `srs.c` in `firmware/main/`, with no LVGL and the
     time passed in, tested the way `wordie.c` and `coach.c` are;
   - new host gates added to `ci.yml` and to the gate list above;
   - the reader fuzzed under ASan and UBSan;
   - screens checked by smoke screenshots before any `[s]`.

When a phase is done, tick its `[s]` items in `BACKLOG.md` and move the
finished text to `BUILD_PROGRESS.md`.
