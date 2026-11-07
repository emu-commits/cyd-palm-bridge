# How CYD Palm works

A native PDA on the base CYD (`ESP32-2432S028R`: **no PSRAM**, 4 MB flash,
about 300 KB of SRAM). It is a Palm-style handheld that works offline: Memos
and Graffiti, a lock screen, a news reader, games, Study (spaced repetition),
and the Guru and Coach apps. HotSync, over Wi-Fi when asked, sets the clock
and fetches the news and the weather. PumpkinOS is a *donor* (data formats,
fonts, icons, layouts), not a runtime: emulating PalmOS was ruled out early
(Dragonfruit needs 8 MB of PSRAM, and a whole PumpkinOS port is
desktop-class with a 150 KB framebuffer).

Until 2026-11-07 it also had Date Book, Address and To Do, synced both ways
with iCloud over CalDAV/CardDAV. That was archived (git tag
`archive/pim-caldav` holds the apps, the sync engine and its gates).

The rest of the docs:
- [`BUILD_PROGRESS.md`](BUILD_PROGRESS.md): the changelog, measured RAM and
  flash numbers, and lessons learned.
- [`BACKLOG.md`](BACKLOG.md): open work and the device checks.
- [`SRS_PLAN.md`](SRS_PLAN.md) and [`COURSE_FORMAT.md`](COURSE_FORMAT.md):
  Study, the spaced-repetition app, and its course files.
- [`COACH_DESIGN.md`](COACH_DESIGN.md), [`GURU_HABITS.md`](GURU_HABITS.md):
  the two coaching apps.
- [`GAME_PLAN.md`](GAME_PLAN.md), [`GAME_FORMAT.md`](GAME_FORMAT.md),
  [`DEX_PLAN.md`](DEX_PLAN.md): Critters and the Dex.

## The pieces

| Folder | What it is | License |
|---|---|---|
| `bridge/` | Portable plain C, built unchanged for the host gates and the device: the PDB container and category table, Find, the calculator, `config.ini`, the RSS reader and its store, the weather and location parsers, the lock screen's rules, the transport's breaker. | MIT (`bridge/LICENSE`) |
| `firmware/` | The ESP-IDF app: the LVGL UI (`main/ui.c`), the device's HTTPS transport over mbedTLS (`dav_esp.c`, for HotSync's fetches), storage on the SD card, Wi-Fi, power. Compiles `bridge/` unchanged as a component. | GPLv3 (it uses PumpkinOS's Palm fonts and icons) |
| `sim/` | The same UI, data layer and Graffiti recognizer built for the desktop and the browser (wasm), with the device's memory limits. | GPLv3 |
| `tests/` | The host gates: Find, the parsers (some under sanitizers), the course and game file readers. | |
| `tools/` | Generators (fonts, faces, icons, the demo course), `mkcourse.py` (Study's course builder) and `mkgame.py` (Critters' converter). | |
| `courses/` | The demo course that ships with the firmware (CC0), and Critters' game file. | CC0 / see `courses/critters/` |

## Memory: the constraint behind most decisions

With no PSRAM, TLS, Wi-Fi and LVGL share about 80 KB of heap while a HotSync
runs. It fits because:
- the LVGL object pool is **32 KB** (`CONFIG_LV_MEM_SIZE_KILOBYTES`; LVGL
  reports 31,100 B on hardware), and `make -C sim poolparity` fails if the
  simulator and the device drift apart;
- mbedTLS uses dynamic buffers, and the Wi-Fi buffers are trimmed;
- every fetch is spooled to the SD card and parsed a window at a time, and
  every database is read and rewritten a record at a time;
- static DRAM is watched on every change (about 73 % used since the sync
  engine left, 2026-11-07); a screen allocates what it needs when it opens and
  frees it when it closes;
- passwords live only in the device's NVS secret store, read when needed and
  wiped afterwards (`SECURITY.md`).

The simulator models this, not just the pixels: the pool is the device's
size, and the general heap is capped near the device's interactive free heap
(`sim/sim_heap.h`), so an allocation that would fail on hardware fails in
the simulator too.

## HotSync (`firmware/main/hotsync.c`)

A background task: Wi-Fi up (the saved networks in order, the last good one
first), the clock from SNTP (TLS needs it), the enabled news feeds (each
streamed to the card and parsed by `bridge/rss.c` into `bridge/news.c`'s
store), the location from the IP when it isn't pinned (`bridge/geoip.c`),
the weather from Open-Meteo (`bridge/wxfetch.c`), Wi-Fi down. Each stage can
be cancelled between stages, replaces its cache whole or not at all, and
reports its own outcome on the status line. No account is involved.

## Memos (`firmware/main/data.c`)

`MemoDB.pdb` on the card, read and rewritten a record at a time through
`bridge/pdb.c` (a rewrite goes to a temp file and replaces the live one only
if every record was read and written). A card with no database gets an empty
one with the standard categories; there is no demo data. A memo can hold
checklist lines (`[ ]`, `[x]`), drawn as boxes and ticked on the detail
screen. Find (`bridge/find.c`) streams the database for words that start with
the query.

## The firmware

The UI is LVGL in a monochrome Palm theme with the authentic Palm fonts and
icons: a launcher grid, silkscreen Home / Menu / Find / Calc buttons beside
the Graffiti area, Palm's pull-down menus, the category pop-up, and Details.
The launcher is Settings, HotSync, Games / News, Memos, (free) / Study, Guru,
Coach, plus a lock screen with the time, weather and Study's due reviews.
Graffiti is a unistroke recognizer, gated at 99.7 % accuracy on letters
(`make -C sim graf`).

Configuration lives in `config.ini` on the SD card and is edited on the device
in **Settings**; passwords are the exception (see above). Guru's habit list
is `guru.txt` on the card, edited on the device or on a computer
(`GURU_HABITS.md`).

## The host gates, on a computer

```
make                    # builds the host programs into build/
make test               # Find and the parsers, no network needed
make ftest              # the parsers and file readers under ASan/UBSan
```

## The firmware, built yourself

```
. ~/esp/esp-idf/export.sh          # ESP-IDF v5.5
cd firmware
idf.py set-target esp32            # first time only
idf.py -p /dev/ttyUSB0 flash monitor
```

LVGL is pinned at 9.5.0 (`firmware/main/idf_component.yml`). On first boot
the device makes an empty Memos database if the card has none; Study installs
its demo course on the card the first time it's opened.

## The simulator

```
sudo mkdir -p /sdcard && sudo chmod 777 /sdcard
make -C sim smoke     # native and headless: a scripted tour of the UI, with screenshots
make -C sim wasm      # the browser build, with Emscripten, into sim/build/web/
```

The simulator fetches LVGL v9.2.2 on its first build. In the browser, records
persist in IndexedDB; passwords are never written to browser storage, and
HotSync fills the news with samples.

## Gates

`.github/workflows/ci.yml` is the authority. Before a change: `make test`,
`make ftest`, every `make -C sim` gate CI lists (including `smoke` and
`smoke32`, the tour built 32-bit), `python3 tests/mkcourse_test.py`,
`python3 tests/mkgame_test.py`, and the firmware build.
