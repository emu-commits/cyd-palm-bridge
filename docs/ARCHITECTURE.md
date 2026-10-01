# How CYD Palm works

A native PDA on the base CYD (`ESP32-2432S028R`: **no PSRAM**, 4 MB flash,
about 300 KB of SRAM). It reads and edits Palm PIM databases and syncs them
both ways with CalDAV/CardDAV servers such as iCloud. PumpkinOS is a *donor*
(data formats, fonts, icons, layouts), not a runtime: emulating PalmOS was
ruled out early (Dragonfruit needs 8 MB of PSRAM, and a whole PumpkinOS port
is desktop-class with a 150 KB framebuffer).

The rest of the docs:
- [`BUILD_PROGRESS.md`](BUILD_PROGRESS.md): the changelog, measured RAM and
  flash numbers, and lessons learned.
- [`BACKLOG.md`](BACKLOG.md): open work and the device checks.
- [`SRS_PLAN.md`](SRS_PLAN.md) and [`COURSE_FORMAT.md`](COURSE_FORMAT.md):
  Study, the spaced-repetition app, and its course files.
- [`COACH_DESIGN.md`](COACH_DESIGN.md), [`GURU_HABITS.md`](GURU_HABITS.md):
  the two coaching apps.

## The pieces

| Folder | What it is | License |
|---|---|---|
| `bridge/` | The Palm ↔ CalDAV/CardDAV codecs and the sync engine, plain C. Also the host CLI `bridge_cli`. | MIT (`bridge/LICENSE`) |
| `firmware/` | The ESP-IDF app: the LVGL UI (`main/ui.c`), the device's DAV transport over mbedTLS (`dav_esp.c`), storage on the SD card, Wi-Fi, power. Compiles `bridge/` unchanged as a component. | GPLv3 (it uses PumpkinOS's Palm fonts and icons) |
| `sim/` | The same UI, data layer and Graffiti recognizer built for the desktop and the browser (wasm), with the device's memory limits. | GPLv3 |
| `tests/` | The host gates: codec round trips, sync against a local Radicale server, parsers under sanitizers. | |
| `tools/` | Generators (fonts, faces, the demo course) and `mkcourse.py`, Study's course builder. | |
| `courses/` | The demo course that ships with the firmware (CC0). | CC0 |

## Memory: the constraint behind most decisions

With no PSRAM, TLS, Wi-Fi, LVGL and the sync working set share about 80 KB of
heap while a sync runs. It fits because:
- the LVGL object pool is **32 KB** (`CONFIG_LV_MEM_SIZE_KILOBYTES`; LVGL
  reports 31,100 B on hardware), and `make -C sim poolparity` fails if the
  simulator and the device drift apart;
- mbedTLS uses dynamic buffers, and the Wi-Fi buffers are trimmed;
- the sync engine streams records to and from the SD card instead of holding
  a collection in RAM;
- static DRAM is watched on every change (about 89 % used); a screen
  allocates what it needs when it opens and frees it when it closes;
- passwords live only in the device's NVS secret store, read when needed and
  wiped afterwards (`SECURITY.md`).

The simulator models this, not just the pixels: the pool is the device's
size, and the general heap is capped near the device's interactive free heap
(`sim/sim_heap.h`), so an allocation that would fail on hardware fails in
the simulator too.

## The sync engine (`bridge/sync.c`)

**Change detection.** A record changed locally if its canonical body's FNV
hash differs from the one stored at the last sync (or Palm's delete bit is
set). It changed on the server if its ETag differs from the stored one (or
it appeared or vanished). Records are matched by UID, not by URL, so a
server moving an object doesn't duplicate it.

**Reconciliation** runs the full (local × server) matrix of new, modified
and deleted, does the DAV operations (conditional `PUT` with `If-Match`,
`DELETE`, `GET`), writes the merged PDB, and rewrites the map
(`state/<collection>.map`, rows `uid⇥href⇥etag⇥hash`). A **conflict** is a
record changed on both sides, resolved by policy: `server` wins, `local`
wins, or `both` (Palm's keep-both: the server copy stays and the local one is
added as a new record). Modify beats delete under `both`.

**Deltas (RFC 6578).** The engine prefers the `sync-collection` REPORT: it
sends the token from the last run, and the server returns only what changed.
An invalid or expired token falls back to a full resync, and a server
without `sync-collection` falls back to `PROPFIND`. The token is kept as a
`#synctoken` line in the map. iCloud supports it, so syncs after the first
are deltas.

**A real account.** Date Book syncs a window of days (`sync_set_window`;
by default yesterday to two weeks ahead), listed with a CalDAV time-range
query that the server applies to recurrences. An object that leaves the
window leaves the device and is never deleted on the server; one the device
changed is checked with a one-object PROPFIND first, so an edit still goes
up and a tombstone still deletes. Bodies are fetched in batches
(`calendar-multiget` / `addressbook-multiget`), streamed onto the card and
looked up there, rather than one GET each. The index files are sorted in a
4 KB buffer, or in runs on the card when they don't fit, so a collection's
size costs card space rather than RAM.

