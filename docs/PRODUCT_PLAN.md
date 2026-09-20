# Product plan — from open-source PDA to a device you can sell

_Written 2026-07-19. The pivot: stop extending the Japanese trainer and turn CYD Palm
into a **consumer-attractive device** worth listing on Etsy._

**The thesis.** What sells this device is not more apps — it's the **at-a-glance
lock-screen dashboard** (a charming glanceable desk companion), a **frictionless
out-of-box setup**, and a **pre-flashed / one-click-flash** experience. Games are the
delightful hook that makes it feel like a toy you keep on your desk.

**Amended 2026-08-17 — one app *does* sell it.** Coach is the named exception. The
thesis holds for *generic* apps (another notepad sells nothing); what Coach has is
that it can only exist on this hardware — the progress bar is a **Graffiti stroke you
drew**, and the session is **sealed** behind a screen that cannot be navigated away
from. Both are properties of a stylus PDA with no notifications.

So: **the dashboard sells it at rest, Coach sells it in use, games make it a toy you
keep.**

> **What's left to do lives in `BACKLOG.md`**, which carries the consolidated list for
> all three docs. This file is the locked decisions and the ship checklist.

---

## Decisions locked

- **BLE + companion iOS app: DROPPED** (2026-07-19). Wi-Fi only. §2 keeps the
  reasoning so it isn't relitigated.
- **Aesthetic: strictly mono Palm** everywhere. No colour accents.
- **Japanese route ends at Tier 2.** Tier 2 teaches stroke order, but tracing a model
  on glass doesn't build the muscle memory that transfers to pen and paper, and the
  gap only widens at kanji scale (15–20 strokes). It's a teaching tool, not a selling
  point. It stays in the launcher as a bonus, not a headline.
- **Weather location: `lat`/`lon` in `config.ini` for v1** (2026-08-19); IP
  geolocation at first sync as the later product answer; **WiFi positioning
  rejected**. A forecast's resolution is kilometres, so locating the device to tens of
  metres is precision that gets discarded on arrival — and the free WiFi-positioning
  landscape has moved (Mozilla Location Service retired in 2024; Google's is billable
  and the key would have to ship inside the device, where it leaks). This is a desk
  PDA that syncs at home over a known SSID: its location *is* "home".
- **An RTC goes into the production BOM** (2026-08-17) — but read the amendment:
  **the drift argument for it is closed** (measured under a minute a day on battery,
  2026-08-27). The surviving argument is the **alarm pin**, which enables Coach's 8:00
  knock. See `BACKLOG.md` §Stubbed. Nothing in the shipping firmware may *depend* on an
  RTC — units already in the field have none.
- **Coach may take over the lock screen** during a focus session (2026-08-17).

---

## The governor: what every idea has to fit inside

- **No PSRAM.** ~45 KB free heap while Wi-Fi + TLS + LVGL are all up (measured at
  `wifi-up` after the 2026-08-20 heap work); ~256 KB with the radio down.
- **The LVGL draw pool is 32 KB** — `CONFIG_LV_MEM_SIZE_KILOBYTES=32`, measured on
  hardware as `lvgl pool: 31100 bytes total`. `make -C sim poolparity` fails if the
  sim and the device's sdkconfig drift apart. *(Older prose anywhere saying 24 KB is
  stale; the gate is the authority.)*
- **Pool-safe widgets only:** labels, lists, tables, buttonmatrix, **I1 (1-bpp)
  canvas**. `lv_bar`/`slider`/`arc`/`meter` allocate draw layers → pool exhaustion →
  WDT freeze. This is why HotSync progress is a label, not a bar.
- **4 MB flash / 3 MB app partition.** Generous for code, fonts, word lists, puzzle
  banks. Bulk data lives on **SD**, loaded on demand.
- **Display is 240×320 *colour* (ILI9341)** even though we render a mono Palm theme.
  Colour is available where it earns its keep, without abandoning the identity.
- **Resistive single-touch.** Swipe = single-pointer drag (the News reader's detector
  is the one to reuse).
- **No battery-backed RTC.** Time persists to NVS and re-anchors on each sync.

---

## 1) The lock-screen dashboard — the hero feature

