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
   confusions. Used it to separate the worst collisions — letters went **97.5% →
   99.6%** mean at 3 px jitter (h→k 72→92, g→o and p→d fixed, no glyph below 92%);
   digits 100%. **Still to do:** the writing *feel* (ink-trail / char-echo UX),
   the punctuation set (harness covers letters + digits only), the `X` 2-stroke
   exception, and final threshold tuning against real on-device `graf` telemetry
   (the synthetic model is a proxy). This is the foundation for the trainer below.

2. **Graffiti training app — a spaced-repetition (SRS) trainer `[sim]`.** The
   long-standing idea (a Palm-style launcher app in the mould of a writing-drill):
   show a target stroke, the user traces it, score against the template, advance;
   an **SRS schedule** resurfaces the glyphs you're worst at, and a **training
   mode** captures the user's *own* strokes as per-device templates (calibrating
   to this exact resistive panel). *Feasibility to settle first:* the SRS queue +
   per-glyph stats are tiny (SD/NVS), so the open questions are (a) template
   storage + scoring cost inside the 24 KB LVGL pool, and (b) whether stroke
   capture is smooth enough on the bit-banged touch.

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

4. **RSS reader — a TikTok-swipe, text-only feed `[sim]`.** A full-screen,
   one-item-per-view reader you swipe vertically through (headline + body text, no
   images), with articles fetched during **HotSync** (over the existing Wi-Fi/TLS
   path in the sync window) and stored on the **SD card** as a local DB the UI
   reads offline — the same offline-first model as the PIM apps. *Feasibility to
   settle:* a feed-fetch + RSS/HTML-to-text step inside the sync task's RAM
   budget; how much text to cache per feed on SD; and the swipe-paging UX in LVGL
   (keep it pool-safe — plain labels, no layer-compositing widgets).

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
