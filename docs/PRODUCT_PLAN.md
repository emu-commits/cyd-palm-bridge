# Product plan — from open-source PDA to a device you can sell

_Written 2026-07-19. The pivot: stop extending the Japanese trainer (Tier 2 is a
good stopping point — see below) and turn CYD Palm into a **consumer-attractive
device** worth listing on Etsy._

## TL;DR — the recommendation

The thing that will actually **sell** this device is not more apps — it's the
**at-a-glance lock-screen dashboard** (people buy a "charming glanceable desk
companion"), a **frictionless out-of-box setup**, and a **pre-flashed / one-click-flash**
experience. Games are the delightful hook that makes it feel like a toy you keep on
your desk.

So the order is: **Dashboard → Alarms/Power polish → Games → Setup/flashing polish.**

### Decisions locked (2026-07-19)
- **BLE + companion iOS app: dropped.** Wi-Fi only. (Rationale kept in §2 for the
  record; the effort goes into dashboard / games / polish instead.)
- **Aesthetic: strictly mono Palm** everywhere — preserve the pure monochrome LCD
  identity; no color accents.
- **First step: a visual mock-up of the dashboard** before any firmware.

### Why end the Japanese route at Tier 2 (agreed)
Tier 2 trains *stroke order and recognition*, but the on-canvas trace-the-model
interaction doesn't build the muscle memory that transfers to pen-and-paper writing —
and that gap only compounds at kanji scale (15–20 strokes). It's a satisfying feature
to *have shipped*, but it's a teaching tool, not a selling point, and further tiers are
a large investment against a narrow audience. Freeze it where it is; it stays in the
launcher as a bonus, not a headline.

---

## The governor: what every idea below has to fit inside

These haven't changed and they constrain all four asks:

- **No PSRAM.** ~35 KB free heap while Wi-Fi + TLS + LVGL are all up; ~256 KB free
  with the radio down. **LVGL draw pool is a hard 24 KB.**
- **Pool-safe widgets only:** labels, lists, tables, buttonmatrix, **I1 (1-bpp)
  canvas**. `lv_bar/slider/arc/meter` allocate draw layers → pool exhaustion → WDT
  freeze. (This is why the HotSync progress is a label, not a bar.)
- **4 MB flash / 3 MB app partition.** Generous for code, fonts, word lists, puzzle
  banks. Bulk/rarely-used data lives on **SD**, loaded on demand.
- **Display is 240×320 *color* (ILI9341)** even though we render a mono Palm theme.
  We *can* introduce tasteful color where it earns its keep (dashboard accents, Wordle
  tiles) without abandoning the Palm identity.
- **Resistive single-touch** (no multitouch). Swipe = single-pointer drag (the News
  reader already does this well — reuse it).
- **No battery-backed RTC.** Time persists to NVS and re-anchors on each sync
  (`clock.c`). Fine for a glance clock; the dashboard should show "as of last sync."

Everything below is checked against this list. Where something strains it, I say so.

---

## 1) The lock-screen dashboard — the hero feature

> **Status: BUILT (emulator-verified).** Shown on boot and re-raised on every wake;
> swipe up to unlock. All tiles below render, strictly mono, pool-safe (sim heap peak
> 600 B). The hero clock is drawn on a 1-bpp canvas (a 4×7 pixel font) so it needs no
> large font. Weather/battery are the two device-bench pieces (see §"device-later").
> Code: `dash.c/.h` (weather cache + moon/sun math), the Lock-screen section of
> `ui.c`, `clock_zone_hhmm()`, `power_battery_pct()`.

**Verdict: GO, and this is where we invest the most design effort.** This is the
screen a buyer sees in the Etsy photos and the screen that's lit on their desk all
day. It should be genuinely beautiful and information-dense, in a Palm-flavored way.

**Interaction:** it's the default screen on wake. **Swipe up to unlock** into the
launcher (reuse the News drag detector). Idle → screen off (power.c already does the
backlight timeout); touch wakes back to the dashboard.

**The tiles (all from data we already have or can cheaply get):**

| Tile | Source | Cost |
|------|--------|------|
| **Time** (big) + date, 12/24h | `clock.c` (already live) | trivial |
| **World time** (2–3 zones) | `clock.c` zone list + a new "offset at now for zone i" helper | small |
| **Weather now + next-hours** (temp, rain %, air quality) | **NEW: Open-Meteo fetch during HotSync**, stored compact on SD | medium (see below) |
| **Battery %** + charging | `power.c` + battery ADC on GPIO34 (planned "U8") | small, needs calibration |
| **Next calendar event** | `data.c` (`data_datebook` / day query) — add "next upcoming" | small |
| **Next to-do due** | `data.c` (`data_todo`) — add "soonest due" | small |
| **Moon phase** | pure date math, drawn on an I1 canvas | small |
| **Sunrise / sunset** | Open-Meteo daily (free) or sun-equation math from lat/lon | small |

**The one real new dependency is weather.** Recommendation: **Open-Meteo**
(open-meteo.com) — free, **no API key**, HTTPS/JSON, and it gives us everything in the
ask: hourly `temperature` + `precipitation_probability` + `weathercode`, daily
`sunrise`/`sunset`, and (via its air-quality API) PM2.5 / an AQI value. Fetch it on the
**same Wi-Fi-up window as HotSync**, *after* the DAV sync releases its TLS buffers
(sequential, so we never hold two TLS sessions at once), **stream-parse** it (we
already stream-parse elsewhere — don't buffer the whole response), and write a compact
binary blob to SD (e.g. next 24 hourly temp/rain/AQI + today's sun times). The
dashboard then renders **entirely offline from SD** — cheap and pool-safe. Needs a
**location** (lat/lon in config, or a coarse "nearest city" picker). Moon phase is
local math (Open-Meteo doesn't provide it), so we compute it on-device.

**RAM:** the dashboard itself is ~15 labels + 1–2 small I1 canvases (moon, weather
glyph) → pool-safe, heap peak ~0. The only tight moment is the extra HTTPS GET during
sync — mitigated by doing it sequentially after DAV and stream-parsing.

**Effort:** ~medium. The layout/design iteration is the bulk of it; the data plumbing
is mostly reuse. **I'd mock this up as a visual first** (an artifact) so you can
approve the look before I build it — this is the feature where the design *is* the
product.

---

## 2) BLE sync to iPhone — DROPPED (kept for the record)

**Decision (2026-07-19): not doing this.** Wi-Fi only. The analysis below is why —
it's a second product, not a feature. Recorded so the reasoning isn't relitigated.

**Verdict: high value, high cost.** Here's the reality behind the decision:

- **The hardware is fine.** This ESP32 (D0WD-V3) has BLE. And BLE could actually be
  *easier on RAM than Wi-Fi+TLS* — the link is local, so there's **no TLS handshake**
  to fit beside everything else, and NimBLE is lighter than the Wi-Fi+TLS stack. You'd
  use **BLE *instead of* Wi-Fi**, not both at once (they share the radio and RAM),
  which is fine since it's an alternative transport.
- **The catch: iPhone will not sync to a generic BLE peripheral.** There is no
  "BLE tether to iCloud." Talking to an iPhone over BLE **requires a companion iOS
  app** that (a) speaks a custom GATT service to the device and (b) does the actual
  data work on the phone side. That app is effectively a **second product**: an Apple
  Developer account ($99/yr), App Store review, and ongoing maintenance.
- **But that companion app is also a big *upside*.** On the phone it can use
  **EventKit + Contacts** directly — which means the buyer **never creates an
  app-specific password or edits `config.ini`**. That current setup flow is the #1
  Etsy-return risk, and the companion app *is the cure*. It can also pull weather
  (WeatherKit/Open-Meteo) and push it over BLE. So BLE isn't just "another sync
  transport" — it's the path to a **tap-to-pair, zero-config** experience.
- **Throughput is a non-issue.** PIM records and a weather blob are tiny; BLE's few
  KB/s is plenty.

**Staged plan:**
- **Phase A (firmware, small, sim-testable):** abstract the sync *transport* so
  "DAV over TLS" and "BLE to companion" are pluggable behind the existing reconcile
  engine. Low risk, useful regardless.
- **Phase B (the real cost): the companion iOS app** — GATT service, EventKit/Contacts
  bridge, pairing UX, App Store submission. This is its own milestone.

**Outcome:** dropped. Setup friction gets solved the cheaper way instead — a friendlier
on-device first-run wizard + a printed quick-start card (§4), not an App Store product.

---

## 3) Games — the delight hook

> **Status: Minesweeper + Wordie BUILT (emulator + host-test verified).** The **Games**
> launcher app (die icon) opens an **icon folder** (a grid mirroring the app launcher,
> one icon per game):
> - **Minesweeper** — a 9×9 board on a 1-bpp canvas, Dig/Flag mode toggle,
>   first-tap-safe flood-reveal, win/lose. `minesweeper.c` + `make -C sim mines`.
> - **Wordie** (the Word clone, renamed off "Wordle") — a five-letter, six-guess game.
>   The guess grid AND an on-screen QWERTY keyboard are drawn mono on one 1-bpp canvas;
>   923-word answer bank, deterministic daily word from the date, Wordle two-pass
>   scoring. Mono state language: CORRECT = filled/knockout, PRESENT = double border,
>   ABSENT = slashed. The Graffiti strip also types. `wordie.c` + `make -C sim wordie`.
>
> Both are pure C with host gates wired into CI; the views are in `ui.c` (canvas-drawn,
> so pool-cheap — smoke32 peak ~600 B).
>
> **Sudoku SHIPPED (2026-07-21):** `sudoku.c` — a seeded generator that guarantees a
> unique solution (fill a random grid, dig holes with a solution-counting check),
> rules + conflict flags + solved detection; `make -C sim sudoku`. View in `ui.c`
> (`show_sudoku`): 9×9 board + number pad on one I1 canvas, **digit entry via the
> Graffiti strip** (the on-brand hook) plus the number pad; clue cells locked with a
> corner tab; persists to `sudoku.sav`. **Next game:** Zip (the last one; most
> puzzle-generation work — a pre-generated bank on SD).

**Verdict: GO. All four are low-RAM and pool-safe.** This is what turns "an organizer"
into "a thing I want on my desk." Build a **shared game foundation** (a grid renderer
on `lv_table` / I1 canvas, tap + drag→cell mapping, a daily-puzzle-from-date seed) and
reuse it across all four. **Trademark note: rename all of them** — ship generic names
(NYT owns "Wordle"; LinkedIn owns "Zip"; "Minesweeper"/"Sudoku" are safe as generic
terms but I'd still theme them). Deterministic **"puzzle of the day" from the date** is
a great low-cost engagement loop.

| Game | Fit | Notes | Effort |
|------|-----|-------|--------|
| **Minesweeper** | table/buttonmatrix grid; small board array | tap = reveal, long-press = flag (fits single-touch) | low |
| **Word (Wordle-clone)** | on-screen keyboard (we have one) + color tiles | needs a word list (~100 KB, flash or SD); daily word from date; **rename** | low–med |
| **Sudoku** | 9×9 table; **digit entry via Graffiti** (we already recognize 0–9!) | ship a puzzle bank on SD, or a small generator/solver | med |
| **Zip-clone** | trace a path 1→N filling the grid (Hamiltonian path) | drag-to-trace input; ship a **pre-generated puzzle bank** on SD; **rename** | med |

**Priority within games:** **Minesweeper + Word first** (broadest appeal, lowest
risk), then **Sudoku** (the Graffiti-digit synergy is genuinely charming and on-brand),
then **Zip** (most puzzle-generation work). Put them behind a **"Games" launcher
icon/folder** so they don't clutter the PDA identity.

---

## 4) Super-polish & wrap-up — the consumer-readiness checklist

These are the things that separate "a cool GitHub project" from "a product a stranger
pays for and doesn't return." Ranked by impact on sellability:

1. **One-click flashing / pre-flashed units.** The README today says "you flash it
   yourself" — a non-starter for Etsy. Two options, do both: **ship pre-flashed
   devices**, and provide a **web flasher** (ESP Web Tools over WebSerial in Chrome) so
   buyers can update with one click, no toolchain. Add **OTA updates** over Wi-Fi so we
   can fix bugs post-sale. *(Highest impact after the dashboard.)*
2. **Zero-config setup.** The app-specific-password + `config.ini` flow is the biggest
   friction. Near-term: a friendlier on-device first-run wizard (we have an onboarding
   hint already) + a printed quick-start card. Long-term: the **BLE companion app**
   (§2) removes it entirely.
3. **Alarms that actually fire.** We already *sync* VALARM data — make Date Book alarms
   wake the screen (and buzz, if the board's buzzer is populated). A PDA that reminds
   you is worth far more than one that just displays. High perceived value, modest cost.
4. **Power/battery honesty.** Calibrate the GPIO34 battery reading (CYD dividers are
   coarse), show a **low-battery warning** and a charging indicator. Don't show a fake
   precise %.
5. **Personalization = perceived value.** Dashboard theme/watch-face options, a name on
   the lock screen, 12/24h, °C/°F, brightness schedule. Cheap to add, sells well.
6. **Reliability / never-brick.** Graceful "no SD card" mode, factory reset, crash
   recovery, sane defaults. A returned device is worse than a lost sale.
7. **Sound/haptics (if hardware supports it).** Verify whether this board's buzzer/
   speaker pad is populated; if so, subtle chirps for alarms + game feedback are
   charming and near-free.
8. **Brand & listing assets.** A product **name**, a real **device photo** in the
   README (it's the single best thing the README could add), packaging, and the Etsy
   listing copy. *(Non-code, but gating for launch.)*
9. **Enclosure.** A case is required to sell a bare PCB as a product — but that's
   CAD/hardware, **out of software scope**; flagging it as a launch dependency.
10. **Licensing reality (commercial).** The **firmware is GPLv3** (it reuses PumpkinOS's
    GPLv3 fonts/icons). Selling GPLv3 hardware is completely fine **as long as buyers
    can get the source** — which they can, it's public, so we're compliant. The
    constraint: we **can't bolt proprietary-locked features onto the GPL parts**, and
    the companion iOS app should be structured as a separable work. Worth being
    deliberate about before money changes hands.

---

## Sequencing (BLE dropped, mono, dashboard-first)

1. Lock-screen **dashboard** — mock approved, **built + emulator-verified.** ✅
   Remaining for it: the device-side weather fetch + battery ADC (below).
2. **Alarms fire** + **battery/power** honesty.
3. **Games**: Minesweeper + Word first, then Sudoku, then Zip (all **renamed**,
   strictly mono).
4. **Web flasher + OTA** + first-run setup wizard + brand/photo/name.

Each item ships as its own PR through the existing branch/CI flow (firmware ESP-IDF +
wasm gates green before merge), sim-verified where the sim can prove it (everything but
real battery ADC, live weather, and buzzer — those are device-bench verifies).

---

## Still-open (smaller) decisions

- **Weather location:** single lat/lon in config, or an on-device city picker?
  (Leaning: config lat/lon for v1, picker later.)
- **Buzzer:** confirm whether this board's speaker/buzzer pad is populated (gates the
  audible-alarm + game-sound polish).

Resolved 2026-07-19: **BLE dropped**, **strictly mono Palm**, **dashboard mock-up
first**.