> **BUILT and emulator-verified.** Shown on boot, re-raised on every wake, swipe up to
> unlock. Strictly mono, pool-safe. The hero clock is drawn on a 1-bpp canvas so it
> needs no large font. Restyled 2026-09-20 into the three-zone
> CONDITIONS / AHEAD / SUN & MOON layout (P7). Code: `dash.c/.h`, the Lock-screen
> section of `ui.c`, `clock_zone_hhmm()`, `power_battery_pct()`.

This is the screen a buyer sees in the Etsy photos and the screen that's lit on their
desk all day. Tiles: time + date, world time, weather now + next hours, battery, next
calendar event, next to-do, moon phase, sunrise/sunset.

**Weather is the one real dependency,** and it is built: **Open-Meteo**, free and
keyless, fetched during the HotSync Wi-Fi window *after* the DAV sync releases its TLS
buffers, `&format=csv` so there is no JSON parser and no new dependency, ~1.5 KB
spooled to SD and read a line at a time. The dashboard then renders entirely offline.

**Remaining:** the live fetch on glass (needs `latitude`/`longitude` in `config.ini`)
and the P7 restyle on glass. Both in `BACKLOG.md` §Device.

---

## 2) BLE sync to iPhone — DROPPED (kept for the record)

Not doing this. Wi-Fi only. The analysis, so it isn't relitigated:

- **The hardware is fine.** This ESP32 has BLE, and BLE could be *easier on RAM than
  Wi-Fi+TLS* — the link is local, so there's no TLS handshake to fit beside everything
  else, and NimBLE is lighter. You'd use BLE *instead of* Wi-Fi, not both.
- **The catch: iPhone will not sync to a generic BLE peripheral.** There is no "BLE
  tether to iCloud." Talking to an iPhone requires a **companion iOS app** — a second
  product: Apple Developer account, App Store review, ongoing maintenance.
- **That app would also be the big upside.** On the phone it could use EventKit +
  Contacts directly, so the buyer never creates an app-specific password or edits
  `config.ini` — which is the #1 Etsy-return risk, and the companion app *is* the cure.
- **Outcome: dropped.** Setup friction gets solved the cheaper way — a friendlier
  on-device first-run wizard (now specified as **P6, the Assistant onboarding**) plus a
  printed quick-start card.

---

## 3) Games — the delight hook

> **All four SHIPPED** (Minesweeper, Wordie, Sudoku, Zip), each with pure host-gated
> logic, a pausable play clock, a persisted best time and SD-backed state, behind a
> **Games** launcher folder. All four share **one** I1 canvas buffer (12.5 KB of BSS
> returned). Graffiti now lives in the folder too. **Renamed off trademarks** — NYT
> owns "Wordle", LinkedIn owns "Zip".

**Remaining: the on-glass ship gate** — drag feel, `New` latency, clocks across a
power cycle, heap re-measure. `BACKLOG.md` §Device.

---

## 4) The consumer-readiness checklist

What separates "a cool GitHub project" from "a product a stranger pays for and doesn't
return." Ranked by impact on sellability, with current status.

