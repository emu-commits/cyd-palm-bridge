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

## Do next — in the simulator

- **`[sim]` C4 extras — finish the Palm form contract.** The bottom-bar
  Done/Details/Delete pattern is in; **Edit Categories (rename/add) is now done**
  (the picker's tail row opens a pool-safe editor; `data_set_categories` writes the
  AppInfo back, preserving records). Still missing Palm staples:
  - Event **alarm** + **repeat** fields in the Date Book Details sheet (the data
    already round-trips VALARM/RRULE — this is UI only).
  - **Address** showing more than 5 fields on the edit form.
  - Category **delete** (deferred — it must recategorise the affected records to
    Unfiled, which rename/add don't touch).
- **`[sim]` I4 — config-field-save feedback.** Record save/delete now toast;
  the Preferences **Save** still uses the modal alert. Give it the same
  transient toast for consistency.
- **`[sim]` I3 "Go to Date"** *(low priority)* — a fast cross-year jump dialog.
  The Day/Week/Month zoom hierarchy + `[<]`/`[>]` paging already reach any date,
  so this is a convenience, not a gap.
- **`[sim]` S5 — real sync in the sim.** Sync is stubbed in the browser build.
  A `fetch()`-based DAV transport behind the same `dav.h` seam would let the
  HotSync flow (and **M2** below) be exercised off-device. Larger effort; gated
  on a safe way to hold credentials in the browser (today they are deliberately
  never persisted).

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

- **`[sim]` Graffiti training game.** A launcher app that shows a target stroke,
  scores the user's trace, and advances — doubling as a per-user *training* mode
  that captures the user's own stroke as the template. Case is settled: one stroke
  set (26 capital-style letters), lowercase output, upstroke = shift-next (two =
  caps lock). Includes the **X** 2-stroke exception.
- Preferences app icon in the launcher; RSS reader; power/reset-button remap;
  dark mode.

---

## Recently done (for context — details in `BUILD_PROGRESS.md`)

Sync is bidirectional + durable (UID identity, streaming reconcile, always-full
device reconcile). On-device `config.ini` + Preferences + Discover. The PalmOS UI
(views, edit forms, menus, categories, Graffiti, HotSync, Calculator, Find).
Legal/CI/README hygiene. The **browser simulator** (real `ui.c` to WASM, live on
GitHub Pages, native headless smoke gate in CI). And this review cycle's charm/
intuitiveness batch: C1 ink trail, C2 HotSync dialog, C4 form contract +
Edit Categories, C5/C6 About honesty, C7 inverted title bar, I1.1 onboarding
hint, I1.2 keyboard, I2 remove-demo-data safety, I3 Week view, I4 save/delete
toasts, and the brightness-stepper freeze fix.