**Safety.** Every durable file is replaced whole (crash-safe writes). A sync
that would delete most of a collection is held back (the mass-delete guard),
deletions travel as tombstones, the demo records are never pushed, and a
collection too large for the device's RAM is refused without changing
anything (`tests/toobig.c`).

**Fidelity.**
- Timezones: timed events carry `TZID` and a matching `VTIMEZONE`; UTC
  inputs are converted to local time with the right DST offset.
- Alarms (`VALARM`), exceptions (`EXDATE`), and text in CP1252/Latin-1 ↔
  UTF-8.
- To Do ↔ `VTODO` (due date, priority, completion).
- vCard is a set of typed values, not Palm's five ordered phone slots, so the
  slot order isn't kept across a round trip (the data is).

**Categories → collections.** `sync_categorized` syncs each Palm category
against its own collection (its own map and token) and writes one merged PDB.
`bridge_cli synccat` routes by display name. Calendars and Reminders lists
can be split this way; iCloud has one address book, so contacts keep their
category on the record. Memos stay on the device: iCloud Notes has no
CalDAV/CardDAV surface.

**What a real server taught us:** Radicale normalizes objects (it adds
`DTSTAMP`) and rejects a vCard without a `UID`; servers return `PROPFIND`
members unordered, so pulls sort by unique ID for a deterministic PDB.

## The firmware

The UI is LVGL in a monochrome Palm theme with the authentic Palm fonts and
icons: a launcher grid, silkscreen Home / Menu / Find / Calc buttons beside
the Graffiti area, Palm's pull-down menus, the category pop-up, and Details.
The apps are Date Book, Address, the Planner (To Do and Memo), News, HotSync,
Games, Study, Guru and Coach, plus a lock screen with the time, weather and
what's ahead. Graffiti is a unistroke recognizer, gated at 99.7 % accuracy
on letters (`make -C sim graf`).

Configuration lives in `config.ini` on the SD card and is edited on the device
in **Settings**; passwords are the exception (see above).

## The host bridge, on a computer

```
make                    # builds the host programs into build/
make test               # codec round trips and parsers, no server needed
make ftest              # the parsers under ASan/UBSan
./tests/run_gates.sh    # starts a local Radicale, runs every server gate, stops it
```

`run_gates.sh` needs Radicale: `python3 -m venv davvenv &&
./davvenv/bin/pip install radicale`, or `pip3 install radicale`.

Against a real account (`DAV_BASE`, `DAV_USER`, `DAV_PASS`, `DAV_CAL`,
`DAV_CARD` and `BRIDGE_TZ` set the target):

```
export DAV_BASE=https://caldav.icloud.com
export DAV_USER='you@icloud.com'
export DAV_PASS='xxxx-xxxx-xxxx-xxxx'     # an app-specific password
export BRIDGE_TZ=America/New_York
./build/bridge_cli discover               # prints the host and collection paths
export DAV_BASE=https://pNN-caldav.icloud.com
export DAV_CAL='1234567890/calendars/home'
export DAV_CARD='1234567890/carddavhome/card'
./build/bridge_cli sync pdb/DatebookDB.pdb pdb/AddressDB.pdb
```

`discover` follows the `caldav.icloud.com` → `pNN-caldav.icloud.com`
redirect, reads `current-user-principal`, then the calendar and address-book
homes, and lists each collection's URL.

## The firmware, built yourself

```
. ~/esp/esp-idf/export.sh          # ESP-IDF v5.5
cd firmware
idf.py set-target esp32            # first time only
idf.py -p /dev/ttyUSB0 flash monitor
```

LVGL is pinned at 9.5.0 (`firmware/main/idf_component.yml`). On first boot
the device writes demo records for the apps that have no database yet; Study
installs its demo course on the card the first time it's opened.

## The simulator

```
sudo mkdir -p /sdcard && sudo chmod 777 /sdcard
make -C sim smoke     # native and headless: a scripted tour of the UI, with screenshots
make -C sim wasm      # the browser build, with Emscripten, into sim/build/web/
```

The simulator fetches LVGL v9.2.2 on its first build. In the browser, records
persist in IndexedDB; passwords are never written to browser storage, and
sync is stubbed.

## Gates

`.github/workflows/ci.yml` is the authority. Before a change: `make test`,
`make ftest`, `tests/run_gates.sh`, every `make -C sim` gate CI lists
(including `smoke` and `smoke32`, the tour built 32-bit), `python3
tests/mkcourse_test.py`, and the firmware build.