| # | Item | Status |
|---|---|---|
| 1 | **One-click flashing / pre-flashed units.** Ship pre-flashed, *and* provide a web flasher (ESP Web Tools over WebSerial). Add **OTA** so bugs are fixable post-sale. | **OPEN — highest impact after the dashboard.** Nothing built. |
| 2 | **Zero-config setup.** The app-specific-password + `config.ini` flow is the biggest friction. | **IN PROGRESS** — this is exactly **P6**, the last unbuilt group in the active phase. A printed quick-start card is still open. |
| 3 | **Alarms that actually fire.** We already *sync* VALARM data; make Date Book alarms wake the screen and buzz. | **BLOCKED on hardware** — no speaker/DAC (C3). |
| 4 | **Power/battery honesty.** Calibrate the reading, warn on low, don't show a fake precise %. | **MOSTLY DONE** — gauge, discharge curve, rested-only sampling, `power.log`, on-device Power screen. Two loose ends in `BACKLOG.md` §Device. **No charge indicator is possible**: the TP4054's `CHRG` pin isn't broken out. |
| 5 | **Personalization = perceived value.** Theme/watch-face options, a name on the lock screen, 12/24h, °C/°F, brightness schedule. | **OPEN.** Cheap to add, sells well. |
| 6 | **Reliability / never-brick.** Graceful no-SD mode, factory reset, crash recovery, sane defaults. | **OPEN.** A returned device is worse than a lost sale. |
| 7 | **Sound/haptics.** Subtle chirps for alarms and game feedback. | **OPEN — gated on the buzzer decision** (see below). |
| 8 | **Brand & listing assets.** A product **name**, a real **device photo** in the README (the single best thing the README could add), packaging, listing copy. | **OPEN.** Non-code, gating for launch. |
| 9 | **Enclosure.** Required to sell a bare PCB. | **OPEN.** CAD/hardware, out of software scope. |
| 10 | **Licensing reality.** Firmware is **GPLv3** (it reuses PumpkinOS's GPLv3 fonts/icons). | **RESOLVED — compliant.** Selling GPLv3 hardware is fine as long as buyers can get the source, and it's public. The constraint: no proprietary-locked features bolted onto the GPL parts. |

**Still-open hardware question: is this board's speaker/buzzer pad populated?** It
gates items 3 and 7. **Second reason as of 2026-08-17:** Coach's session end is
visual-only (a backlight pulse) because there is no speaker/buzzer/DAC — fine on a desk
in your eyeline, wrong in a bag, and currently a disclosed limitation. A piezo also
unlocks the once-a-minute session *tick*, the cheapest presence the device could have.
See `COACH_DESIGN.md` §9.

---

## 5) Coach — the app that sells it in use

> **BUILT + merged (#39), flashed, boot-verified.** Design and measured costs in
> `COACH_DESIGN.md`. The on-glass notes pass is round 2 and still open —
> `BACKLOG.md` §Device.

A ritual-based focus timer. **Six taps and one stroke** per session, no typing: three
auto-advancing selectors (energy / domain / intention), one Graffiti mark, a sealed
25-minute session, two taps to record how it went.

**Why it sells, in the order a buyer notices:**

1. **The sigil.** You draw one stroke, and *that mark* is the progress bar — it inks in
   from the bottom as you work. Finished marks are kept, so a month of focus is a wall
   of your own handwriting. No phone app can copy this; it needs a digitizer and a
   unistroke recognizer, both of which were already in the firmware. It is also the
   most photographable thing the device does, which matters for how a hand-made gadget
   spreads.
2. **Sealed mode.** During a session the silkscreen row is unreachable — not disabled,
   *covered*. Giving up costs a five-second hold and breaks the streak. This is the
   "phone in a lockbox" mechanic in software, and it is the honest argument for owning
   a dedicated device instead of using the thing that interrupts you.
3. **It produces something that outlives it.** Each session writes a Date Book block
   (and an optional note as a Memo), so the next HotSync puts your real focus hours in
   iCloud. For anyone who bills time, that alone justifies the purchase.

**It cost almost nothing:** 226 bytes of static DRAM and **zero new canvas buffers** —
sealed mode makes it impossible to open a game mid-session, so the sigil reuses
`game_cv_buf`. For scale, the BLE mesh analysis failed the link by 24,064 bytes before
any application code existed.

**Deliberately not built, and worth holding the line on:** badges, XP, levels,
leaderboards, accounts, generated encouragement copy, streak freezes. The audience for
a hand-made Palm-style device is exactly the audience that finds gamification
insulting — and we are selling a device that *can't reach you*, so we should not build
the thing we are selling against.

---

## Sequencing

1. **Lock-screen dashboard** — built + emulator-verified. ✅ *(Device-side weather
   fetch still needs a live run.)*
2. **Battery/power honesty** — gauge and instrumentation shipped. ✅
   **Alarms still blocked** on sound hardware.
3. **Games** — all four shipped. ✅ *(On-glass ship gate open.)*
4. **Coach** — built out of order (2026-08-17), ahead of 2 and 4, because it turned out
   to be the strongest single reason to pick the device up daily. ✅
5. **The three speakers** — Coach greeting, the Guru app, the lock restyle, HotSync
   cancel, the list restyle: all merged (#52). **P6, the Assistant onboarding, is the
   last unbuilt group**, and it is also checklist item 2.
6. **Next, for shipping:** web flasher + OTA (item 1), then personalization and
   reliability (5, 6), then brand/photo/name (8).

Each item ships as its own PR through the existing branch/CI flow (firmware ESP-IDF +
wasm gates green before merge), sim-verified where the sim can prove it — everything
but real battery ADC, live weather, live sync and sound, which are device-bench
verifies.
