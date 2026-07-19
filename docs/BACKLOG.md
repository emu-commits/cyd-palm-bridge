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

> History lives in `BUILD_PROGRESS.md` (milestone changelog + hard-won lessons).
> The original design analyses are `UI_ROADMAP.md` (memory/hardware),
> `ROADMAP.md` (sync/port, done), and `SIMULATOR_PLAN.md` (the emulator).
> `REVIEW_2026-07-15.md` is the whole-repo review these item IDs come from.

---

## Next up when we resume (priority order)

The sim-testable charm/intuitiveness backlog is done (see "Recently done"). The
next arc is about the **input experience** and **new apps**. Each of 2–4 starts
with a **feasibility check on the base CYD** before committing to a build.

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

3. **Japanese kanji trainer — extend the training app `[sim]` + dataset work.**
   Reuse the SRS engine + stroke scoring from (2) for kanji. **Stroke-order data:
   KanjiVG** (github.com/KanjiVG/kanjivg, CC BY-SA) — per-character SVG stroke
   paths *with order*. **Learning sequence:** a WaniKani-style ordering (community
   repos expose the level/sequence lists). The real work is a **build-time
   pipeline**: parse KanjiVG SVG paths → resample into our recognizer's point
   format (or a compact polyline), pack into a device-friendly binary indexed by
   codepoint, and pick a subset (e.g. WaniKani levels) that fits 4 MB flash / SD.
   *Feasibility to settle:* processed dataset size; per-kanji stroke count vs. the
   recognizer; and **CJK rendering** — the bitmap font is Latin-only, so kanji
   display likely needs a small embedded CJK subset font *or* drawing the KanjiVG
   outlines directly. Note KanjiVG's CC BY-SA attribution/share-alike terms.

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

## Needs hardware — features

- **`[device]` C3 — Sound.** PalmOS clicked on taps, chirped on HotSync
  start/finish, and alarmed on appointments. Needs the CYD's audio out
  (DAC/I2S + speaker). Highest perceived-charm-per-byte item on the list; also
  unlocks Date Book alarms actually *alarming* (VALARM already syncs).
- **`[device]` U8 — Power.** Battery gauge (GPIO34 ADC → battery % by the clock);
  confirm light-sleep + PWM backlight behave on a real cell.
- **`[device]` U9 — Case.** Printed enclosure.

## Needs hardware — on-device verifies (written, awaiting flash)

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

- Preferences app icon in the launcher; power/reset-button remap; dark mode.

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

This cycle also landed two of the "new apps / input experience" items: the
**Graffiti accuracy harness + template fixes** (letters 97.5%→99.6%, now a CI
gate) and the **Graffiti SRS trainer** (Drill/Train, per-device user templates),
plus the full **RSS reader** — streaming parser, on-SD store, swipe reader app,
and the HotSync fetch phase (all merged; only the on-glass live-fetch verify
remains).

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
