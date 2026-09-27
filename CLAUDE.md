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
- `docs/APP_PLATFORM_PLAN.md`: the plan for native C apps loaded from the SD
  card. **This is the current project; see "Current work" below.**

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
  `github.com/lvgl/lvgl` at tag `v9.5.0`, and `elf_loader` from
  `github.com/espressif/esp-iot-solution` (`components/elf_loader`). Do this
  in a scratch copy only; don't commit the vendoring.
- A faithful baseline build is **1,582,695 B** (within 121 B of CI). Check
  this first, so later measurements are known to be comparable.
- **QEMU doesn't emulate the touch panel, the battery ADC or the SD card.**
  - The battery ADC read hangs forever; stub it for emulator builds only.
  - Skip touch calibration for emulator runs.
  - Put test files in a flash partition instead of the SD card.
  - Estimated heap cost of a mounted SD card on real hardware: 6–8 KB.
- **Do spikes in a scratch copy of the repo** (in the session scratchpad) so
  the working branch isn't touched. The container is wiped between sessions:
  anything not committed and pushed is lost, including earlier spike code.

## Current work: app platform, Phases 0 and 1 (approved 2026-09-27)

The plan is `docs/APP_PLATFORM_PLAN.md`. §10 has the phases, §11 the
decisions. It was written on branch `claude/project-review-recommendations-x1ihdn`.
If your branch lacks the file, get it with `git fetch origin
claude/project-review-recommendations-x1ihdn` and `git show` or `git
checkout` it from there, or ask the user whether that branch has been merged.

**Decisions already made (don't reopen them):**
1. Apps stay in `apps/` in this repo until `sdk-v1.0`, then move to
   `cyd-palm-apps`.
2. `sdk/` and `apps/` are MIT. `NOTICE` gets a GPLv3 §7 permission for apps
   that use only `palm_app.h`; the wording needs checking before the first
   app is shared.
3. **No signing and no Developer Mode.** Apps are checked by a manual
   inspection that prints a text report.
4. The API has 44 slots (§5.3), with the `PA_UPDATE`/`PA_REPLACE` file modes
   and 4 open files (§5.8). Text is UTF-8, drawn in the Palm font with
   accents folded to ASCII. The user's Keep/Cut/Change review of the list
   hasn't come back yet; the review page is
   https://claude.ai/artifact/D9FodsMfNvoS2EC3whQ9f9.
5. **No flash or NVS encryption.** Protection is in the API (§8.2).
6. **Inspection runs locally** (`palm-inspect`, one pinned Docker image,
   networking off). It never uses a commit or this repo's Actions.

**Measured already (QEMU, launcher showing):**

| Measurement | Value |
|---|---|
| Free heap | 114,352 B (largest block 90,112 B) |
| Largest free IRAM block | 30,720 B |
| Data heap after a 16 KB IRAM load | Unchanged |
| LVGL pool free | 11,024 B |
| C Sudoku board | 9.5 ms |

### Phase 0: loader spike (throwaway code; the numbers are the product)

1. Make a scratch copy of the repo and start the IDF container. Add LVGL
   9.5.0 and `elf_loader` as local components (see the environment notes).
   Build the baseline and confirm 1,582,695 B.
2. Stub the battery ADC read and touch calibration for the emulator only.
   Add a small data partition to the scratch `partitions.csv` to hold a test
   pack.
3. Build a hello pack with the IDF toolchain as a relocatable ELF. It takes
   a tiny function table (for example, a log call and a counter). It needs a
   string in `.rodata`, a variable in `.bss` and a call back into the
   firmware.
4. Load it with `elf_loader` from the flash partition, call its entry point,
   and unload it. Then measure each Phase 0 exit criterion in plan §10:
   - loader flash cost ≤ 40 KB (`idf.py size-components`, diffed against the
     baseline);
   - `.text` and literals in IRAM, and `.rodata`, `.data` and `.bss` in DRAM
     (print the addresses; the `.rodata` string must read correctly);
   - data heap before and after load equals the data block size
     (`heap_caps_get_free_size` and `heap_caps_get_largest_free_block`, for
     both 8-bit and exec memory);
   - IRAM and heap fully back after 50 load/unload cycles;
   - time to SHA-256 and load a 24 KB pack;
   - main-task stack high-water mark around an app call
     (`uxTaskGetStackHighWaterMark(NULL)`), which checks the 2 KB app
     budget;
   - also the heap scrub cost (§8.2 item 2): zeroing all free heap.
5. If the loader costs more than 40 KB, or mishandles ESP32 IRAM (byte
   access to IRAM faults), prototype the small custom loader from plan §3
   instead.
6. Record every number and how it was measured in `BUILD_PROGRESS.md`
   (a new "App platform: Phase 0" section). Update plan §2 and §10 if a
   number changes the design. Commit only docs; the spike code stays in
   scratch.

### Phase 1: groundwork (real firmware changes; each item its own commit)

1. **Secrets out of resident RAM** (plan §8.2 item 1). Do this first; it's
   worth having even if apps never ship.
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
     closing Settings. Add it to `ci.yml`.
   - Add a `[d]` bench item for the user: Wi-Fi join and an iCloud HotSync
     still work.
2. **Versions.**
   - Set the firmware version from `git describe --tags --always --dirty`
     (`PROJECT_VER`), and check what ESP-IDF already does by default.
   - Show it in Settings ▸ About with "Apps API 0.1 (unstable)". The About
     text is in `ui.c`, near the Settings row list with "About". The sim
     needs an `esp_app_get_description` shim in `sim/include`.
   - Make `tools/package_firmware.py` and `ci.yml` use the same string.
3. **SDK skeleton.**
   - `sdk/palm_app.h` with the §5.3 table as `struct pa_api` (API 0.1,
     marked unstable), the events, error codes, the `PA_APP`/`PA_MAIN`
     macros and the inline wrappers.
   - `sdk/abi/pa_api_v0.txt`, `sdk/tools/abicheck.py`, `make -C sdk
     abicheck`, and a step in `ci.yml` host gates.
   - Nothing implements the header yet (Phase 2); just check it compiles
     with `-std=c11 -Wall -Wextra -Werror`.
4. **Licences.**
   - `sdk/LICENSE` and `apps/LICENSE`: MIT, "Copyright (c) 2026 the CYD Palm
     contributors" (same as `bridge/LICENSE`).
   - A `NOTICE` section for them, plus the draft GPLv3 §7 permission,
     marked "wording to be checked".
   - SPDX headers in the new files.
5. **Rename Games to More.**
   - The launcher label and `show_games()` title in `ui.c`; the same five
     tiles.
   - Update `sim/tests/smoke.txt` (its comments and shot names mention
     Games) and user-facing docs.
   - Look at the new smoke screenshots before ticking `[s]`.

When Phase 1 is done: tick its `[s]` items in `BACKLOG.md`, move the
finished text to `BUILD_PROGRESS.md`, and stop for the user's API review
before starting Phase 2.
