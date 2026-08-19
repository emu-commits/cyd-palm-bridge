# Work backlog — the single source of truth

The one list of what's left to do. Grouped by **where it can be worked**, because
that's the binding constraint on this project (a base CYD with no PSRAM, developed
mostly away from the bench via the browser simulator).

- **`[sim]`** — buildable and verifiable *today* in the emulator
  (`make -C sim smoke`, or the live page at
  https://emu-commits.github.io/cyd-palm-bridge/). No hardware needed.
- **`[device]`** — needs the physical CYD (touch panel, Wi-Fi/TLS, sound, battery)
  or a live iCloud account to verify. Can often be *written* off-device and
  flash-verified on return.
- **`[blocked]`** — has an unmet prerequisite spelled out in the item.

> **The docs folder is four files.** This one is what's LEFT;
> `BUILD_PROGRESS.md` is history + the hard-won lessons + the hardware/RAM reference;
> `PRODUCT_PLAN.md` is the path to shipping (the consumer-readiness checklist and the
> locked product decisions); `COACH_DESIGN.md` is the one surviving per-app design
> spec, kept because Coach is the flagship and its rule engine, storage format and
> optional-hardware backlog are all specified there rather than in code comments.
> It was deliberately three files before 2026-08-17 — the bar for a fifth is high. The original design analyses — `UI_ROADMAP.md` (memory/
> hardware), `ROADMAP.md` (sync/port), `SIMULATOR_PLAN.md` (the emulator),
> `REVIEW_2026-07-15.md` (the review the `O#`/`M#`/`C#`/`I#` IDs come from) and
> `KANA_TRAINER.md` — were retired once each had been built or decided against. Their
> load-bearing facts were salvaged into `BUILD_PROGRESS.md`; the full text is in git
> history: `git log --diff-filter=D -- docs/`.

---

## Next up when we resume (priority order)

The sim-testable charm/intuitiveness backlog is done, and so is the **Games app**
(Zip was the last game — see "Recently done"). Everything sim-testable in the games
line is complete; what's left for them is the on-glass pass in **`[device]` Games on
glass** below, which is a **ship gate**.

The remaining arc is the **input experience** and the Japanese trainer. Each of 2–4
starts with a **feasibility check on the base CYD** before committing to a build.

1. **Graffiti polishing `[sim]`.** *In progress.* Built an offline **accuracy
   harness** (`sim/tests/graf_test.c`, `make -C sim graf`, now a CI gate): it
   synthesizes noisy strokes from each template and reports per-glyph accuracy +
   confusions, across **all three sets** — letters, digits, and punctuation (the
   two-step punct-shift arm is simulated). Used it to separate the worst letter
   collisions — letters **97.5% → 99.7%** mean at 3 px jitter (h→k 72→92, no glyph
   below 92%); digits and punctuation both 100%. Reshaped several glyphs from the
   on-glass feedback: `S` is a **more proportional two-lobe** stroke that survives a
   fast hand; `X` is the real **single continuous stroke** (first diagonal, a bridge
   up the right edge from bottom-right to top-right, then the second diagonal) rather
   than a two-stroke cross; `G` went from the old inward-crossbar capital (which
   stayed loop-like and read as `O` on-device) to a **wide-open C with a full-width
   horizontal mid-bar** — maximally distinct from a circle, taking g 98%→100% with
   `O` still 100%; and `?` gained the **straight downward tail** the stroke
   naturally ends on (without it, a natural downward flick read as `)`). The
   trainer's guides draw straight from these templates, so they updated for free.
   **Still to do:** the writing *feel* (ink-trail / char-echo UX) and final threshold
   tuning against real on-device `graf` telemetry (the synthetic model is a proxy);
   for a hand the built-ins still misread, **Train mode** records a per-device
   template that wins when closer (the calibration path for e.g. G↔O).

2. **Graffiti training app — a spaced-repetition (SRS) trainer `[sim]`. DONE.**
   A **launcher app** ("Graffiti", its own icon) with two modes:
   - **Drill** — shows a target glyph + its **stroke guide** (drawn on an I1 canvas
     from the recognizer's template, start dot for direction); you write it, scored
     by the real recognizer with a **graded %** (from the $1 match distance). The
     schedule is a **deterministic** (never-random) SRS: every glyph has a **level
     1–5** and a due "tick"; a correct stroke promotes it a level (longer interval →
     resurfaces less often) and, past level 5, **burns** it (retired until reset); a
     wrong stroke demotes a level and reschedules it immediately (resurfaces more
     often). The next glyph shown is always the non-burned one with the smallest due
     tick — fully reproducible. The set spans **letters + digits + punctuation**
     (the prompt nudges you to the 123 pad / punct-shift as needed). Progress
     persists to `/sdcard/graf_train.dat`; **Menu > Reset progress** wipes it.
   - **Train** — records *your own* stroke for each letter as a **per-device
     template** (`graffiti_capture_user`, stored ~3.3 KB, persisted to
     `/sdcard/graf_user.dat`, loaded at boot). Recognition then prefers a user
     template when it's a closer match, calibrating to this hand + resistive panel.
   Pool-safe throughout (labels + one canvas; heap peak 0 in the sim).

3. **Japanese trainer — FROZEN at Tier 2 (product decision, 2026-07-19).**
   `PRODUCT_PLAN.md` ends the Japanese route here: Tier 2 teaches stroke order, but
   tracing a model on glass doesn't build the muscle memory that transfers to pen and
   paper, and the gap only widens at kanji scale (15–20 strokes). It stays in the
   launcher as a bonus, not a headline. **Built and shipped:**
   - **Tier 1 (kana → sound):** the `Kana` app shows a kana in the `lv_font_kana`
     bitmap subset (IPAGothic via `lv_font_conv`); you answer the SOUND by drawing
     romaji in the Graffiti strip (Latin recognizer untouched). Deterministic SRS per
     kana, persisted to `/sdcard/kana_train.dat`.
   - **Tier 2 (write the kana):** a Sound/Write toggle; Write shows the numbered
     KanjiVG stroke model and matches each drawn stroke (`kana_write.c`, a separate
     `$1` instance) against the expected next one, enforcing official order. Stroke
     data in `kana_strokes.c` (~28 KB, from `tools/gen_kana_strokes.py`, CC BY-SA).
     Own SRS state (`KT02`). Emulator-verified end to end.
   - **The one open device item:** tune the per-stroke accept threshold (`KW_THRESH`)
     on the real resistive panel and confirm the feel. Worth doing because Tier 2 is
     shipping; it is no longer a gate on anything downstream.
   - **Tiers 3–5 (kanji: kun'yomi/gloss, vocab, writing) are NOT planned.** The full
     five-tier feasibility analysis lived in `docs/KANA_TRAINER.md`, retired with this
     decision — recover it from git history if the route is ever reopened. The
     KanjiVG→polyline pipeline is proven on kana and would extend directly.

4. **RSS reader — a TikTok-swipe, text-only feed `[sim]`. Feasibility: GO; code
   DONE (device runtime-verify pending).** A full-screen, one-item-per-view reader swiped vertically (headline
   + body text, no images), with articles fetched during **HotSync** and stored on
   **SD** for offline reading — the same offline-first model as the PIM apps. The
   RAM math checks out: fetch **streams to SD** (bounded per-item RAM, like the DAV
   sliding-window enumeration), sync stays short with a feed/item cap + conditional
   GET, and the reader holds only the current article (+ a small index) in RAM.
   Staged build:
   - **A — DONE:** `bridge/rss.c` streaming RSS 2.0 / Atom parser + HTML-to-text
     (handles CDATA vs entity-escaped HTML, entity decoding, body preference,
     item cap; bounded per-item RAM). Host-gated (`rss_test`, in `make test` +
     a sanitized `rss_asan` in `ftest`).
   - **B — DONE:** the reader app. `bridge/news.c` is an on-SD store (a fixed-record
     index + a text blob; O(1)-RAM reads by index, host-gated `news_test`). A **"News"
     launcher app** shows one article per screen (feed · position · bold title ·
     body) and navigates by **vertical swipe** (press/release Y-delta — robust on the
     headless host *and* real touch, where LVGL's gesture heuristic isn't). Seeded
     with sample articles until a real fetch runs. Pool-safe (labels + content swap
     on a gesture surface). Smoke-gated.
   - **C — DONE (compile-verified):** the HotSync fetch phase. A new device-only
     `dav_fetch_url()` streams a public feed to SD (reusing the
     `esp_http_client`/mbedTLS path + the spool-to-SD pattern, no auth);
     `fetch_news()` runs after the PIM sync (Wi-Fi still up) — for each **enabled**
     feed, GET → `rss_parse_file` → `news_add`, capped per-feed and overall, then
     `news_commit`. Compiles in the ESP-IDF CI build; **runtime-verify on device**
     (only the live network GET is unexercised off-glass). On glass, confirm: a feed
     fetches, items appear in News, sync stays reasonably quick, and heap holds
     during the fetch.
   - **D — DONE:** **feed management.** A portable, host-gated store
     (`bridge/feeds.c`, `feeds_test`) keeps the source list on SD (`feeds.txt`:
     `on/off · name · url`, ~4 KB fixed table, no heap) and ships **10 reputable
     world-English feeds** pre-seeded on first run (BBC/NPR/Guardian/Al Jazeera on
     by default). **Preferences > News feeds** is a pool-safe `lv_table` with a
     **checkbox column** (tap = enable/disable, the To Do pattern) and a URL
     **editor on the tap keyboard** (Add/Edit/Delete; names auto-derive from the
     host). Replaces the old `config.ini news_feed1..3`.
   - **E — DONE (sim):** the **web-emulator HotSync** now populates News. The sim
     has no network, so "Sync Now" rebuilds the News store from the enabled feeds
     with sample items — the whole loop (add/enable a feed → HotSync → swipe the
     reader) is demoable in the browser and smoke-gated.

   **The RSS reader is now feature-complete in code** (parser + store + reader app +
   fetch phase + feed manager), sim/host-verified except the live network GET.

**Also open (infrastructure, needs a decision):**
- **S5 — real sync in the sim `[sim]`.** A `fetch()`-based DAV transport behind
  the same `dav.h` seam so the HotSync flow (and **M2** below) can be exercised in
  the browser. Larger effort; gated on how credentials are handled in the browser
  (today they are deliberately never persisted). Relevant to (4)'s HotSync fetch.

## Blocked — needs a prerequisite

- **`[blocked]` C7 ✓-glyph in To Do.** Show a real checkmark instead of `[x]`.
  The Palm bitmap font has no checkmark in codepoints 32–255, so this needs a
  deliberate font regeneration (keeping the GPLv3 PumpkinOS provenance).
- **`[blocked]` M2 — tear down LVGL draw buffers during sync.** Frees real heap
  for TLS on-device. Needs the live sync path, which is stubbed in the sim — so
  it's effectively **`[device]`** until **S5** lands.

---

## Parked — offered, NOT approved (do not build without a yes)

- **Opt-in CORS-proxy RSS fetch in the web emulator**, so News feeds could sync
  in-browser. Feed servers send no `Access-Control-Allow-Origin`, so an in-page
  `fetch()` is blocked; the only routes are a public proxy (fragile, third-party) or a
  self-hosted one (infra). Would be off by default with a configurable proxy URL.
  **The user has not said yes.** On device the fetch is direct and needs no proxy, so
  this only ever buys emulator parity. Public feeds are low-risk (only the URL is
  exposed); credentialed iCloud sync in-browser is the harder **S5** item above.
- **BLE + a companion iOS app: dropped, not parked** — see `PRODUCT_PLAN.md` §2 for
  the reasoning, kept so it isn't relitigated.
- **Coach's optional-hardware backlog** (`COACH_DESIGN.md` §9). Specified but **not
  approved**, and nothing in Coach depends on any of it:
  - **The 8:00 knock** — an RTC alarm interrupt wakes the ESP32 from deep sleep and
    the device asks "Career. 25 minutes. Ready?" Flips it from a passive object into a
    habit trigger, which is the single biggest lever on *essential*. **An RTC is
    committed to the production BOM** (owner, 2026-08-17), so the ask here is just
    routing its alarm pin to an RTC-capable GPIO — a trace, not a part, and far cheaper
    to decide before a respin. Unknown: deep sleep is untouched here; `esp_pm` light
    sleep is disabled because it gates APB and glitches this display, and deep sleep
    *should* be fine since the panel re-inits on wake, but that is an assumption.
  - **Piezo tick** — one GPIO and a second LEDC channel (the driver is already in
    `REQUIRES`). Buys a real end-of-session alarm (closing the disclosed no-speaker
    gap) and, better, a **once-per-minute tick while a session runs**: a ticking object
    on a desk has presence, and in a shared room it signals "I'm in a session" without
    saying so. Ties into the already-open "is the buzzer pad populated?" decision in
    `PRODUCT_PLAN.md`.
  - **Co-working over ESP-NOW** — two or more devices share one session, each screen
    showing the countdown plus a row of sigils; give up and yours hollows out for
    everyone. `esp_now.h` ships inside the already-linked `esp_wifi`, so it sidesteps
    the whole BLE-controller problem (none of the 56 KB reserved DRAM that put bitchat
    24,064 B over the link). **Unmeasured**: needs Wi-Fi resident during a session
    (heap + battery), and coexistence with a HotSync is unexamined. Measure before
    committing — same discipline as the bitchat answer.

## Needs hardware — features

- **`[device]` C3 — Sound.** PalmOS clicked on taps, chirped on HotSync
  start/finish, and alarmed on appointments. Needs the CYD's audio out
  (DAC/I2S + speaker). Highest perceived-charm-per-byte item on the list; also
  unlocks Date Book alarms actually *alarming* (VALARM already syncs).
- **`[device]` U8 — Power.** Battery gauge (GPIO34 ADC → battery % by the clock);
  confirm light-sleep + PWM backlight behave on a real cell.
  **Confirmed against the vendor docs (2026-08-19):** this board *does* carry a charge
  path — a **TP4054** charge-management IC on the 2-pin `JP2` battery seat, with a
  P-channel FET for discharge switching (user manual Fig. 3.13). `BAT_ADC` is wired to
  `IO34`. So the battery charges over USB with no extra part, and `power_battery_pct()`
  is implementable rather than blocked — it returns `-1` today purely because the
  divider was never calibrated, which is a bench measurement, not a code problem.
- **`[device]` U9 — Case.** Printed enclosure.

## Needs hardware — on-device verifies (written, awaiting flash)

- **`[device]` Wake + lock-screen notes — ROUND 2 SHIPPED (2026-08-19), NEEDS A LOOK.**
  Lock raised at sleep instead of at wake, Coach exempt through the reflect flow, a
  slower/stronger/longer end-of-session flash, and stale weather hidden after 24 h.
  See the `2026-08-19` entry in `BUILD_PROGRESS.md`. What the sim cannot judge:
  - **Is 1.4 s x 10 phases at 100% the right flash?** Tune `CO_FLASH_MS` / `CO_FLASH_N`
    in `ui.c`. It ends early on the first touch, so the ceiling only costs something
    if nobody is in the room.
  - **Does the wake now land clean?** The dashboard is built behind a dark panel and
    the backlight is deferred one render pass. If any of the previous screen still
    flickers through, the deferral needs a second pass, not a longer one.
  - **The reflect screen now holds the display indefinitely.** By request: a session
    that ends while the device is face-down must be answered, so no lock covers it.
    That means an unanswered "how did it go" keeps the device out of glance mode until
    someone taps. If that turns out to be annoying rather than insistent, the fix is a
    timeout that banks the session as unrated, not a lock screen over the top of it.
  - **24 h is a guess.** Long enough that an overnight gap doesn't blank the screen,
    short enough that yesterday's forecast never shows. `WX_STALE_MIN` in `dash.h`.
    The agenda is deliberately NOT gated — those are the user's own records and stay
    true offline; only the readings of the outside world are hidden.
- **`[device]` Coach on glass — NOTES ROUND 1 DONE (2026-08-18), ROUND 2 OPEN.**
  Six notes came back from the first real use and are all shipped (icon, back links
  out of Marks/This-week, non-dismissing Length/Day-goal rows, on-screen explanation
  of what the app is, Family + Relationships domains, sigil scaled to fit its
  control). See the `2026-08-18` entry in `BUILD_PROGRESS.md`. Still open, and still
  the things the simulator structurally cannot judge:
  - **Does the 15% session dim read as intentional, or as a fault?** On the sim it is
    just a number; on the real panel a dim backlight can look like a failure. Tune
    `CO_DIM_PCT` if it reads wrong. **Not raised in round 1 — may be fine.**
  - **Hollow (gave-up) marks on the wall are faint.** Now that the wall renders whole
    marks at 30 px, the solid ones read well; the stippled ones are close to invisible
    at that size — the 50% parity stipple leaves ~1 px of ink on a 2 px stroke. A
    thicker stroke for hollow marks, or a different "gave up" treatment (a slash
    through the cell?), would fix it. Not raised in round 1, but visible in
    `coach_marks_hollow`.
  - Also worth a look: the five-second give-up hold (too long? too short?), whether
    the exported Date Book block is welcome or noise (it is opt-in, default on), and
    whether the ritual's three questions are the right three.
- **`[device]` Coach: retire the `UI_DEVTOOLS` seal escape.** Sealed mode is
  deliberately unreachable from the menu, so CI has no route to the reflect/note
  screens without waiting out a real 25-minute session; the current answer is an
  invisible `UI_DEVTOOLS`-only corner target on the seal that jumps the clock. It
  compiles out of release builds, but it is an escape hatch inside the one feature
  whose premise is that there isn't one. Better options if it bothers us: a
  build-time short session length for CI, or driving `coach.sav` directly from the
  harness.
- **`[device]` Sync self-heal.** Confirm the device's always-full-reconcile heals
  To Do (out 2 → 3, pulling the orphaned test todo) and that a 2nd sync is
  idempotent (`push=0 pull=0`). Capture the `[sync]` line.
- **`[device]` Large collection.** >24-record collection round-trips (the `MAXR`
  cap is gone via streaming; confirm on real data).
- **`[device]` iCloud href relocation.** The idempotency fix for a relocated
  object whose GET-for-UID truncates on the 8 KB no-PSRAM buffer — verify against
  a photo-heavy contact live (no delete/dup/loss).
- **`[device]` config.ini round-trip.** Flash `main`, edit the Preferences form,
  and run a live Discover → assign → Save against a real iCloud account.
- **`[sim]` A short "About this screen" help panel on EVERY screen.** Prompted by a
  real first-run experience: a new player opened Zip and had no way to learn the rules
  from the device — they connected all the numbers and could not tell why it still said
  "4 left" (the cover-every-cell rule). Nothing on screen explains any app.
  **Shape:** extend the existing **Menu ▸ About** item so it is present on every screen
  and shows a couple of short paragraphs *for that screen*, not the global About box.
  Keep the current global About reachable (it carries the GPLv3/PumpkinOS provenance
  and the C6 honesty lines). **Pool-safe:** one scrollable label in the existing alert/
  overlay pattern — no new widget classes, no per-screen canvases. Text lives in one
  static table keyed by screen so it costs flash, not RAM. **Must cover the four games'
  rules** (Zip's cover-every-cell rule especially, plus retrace-to-rewind, Undo/Clear
  and what the clock/Best mean), the four PIM apps, Graffiti, HotSync, News and the
  lock screen. Smoke-gate one screen's panel so the overlay can't regress.
- **`[device]` Games on glass — SHIP GATE.** All four games are sim- and host-verified;
  four things only the real panel can settle. **(a) Zip's drag feel:** the
  bridge-through-a-free-neighbour rule was tuned against a mouse — confirm a fast
  diagonal fingertip sweep draws the intended path and that retracing rewinds cleanly
  (`ZP_CELL`, 24 px, is the knob if the target is too small). **(b) Zip's `New`
  latency:** generation is 2.4 ms on the host, so expect ~40–80 ms on a 240 MHz ESP32 —
  confirm the button feels instant and the WDT stays quiet (lower the `count_paths`
  budget in `zp_new` if a seed ever stalls). **(c) The play clocks across a real power
  cycle:** the save holds a *paused* snapshot, so a game resumed after a battery pull
  must show the banked time, not the wall-clock gap; best times must survive.
  **(d) Re-measure heap/BSS** after the shared game canvas (expect ~12.5 KB more free).
- **`[device]` Should a game's clock pause when the backlight times out?** Today a game
  left open on the desk keeps counting — `ui.c` still considers the screen open. Pausing
  would need a hook from `idle_step()` (`lvgl_port.c`) into `games_pause_clocks()`.
  Deliberately not built blind: decide it on glass, where the real timeout is visible.
- **`[device]` The device-side weather fetch is NOT BUILT.** The lock screen reads
  `/sdcard/weather.dat`, but nothing on the device has ever written it — the only
  writer is `dash_weather_seed_sample()`, which fabricates a plausible snapshot so the
  dashboard renders before a real fetch exists. So every temperature the device has
  ever shown is synthetic, and the 24-hour staleness gate added on 2026-08-19 can only
  ever fire on the seeded sample's age. `PRODUCT_PLAN.md` §"device-later" specifies the
  fetch (Open-Meteo, hourly temp + precipitation probability + weathercode + daily sun
  times, written compact to `WX_PATH` during HotSync); it belongs in the internet-only
  stage of `hotsync_task()` beside `fetch_news()`, which now runs regardless of the
  account. **Open first:** where the location comes from — a single lat/lon in
  `config.ini`, or an on-device picker (`PRODUCT_PLAN.md` lists this as undecided).
- **`[device]` A real RTC part — decide and fit.** The clock problem in one line: this
  board has no battery-backed RTC, so the wall clock is only as good as the last
  checkpoint. `clock.c` persists the epoch to NVS every 120 s and restores it at boot,
  so after a power cycle the clock resumes reading *the moment power was cut* — behind
  by the whole outage. Nothing re-anchors it automatically either: `app_main.c:256-257`
  does `wifi_connect()` → `clock_sync()`, but that code is **unreachable**, because
  `lvgl_port_run()` above it is a bare `while(1)`. SNTP therefore only ever runs on a
  user-initiated HotSync.
  **Why a sleep button alone does not fix it:** ESP32 deep/light sleep does keep time,
  but off RTC_SLOW_CLK, and the WROOM-32 has no 32.768 kHz crystal fitted — our own pin
  map proves it (the 32K pins are GPIO32/33, both used by touch). That leaves the
  internal ~150 kHz RC oscillator: calibrated at boot, but temperature- and
  supply-dependent, drifting on the order of a percent — minutes/day, not seconds.
  **MEASURE BEFORE BUYING — instrumented 2026-08-19.** Every HotSync is a free
  reading of how far the clock wandered since the last one, and `clock_sync_begin/end`
  now takes it: the correction SNTP applies, the interval it accumulated over, and the
  implied ppm, logged and appended to `/sdcard/drift.log` (the experiment runs on
  battery with no USB attached, so a serial-only reading would never be read). Samples
  whose interval contains a power loss are reported but not counted — that correction
  is the outage, not drift. **The estimates below span seconds/day to minutes/day
  depending on how much time goes to light sleep on the uncalibrated RC; if the real
  number lands at the low end, a battery alone closes this item and no RTC part is
  needed.** Two syncs are required before the first real sample (the first sets the
  anchor). Gate: `make -C sim clock`.
  **Size the part to the sync interval, not to the spec sheet.** Assume Wi-Fi once a day
  (user is home daily). At one anchor/day a plain crystal RTC at ±20 ppm drifts
  **~1.7 s/day** — already invisible. The DS3231's ±2 ppm TCXO buys ~1 min/*year*, which
  only earns its price if the device can go weeks without Wi-Fi. So a **PCF8563 /
  DS1307-class part is sufficient and cheapest**; DS3231 is the upgrade if that daily
  assumption weakens.
  **Fit — settled against the vendor docs (2026-08-19), and the earlier guess was wrong.**
  The board is the **E32R28T** (LCDWIKI, ESP32-WROOM-32E + ILI9341 + XPT2046); its IO
  table in `5_Schematic/` matches our pin map exactly, so it is authoritative. The old
  note here claimed `22` and `27` were the free pair broken out on the side headers.
  **`IO22` is the red RGB LED and is not brought out at all.** The three 4-pin 1.25 mm
  seats are, per Figures 3.12 and 3.13 of the user manual:

  | seat | pin 1 | pin 2 | pin 3 | pin 4 |
  |---|---|---|---|---|
  | **P2** Serial | +5V | GND | TXD0 (IO1) | RXD0 (IO3) |
  | **P3** SPI Peripheral | MOSI (IO23) | MISO (IO19) | CLK (IO18) | **CS (IO27)** |
  | **P4** Expand Pin | VCC3V3 | **IO35** | *(n/c)* | GND |

  **The pin census forces the answer.** Reachable without soldering: `IO27` (the only
  free *bidirectional* GPIO), `IO35` (**input only**), `IO1`/`IO3` (UART0), and the SD
  bus. I2C needs two bidirectional lines and there is exactly one, so an I2C part must
  take a UART0 pin — and that is worse than inconvenient: **`RXD0` is driven by the
  CH340 whenever USB is connected**, and `TXD0` carries the ROM bootloader's output
  before our code runs. Either choice costs the console and both are needed back to
  flash. So **I2C is off the table on this board without soldering.**

  **Therefore: an SPI RTC on P3, with `IO27` as its chip select.** That is what the
  seat is for — the manual's words: *"Lead out an unused chip selection pin and SPI
  interface pin used by the MicroSD card, which can be used for external SPI devices."*
  Candidate part: **DS3234** (the SPI sibling of the DS3231). Two consequences:
  - **P3 carries no power.** 3.3V and GND come from **P4**, so the cable is a
    two-connector splice — crimp only, nothing soldered to the board.
  - **OPEN RISK, de-risk before buying: the RTC shares SPI2 with the SD card.**
    Multiple devices per host is supported by the SPI master driver (add the RTC with
    `spi_bus_add_device` on `SD_SPI_HOST` after `esp_vfs_fat_sdspi_mount` has brought
    the bus up; per-device clocks handle the DS3234's ~4 MHz ceiling against the SD's
    20 MHz). The failure mode is the module, not the driver: **a breakout whose MISO
    does not go high-Z when CS is high will corrupt every SD read.** Buy a 3.3V-native
    board with no buffer or level shifter on MISO, and prove SD + RTC coexist on the
    bench before the enclosure design assumes it.

  **The alarm pin has a home, so the 8:00 knock survives.** `IO35` is useless for a bus
  but it is `RTC_GPIO5` and a valid `ext0` deep-sleep wake source — exactly what an
  alarm input is — and it sits on P4 beside the 3.3V and GND the module needs.
  **Gotcha:** `IO34`–`IO39` have no internal pull-ups and INT/SQW is open-drain, so that
  line needs an external pull-up in the cable. This is also the reason to pay for a
  DS3231-class part over the PCF8563 the drift maths alone would pick: the alarm is
  worth more here than the ±2 ppm is.
  **Gotcha:** ZS-042-style DS3231 modules trickle-charge their cell — fit a LIR2032, or
  remove the charging resistor before putting a non-rechargeable CR2032 in one.
  **Why this leads rather than the software fallback below.** The alternative is to
  bring Wi-Fi up for SNTP, and Wi-Fi is the one resource this board cannot spare — see
  the corrected Mode A/B table in `BUILD_PROGRESS.md`, where on-device Mode B is now
  estimated at **~20 KB** free (the old ~70 KB assumed an LVGL teardown that was never
  implemented, and the 78 KB that appeared to confirm it was measured headless). A ~$1
  I2C part converts a RAM-and-timing question into a two-wire read at boot and takes
  the clock off the network permanently. That is worth well more than the part price on
  a board with no RAM to spare.
- **`[device]` FALLBACK (only if the RTC is rejected): make boot-time SNTP reachable.**
  Strictly second choice — the RTC above removes the need for this entirely. If it is
  built anyway, the constraints are not negotiable:
  **(a) Boot ordering, not a background task.** Do NOT bring Wi-Fi up during interactive
  use: `lvgl_port.c:63-66` already documents the priority-4 sync task starving the LVGL
  wake-poll, so a mid-session connect would stutter the UI even if the RAM fit. The
  place for this is `app_main`, *above* `lvgl_port_init()`, where LVGL does not exist
  yet — no draw buffer, no view tree, no 24 KB pool. The dead code at
  `app_main.c:256-257` had the right idea and the wrong address: it sits below
  `lvgl_port_run()`'s `while(1)` and can never execute.
  **(b) Allocate the draw buffer FIRST.** `lvgl_port.c:88-90` takes a single *contiguous*
  ~19 KB `MALLOC_CAP_DMA` block, and its failure path just `return`s — a fragmented
  DMA heap gives you a black screen and one log line. Wi-Fi up/down before that malloc
  can fragment exactly that region. Grab the buffer while the heap is pristine, then do
  Wi-Fi, then hand the saved pointer to `lv_display_set_buffers`.
  **(c) Hard timeout.** 3–5 s cap on connect+SNTP, then skip. Booting away from home
  must not hang on a scan.
  **(d) SNTP is not TLS.** It is one UDP datagram: it needs Wi-Fi+lwIP (~50 KB) but
  neither mbedTLS (~40 KB) nor the sync working set (~55 KB). Do not let this grow into
  a second sync path.
- **`[device]` Measure Mode B headroom with the UI resident.** The number in
  `BUILD_PROGRESS.md` is arithmetic, not a measurement, and it is the estimate the whole
  Mode A/B rule rests on. **The probe is already in the firmware** — `hs_heap()` in
  `hotsync.c` logs `free / min_ever / largest8 / largestDMA` at six points:
  `wifi-up`, `pre-tls`, `post-tls`, `sync-done`, `wifi-down`, plus `disc wifi-up` /
  `disc post-tls` on the discovery path. Nothing to write on the next flash; just run a
  HotSync and read the log.
  **How to read it.** The handshake peak is the drop in `min_ever` from `pre-tls` to
  `post-tls`. `min_ever` is since *boot*, so if it does not move, the handshake simply
  never beat an earlier boot transient and the run is inconclusive — reboot and sync
  immediately for a clean bracket. The `free` column at `post-tls` is sampled after the
  handshake buffers are released, so it overstates headroom.
  **What to do with the answer.** If the peak really is ~20 KB, implement the
  draw-buffer teardown `hotsync.c:8` has promised since the beginning — that is ~19 KB
  in reserve. Separately, compare `largestDMA` at `wifi-up` against `wifi-down`: if a
  Wi-Fi cycle fragments the DMA region, that alone kills the boot-SNTP fallback above,
  since LVGL needs ~19 KB *contiguous* DMA and its failure path just returns.
  Write the measured figures back into the `BUILD_PROGRESS.md` table and drop the
  "(est.)" markers.
- **`[device]` Make the hardware button sleep, not power off.** Today the firmware reads
  **no** button at all and has no software power-off path — the only way off is cutting
  the rail. Worth building for UX (instant wake, no boot wait, game and screen state
  preserved), but it is **not** the clock fix — see the RTC item above.
  **Which button decides whether this is firmware or soldering:** RESET/EN is wired to
  the chip's reset pin and can *never* be intercepted; BOOT/GPIO0 is RTC-capable and
  works both as the sleep trigger and as an `ext0` deep-sleep wake source; a slide
  switch in the battery/USB line is a hard cut, and converting it means rewiring it to
  a GPIO so the SoC keeps power.
  **UX consequence:** from deep sleep, tap-to-wake is gone — touch is read via pressure
  z1 over SPI (`IRQ36 unused`) and SPI is dead in sleep. Either wake on the button only,
  or rewire touch IRQ as the wake source. Today's awake-but-dark model keeps tap-to-wake
  for free.
  **Measure before committing:** this board's real sleep current. The ESP32 die draws
  microamps in deep sleep, but the CYD's LDO and USB-serial chip dominate — that number
  decides whether an all-day sleep on a LiPo is viable at all.
  Note `CONFIG_PM_ENABLE` / tickless idle are **commented out** in `sdkconfig.defaults`
  (light sleep gated APB and made the display flash), so "sleep" today means a
  full-speed SoC with the backlight off: accurate clock, thirsty.
- **`[device]` Graffiti tuning.** The letter + punctuation stroke templates are
  coarse starters; tune thresholds from on-device `graf`/`graf pnc` telemetry on
  this exact resistive panel.
- **`[device]` UX on glass.** Sync-awake screen, brightness stepper, and the To Do
  due-date picker against a real HotSync; plus on-glass verification of everything
  built in the sim this cycle (C1 ink, C2 HotSync dialog, C4 forms, I1.2 keyboard,
  brightness stepper, inverted title bar, toasts, Week view).
- **`[device]` heap re-measure.** Re-measure interactive heap headroom after the
  M1 static→heap move.

## Cleanup / housekeeping

- **`[device]` Gate firmware telemetry.** The host `[sync]` lines are behind
  `SYNC_DEBUG`; the `dav_esp.c` `[dav]` firmware lines aren't yet — do it on the
  next flash (needs an ESP-IDF compile to confirm no unused-variable warnings).
- **`[device]` iCloud data hygiene.** One-time: remove seed contacts / duplicate
  events left in the real account from the broken-sync era.

## Someday / nice-to-have

*(The Graffiti trainer and RSS reader graduated to the prioritized roadmap at the
top.) The Graffiti case model is settled: one stroke set (26 capital-style
letters), lowercase output, upstroke = shift-next (two = caps lock).*

- Preferences app icon in the launcher; dark mode. *(The button remap graduated to
  "Needs hardware" above — it turns on which physical button it actually is.)*

---

## Recently done (for context — details in `BUILD_PROGRESS.md`)

Sync is bidirectional + durable (UID identity, streaming reconcile, always-full
device reconcile). On-device `config.ini` + Preferences + Discover. The PalmOS UI
(views, edit forms, menus, categories, Graffiti, HotSync, Calculator, Find).
Legal/CI/README hygiene. The **browser simulator** (real `ui.c` to WASM, live on
GitHub Pages, native headless smoke gate in CI). And this review cycle's charm/
intuitiveness batch: C1 ink trail, C2 HotSync dialog, the full C4 form contract
(bottom bar, Edit Categories, Address 10 fields, event Alarm/Repeat), C5/C6
About honesty, C7 inverted title bar, I1.1 onboarding hint, I1.2 keyboard,
I2 remove-demo-data safety, I3 Week view, I4 feedback toasts (record
save/delete *and* config-field save), and the brightness-stepper freeze fix.

**Coach (2026-08-17)** — the ritual focus timer, and the first app built from a
written design spec (`COACH_DESIGN.md`) rather than straight into `ui.c`. Sigil
capture, sealed mode, the six-rule advice engine (68 host assertions), the marks
wall, the weekly report, and Memo + Date Book export. 226 B of static DRAM, zero new
canvas buffers. Merged in #39 and flashed; **the on-glass notes pass is still open**
(below).

This cycle also landed two of the "new apps / input experience" items: the
**Graffiti accuracy harness + template fixes** (letters 97.5%→99.6%, now a CI
gate) and the **Graffiti SRS trainer** (Drill/Train, per-device user templates),
plus the full **RSS reader** — streaming parser, on-SD store, swipe reader app,
and the HotSync fetch phase (all merged; only the on-glass live-fetch verify
remains).

**The Games app is finished (2026-07-24).** Four games, each with pure host-gated
logic, a pausable play clock, a persisted best time and SD-backed state: **Mines**,
**Wordie**, **Sudoku**, and **Zip** — a 6x6 one-line path puzzle (start on 1, hit the
numbers in order, cover every cell). Zip's generator inverts the naive approach:
numbering every cell of a random Hamiltonian path is trivially unique, so it *removes*
numbers while exactly one solution survives (the `sudoku.c` hole-digging pattern),
yielding minimal boards of 5–12 numbers in ~2.4 ms. Input is a forgiving drag
(retrace-to-rewind, bridge a skipped cell) rather than taps. Two platform wins landed
with it: the play clocks now **pause when a game is off screen** (`playclock.h`, gated by
`make -C sim clock` — they used to count while closed), and all four games **share one
I1 canvas buffer**, returning **12.5 KB of BSS** on a no-PSRAM board. New gates:
`make -C sim zip`, `make -C sim clock`, `make -C sim games`.

A follow-up polishing pass then: reshaped Graffiti **`G`, `S`, `X` and `?`** from
on-glass feedback (`G` a wide-open C + full-width mid-bar so it no longer reads as
`O`; `S` a proportional two-lobe; `X` one continuous stroke; `?` with a downward
tail so it stops reading as `)`) — gate green throughout; rebuilt the trainer on a
**deterministic 5-level SRS with a burn state**, extended to **digits +
punctuation**, with a **Menu > Reset progress**; and turned the RSS reader's
sources into a managed **feed list** (`bridge/feeds.c` + `feeds_test`) — 10
pre-seeded world feeds, a **Preferences checkbox manager** with a keyboard URL
editor, and a sim **HotSync** that fills News from the enabled feeds so the whole
loop demos in the browser.

Real feeds *in the browser emulator* remain gated on CORS: feed servers don't send
`Access-Control-Allow-Origin`, so an in-page `fetch()` is blocked and would need an
opt-in CORS proxy (public = fragile/third-party; self-hosted = infra). Public feeds
are low-risk (only the URL is exposed); credentialed iCloud sync in-browser stays
the harder S5 item. On device the fetch is direct, no proxy.
