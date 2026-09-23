# Work backlog — what is left

The one list of what's left to do. **Only open work lives here.** When something
is done it moves to `BUILD_PROGRESS.md` (the changelog, the hardware/RAM
reference and the hard-won lessons) and leaves this file. `PRODUCT_PLAN.md` is
the path to shipping; `COACH_DESIGN.md` and `GURU_HABITS.md` are per-app specs.
Retired analyses are in git history: `git log --diff-filter=D -- docs/`.

Every item is tagged by **where it can be worked**, because on this project that
is the constraint that binds (a base CYD with no PSRAM, developed mostly away
from the bench):

- **`[s]`** — buildable and verifiable *today* in the emulator (`make -C sim
  smoke`, or https://emu-commits.github.io/cyd-palm-bridge/). No hardware needed.
- **`[d]`** — needs the physical CYD (touch, Wi-Fi/TLS, sound, battery) or a live
  iCloud account. It can usually be *written* off-device and checked on return.

**Working agreement.** Tick a box only when it is *verified*, not when it is
written: `[s]` means the smoke ran and the screenshot was looked at, `[d]` means
someone looked at the glass. **The user runs the bench checks — never tick a
`[d]` box for them.** One group per commit, on a branch off `main`.

---

## RESUME HERE — 2026-09-23

**§R is code complete** (below): fifteen refinements requested on 2026-09-23,
built and sim-verified on branch `feat/r-phase-polish` — **not yet merged, not
yet flashed**. R15's review is in §Proposals, and nothing in it is approved.

**Before that, everything was on `main` (`97874fc`)**: the W phase (Settings and
its nine wizards) and the Q phase (quick entry) are both code-complete, the web
emulator is deployed from `main`, and the bench device was flashed from
`958a979`. **What is left of W and Q is on-glass checks only**, and they are all
in §Bench now, in one list instead of four.

**Before pushing anything: every gate, not just the smoke.** `make -C sim` has
twelve gates plus `smoke32`; the wasm build is a different toolchain; ESP-IDF
compiles with `-Werror=format-truncation`; `nosecrets` links a subset of the
sources. Each of those has broken CI on its own at least once while the others
were green. See `BUILD_PROGRESS.md` §Hard-won lessons.

**Next after §R, in order of value:** the bench pass (§Bench — the device holds
all of Q and none of it has been looked at), then §Engine 1 (external merge sort)
and §Engine 2 (shrink the sync working set), then §Next 1 ("About this screen").

---

## §R — PHASE: refinements (requested 2026-09-23, CODE COMPLETE — `[s]` all)

**All fifteen are built and sim-verified** on `feat/r-phase-polish`; the
on-glass checks are in §Bench ▸ *Unseen: the R phase*. What was learned is in
`BUILD_PROGRESS.md`. The items below are kept as they were asked, until the
phase is merged and seen on glass. Each is small on its own; the one that was a
real design job is **R4**, the week screens. Design rules from the W phase still apply
everywhere: **tap to pick, don't type; scroll-to-read is fine, scroll-to-select
is not; Home is the way out; pool-safe widgets only** (no `lv_bar`, `lv_slider`,
`lv_arc`, `lv_meter` — they allocate draw layers and live-lock the 31 KB pool).

**The strip is a place to stand** — W3's finding, and R3/R4 extend it to Coach
and Guru. The Graffiti strip is 240×112 px that a non-typing screen has no use
for; `speaker_aside()` puts a speaker there on `lv_layer_top()` at `PDA_H`, over
a screen that stays finished and live underneath. It costs the four silkscreen
buttons until tapped away.

### Coach and Guru
- [x] **R1 — Guru's blank header row gets a job `[s]`.** The row under
      "x of y today" is empty because the list is sized as `lv_pct(100) - 20`,
      which LVGL reads as *80 %*, not *100 % minus 20 px*. Use it for a one-line
      "what to do here" fragment on the left and a **Week** button on the right;
      the button may span both header rows, with the streak ("day x") moved left
      of it. The list's height becomes explicit pixels.
- [x] **R2 — Guru's menu says "This week", not "Her week" `[s]`.** The report's
      title bar too, so the menu entry and the screen it opens agree — and it
      matches Coach's entry for the same screen.
- [x] **R3 — the Coach and Guru greetings stand in the strip `[s]`.** Today the
      hello *replaces* the landing page (it is drawn over the week screen) and
      the tap that dismisses it rebuilds the app. Change: the landing page is
      built and live, and the speaker stands in the Graffiti strip the way the
      Assistant does — **but with the portrait on the right**, as it is today,
      not on the left like hers. Nothing is blocked any more, so there is no
      reason to show the week screen at that moment.
- [x] **R4 — the week screens: speaker in the strip, and a real design `[s]`.**
      The verdict moves into the strip (as R3), which gives the stats the whole
      content area and should end the scrolling. Then redesign the stats, which
      today are a monospace column of `#` bars. The brief: **clear at a glance,
      intuitive, and sticky** — the screen should make you want to beat it.
      Direction: a seven-day chart (one column per day, today marked, the target
      drawn as a line) on the shared I1 canvas; the day-by-day "chain" of
      active days, because an unbroken run is the most motivating thing a habit
      screen can show; *this week vs your best* for the headline numbers; and
      the per-category / per-domain split as proportional bars. Pool-safe:
      one canvas (buffer from outside the pool) plus labels.

### System
- [x] **R5 — "Lock" in the Applications screen's Options menu `[s]`.** Launcher
      only: locking from inside an app would throw away whatever the content
      area holds (the lock calls `content_clear()`), including a half-edited
      record.
- [x] **R6 — the lock screen's "swipe up to unlock" is the thing you see `[s]`.**
      Bold, with an up arrow, and visually distinct enough to catch a new user.
      The font has no arrow glyph, so the arrow is drawn on the dash canvas.
- [x] **R11 — the lock goes over the Calculator, and gives it back `[s]`.** The
      calculator lives on `lv_layer_top()`, above the lock (which is on the
      screen), so the lock came up *underneath* it. Hide it while locked, show
      it again on unlock, expression intact.

### PIM apps
- [x] **R7 — To Do: pick the priority `[s]`.** Four tap targets, 1–4, below the
      Due date on the edit form. One `lv_buttonmatrix` (one object). A record
      that arrives from a sync with priority 5 shows nothing selected and keeps
      its 5 unless you pick.
- [x] **R8 — Address: a phone keypad for Phone and Zip `[s]`.** Tapping either
      field opens a modal 3×4 keypad, read across: `1 2 3 / 4 5 6 / 7 8 9 /
      * 0+ #`. The `0+` key types `0`, and `+` when held (as on a phone).
- [x] **R9 — Graffiti auto-capitalises by field `[s]`.** Address Last, First,
      Title, Company, Address, City, State: **Each Word**. First letter of the
      field only: Address Note; Date Book Description and Note; To Do quick-add,
      Description and Note; Memo quick-add and the memo text. A shift the user
      armed still wins; Look Up and Find are not touched (they are filters).

### Accessories and Graffiti
- [x] **R10 — Calculator: grey digit keys `[s]`.** Grey separates the numbers
      from the operators and functions, the way a real calculator does.
- [x] **R12 — Graffiti: forgiving space and backspace swipes `[s]`.** Today a
      swipe must be 24 px wide and 2.5× wider than tall. Loosen both, and add a
      straightness test in their place so that loosening does not start eating
      letters — gated by `make -C sim graf`, which must not lose accuracy.
- [x] **R13 — Graffiti: a way out of punctuation mode `[s]`.** A stray tap on the
      pane arms punctuation, and the only way out today is to write something.
      Three exits, none of which types anything: tap the `PUNC` marker, swipe
      backspace, or wait (it lapses after a few seconds).
- [x] **R14 — News rotates between sources `[s]`.** The store is written feed by
      feed, so the reader shows all of one source before the next. Interleave
      the index round-robin by feed when a fetch commits — records are
      self-contained, so reordering the index moves no body text. Host-gated in
      `news_test`.

### The repo
- [x] **R15 — a review of the whole project** for user experience, robustness,
      and repo/code cleanliness as an open-source project. The findings go in
      §Proposals below, **as proposals** — nothing there is approved.
- [x] **R0 — this file cleaned up.** DONE 2026-09-23. The finished P, W and Q
      phases are in `BUILD_PROGRESS.md`; every open `[d]` check is in §Bench.

---

## §Bench — needs the device `[d]`

**Re-flash before reasoning about what is on the device.** The bench is only as
current as the last flash (`958a979`, 2026-09-22, which holds W and Q but not R).

### Unseen: the R phase (flash `feat/r-phase-polish` first)
- [ ] **R12 — the swipes on real glass.** The harness says 99.5 % of hurried
      swipes now register (was 76.8 %) with no letter lost; a thumb on a
      resistive panel is the real test. Also: does a sideways flick ever eat a
      letter you meant?
- [ ] **R4 — the week charts.** Legible at 18 px bars? Does the dashed target
      line read as a line? Coach's day goal (6) dwarfs a normal day's bars —
      honest, or discouraging?
- [ ] **R3/R4 — the speakers in the strip**, portraits right. Same question as
      the Assistant's pane below, now for three speakers.
- [ ] **R8 — the keypad under a thumb**: 44 px keys, and whether the 0+ hold
      (LVGL's 400 ms long press) feels like a phone's.
- [ ] **R10 — the grey calculator keys**, which join the greys question below.
- [ ] **R6 — the unlock band**: does it catch the eye without shouting?
- [ ] **R13 — the PUNC chip** is a small target at the top of the strip, and
      4 s may be too short or too long.

### Unseen: the Q phase (in rough order of how likely the emulator lied)
- [ ] **Q5's greys, and P9's with them.** The zone bars and the list hairlines
      are `COL_RULE` `0xC8C8C8`, the dimmed text is `COL_DIM` `0x8C8C8C`.
      Grey-on-grey behind a resistive panel is what an emulator judges worst.
      If the bar disappears, it wants `COL_DIM`; if it competes with the data,
      the old black was not the problem. §R10's grey keys join this question.
- [ ] **Q4's stroke sheet** at 40 px cells: legible? Does the scroll feel right
      under a thumb (it is scroll-to-read, so a drag landing as a tap must do
      nothing)?
- [ ] **Q7/Q8 quick-add with real Graffiti**: is the bar worth using over the
      full form? **Q6**: is the shortened Look Up box still wide enough?
- [ ] **12/24-hour** consistency across the title bar, weather strip, Rise/Set.
- [ ] **The Assistant's strip pane** covers the silkscreen buttons until tapped
      (W3) — tolerable? Does it read as her standing in front of the writing
      area, or as the screen growing taller? §R3/§R4 put Coach and Guru there
      too, so this answer now covers three speakers.

### Unseen: the speakers and lists (P phase)
- [ ] **P2** launcher, **P3/P4/P5** Guru shell, habits and week, **P7** lock
      restyle — each simply "look at it on glass".
- [ ] **P8 — HotSync cancel.** Device-only: the sim's sync finishes before any
      button can be pressed.
- [ ] **P9 — the 13 px drawn checkbox** as a finger target (the greys are above).
- [ ] **P10 — the portraits at 1:1** (thin ink outlines are exactly what a
      resistive panel's diffusion softens), and **tap-anywhere with a thumb**:
      scrolling a long week must not dump you home. *(§R4 may retire the
      scroll, which retires the risk.)*
- [ ] **Kana Tier 2** — tune `KW_THRESH` on the real panel.

### Unseen: the W phase
- [ ] **W5 Wi-Fi**: a real join, the scan against real APs, and whether 8 s is
      enough for networks 2–4.
- [ ] **W6 Accounts**: a real iCloud login and discovery.
- [ ] **W8**: the clock write (`settimeofday` is refused in a container).
- [ ] **W10**: a real accountless sync reports clock and news honestly.
- [ ] **Location**: wipe `latitude`/`longitude`, sync, check the `geoip:` line
      names the right town; a carrier NAT or captive portal must leave the
      location unchanged; a PINNED location must survive two syncs.

### Round 2 notes, still open
- **Coach.** Does the 15 % session dim read as intentional (`CO_DIM_PCT`)?
  Hollow (gave-up) marks are faint — ~1 px of ink at 30 px; thicker stroke or a
  slash? The five-second give-up hold, the exported Date Book block, the
  ritual's three questions.
- **Wake + lock.** Is 1.4 s × 10 phases the right flash (`CO_FLASH_MS`,
  `CO_FLASH_N`)? Does the wake land clean? Does the indefinite reflect hold read
  as insistent or annoying? `WX_STALE_MIN` (24 h) is a guess.
- **Games ship gate.** Zip's drag feel under a fingertip (`ZP_CELL`); `New`
  latency and WDT quiet; play clocks across a real power cycle; re-measure
  heap/BSS after the shared game canvas. **Open decision:** should a game's
  clock pause when the backlight times out? (A hook from `idle_step()` into
  `games_pause_clocks()`; decide it on glass.)
- **A due date picked on glass survives a real HotSync** round-trip to iCloud.
- **Coach's `UI_DEVTOOLS` seal escape** should be replaced by a build-time
  short session or a harness-written `coach.sav`.

### Live network
- **RSS**: a feed fetches, items appear, heap holds. **Weather**: `weather.dat`
  written and real numbers on the lock screen. **Sync self-heal**: To Do heals
  and a second sync is `push=0 pull=0`. **Large collection** (>24 records).
  **iCloud href relocation** against a photo-heavy contact. **`config.ini`
  round-trip** through a live Discover → assign → Save.

### Power and hardware
- **U8 battery.** Does the gauge track a real discharge through the flat middle?
  Is the divider on tolerance (`BAT_TRIM_PERMILLE` at unity; one multimeter
  reading at `JP2`)?
- **U8b screen-on power.** Idle is solved (>24 h, <46 mA). Ranked, unmeasured:
  (1) backlight level — a runtime setting, try first; (2) DFS *without* light
  sleep (`esp_pm_configure`, `min_freq_mhz = 80`) — never tried, and it does not
  gate APB the way the reverted light-sleep did; (3) CPU 160 → 80 MHz fixed —
  A/B it with `power.log`; (4) panel `SLPIN` when the backlight blanks. If the
  numbers are ambiguous, an inline meter beats a longer log.
- **Hardware button → sleep.** The firmware reads no button today. BOOT/GPIO0 is
  RTC-capable and can be both the trigger and the `ext0` wake; RESET/EN cannot
  be intercepted. Deep sleep kills tap-to-wake (touch is SPI). Measure this
  board's real sleep current first.
- **C3 — Sound.** No speaker/DAC wired. Gates real alarms (VALARM already syncs)
  and game feedback; highest perceived-charm-per-byte item on the list.
- **U9 — Case.** Out of software scope; gating for launch.

---

## §Engine — the sync engine

1. **External merge sort.** The ceiling is a few hundred records per collection:
   `SV_RAW` (~60–80 B/record) must be sorted in one contiguous block, and ~30 KB
   is the largest free block during the sort. A too-big collection is refused
   safely today (`-6`, gated by `tests/toobig.c`). Sort fixed-size runs, spill to
   SD, k-way merge back; every reconcile input is already a line-per-record
   file, so nothing else changes. **This turns "refuses safely" into "handles a
   real account."**
2. **Shrink the working set 23.5 KB → ~7 KB.** Stream `g_objbuf` (8192) to SD
   the way the news phase already does, and emit `g_body` (8192) straight to
   `BODY_TMP`. `g_lrec` (4096) is `PALM_REC_MAX` — keep. `g_state` (3076) is two
   sync tokens, probably shrinkable. Then make the budget in `hotsync.c` honest
   (~29.7 KB including the TLS handshake) and leave it on.
3. **The `heap[wifi-up]` creep:** ~13 KB lower after a session of app use than
   after a fresh boot. Not chased. It is also the measurement that settles the
   TLS handshake peak with the UI resident (`min_ever` between `pre-tls` and
   `post-tls`, on a freshly booted device). The next thread if feed fetches fail
   intermittently.
4. **iCloud data hygiene** — one-time removal of the seed contacts and duplicate
   events left in the real account from the broken-sync era.

---

## §Next — features, not yet scheduled

1. **"About this screen" on every screen `[s]`.** Someone finished a Zip board
   and could not learn from the device why it still said "4 left". Extend
   Menu ▸ About to show a couple of paragraphs *for the current screen*, keeping
   the global About (GPLv3/PumpkinOS provenance). One scrollable label in the
   overlay pattern, text in one static table keyed by screen (flash, not RAM).
   Cover the four games, the four PIM apps, Graffiti, HotSync, News and the lock
   screen. Smoke-gate one screen's panel.
2. **Graffiti writing *feel*.** Accuracy is done and gated (99.7 % letters at
   3 px jitter; `make -C sim graf`). Left: ink-trail / char-echo polish and
   threshold tuning from real on-device telemetry. Train mode already records a
   per-device template for a hand the built-ins misread.
3. **The Assistant's other jobs** (parked until asked): a sync-failure explainer
   (the highest value — sync errors are raw status text today), a first-boot
   welcome, an SD missing/corrupt explainer. **Open:** should the launcher's
   demo-data hint appear at all now that sync works without iCloud?
4. **S5 — real sync in the simulator.** A `fetch()` DAV transport behind the
   `dav.h` seam. Needs a decision on credentials in a browser first (today they
   are deliberately never persisted); buys emulator parity and little else.

---

## §Proposals — from the R15 review, NOT approved

A review of the whole project on 2026-09-23 for user experience, robustness,
and fitness as an open-source repo. **Nothing here is approved.** Ranked within
each heading by value for effort.

### Robustness
1. **Crash-safe writes for every durable file. THE ONE TO DO FIRST.** Every
   record save rewrites the whole PDB in place (`pdb_write_ai` opens the live
   path with `"wb"`), and so do `config.ini`, `feeds`, the weather cache and
   every `.sav`. A power cut or watchdog reset mid-write truncates the file.
   For Date Book, To Do and Address the mass-delete guard now pulls the
   server's copy back — **but Memo is device-only, so a truncated MemoDB is
   simply gone.** Fix: write `<path>.tmp`, `fflush` + `fsync`, `remove`, then
   `rename` (FatFs will not rename over an existing file); on boot, a missing
   `<path>` beside a `.tmp` is promoted. Host-gate it with a fault-injection
   test that stops between the two steps.
2. **Keep deleted records as tombstones until they have synced.** `data_delete()`
   drops a record outright, which is why the engine cannot tell a user's delete
   from a bad card read (§Engine, and the mass-delete guard's stated trade).
   Palm's own `REC_ATTR_DELETE` bit is the fix, and it also makes an Undo
   possible (UX 4).
3. **Don't push the demo records.** The README tells new users that the first
   sync pushes the seed records into their real iCloud and asks them to delete
   them there by hand. The demo manifest already knows every seeded uid; skip
   them on push.
4. **A release build profile.** `UI_DEVTOOLS` is compiled into *every* firmware
   build (`firmware/main/CMakeLists.txt`: "REMOVE this line for a release
   build") — including Coach's invisible seal escape. Make it a Kconfig option,
   default off, and have CI build both.
5. **Retire compile-time `secrets.h`.** Settings can now enter Wi-Fi and the
   account on the device, so the header's only remaining effect is to make it
   possible to build — and share — a binary with credentials inside it.
6. **Say when the pool runs out.** On the device `LV_ASSERT_MALLOC` ends in
   `while(1)` and a watchdog reset. Log the pool's low-water mark per screen on
   the UART (the simulator already measures it) so a field report can name the
   screen.
7. **`config.ini` holds the Wi-Fi and app-specific passwords in plain text** on
   a removable card. At minimum say so plainly (SECURITY.md); better, keep the
   secrets in NVS and leave the card holding only non-secret settings.

### User experience
1. **Flash from the browser.** "There's no prebuilt binary yet — you flash it
   yourself" is the biggest barrier to the intended audience. CI already builds
   the firmware: publish the `.bin` per release and put an ESP Web Tools
   *Install* button on the Pages site beside the emulator.
2. **A first-run path.** A fresh device boots to the lock screen and then a
   launcher full of demo data; nothing points at Settings ▸ Wi-Fi and Accounts.
   The Assistant could greet the first boot and say where to start (§Next 3).
3. **"About this screen"** (§Next 1) and **a sync-failure explainer** (§Next 3)
   remain the two highest-value features not yet built.
4. **Undo a delete.** A "Deleted — Undo" toast for a few seconds; needs
   tombstones (Robustness 2).
5. **Menus that fit.** Coach's Options menu runs to the bottom edge of the
   screen. Move the rarely used rows (Reset counters, Remove demo data) under
   one "More..." or into Settings.
6. **The public emulator shows developer items** ("Add test events" — the
   Pages build defines `UI_SEED_TESTEVENTS`). Build the showcase without them.

### Open-source readiness
1. **README.** Lead with what it is, a photo or GIF, and the "try it in your
   browser" link; then flashing; then setup through **Settings** (it still
   says *Preferences → Discover collections*). Move the architecture and build
   log (half the file) to `docs/ARCHITECTURE.md`. Fix the stale facts: "Status
   (2026-07-09)", a 24 KB pool (it is 32 KB, `31100` bytes on hardware), and
   "~$15" in one place and "$12" in another.
2. **Split `ui.c`** (10.5k lines) into modules behind a private header —
   lock/dashboard, PIM lists and forms, Settings, Coach, Guru, games, Graffiti,
   overlays. It is the single largest barrier to a contributor, and it makes
   every simulator rebuild recompile everything.
3. **Comments that outsiders can read.** Many comments carry history ("used
   to", "was tried and rejected") and internal ticket codes (R4, W3, Q5, P10)
   that mean nothing outside this backlog. Keep the *why* in the code, move
   the *story* to `BUILD_PROGRESS.md`, and name things instead of numbering them.
4. **One `make check`.** The rule "every gate, not just the smoke" lives in
   this file and in a maintainer's head. One target that runs the host gates,
   the twelve sim gates, `smoke32` and (when available) the wasm and IDF builds
   makes it a command. Ship the simulator's toolchain as a Dockerfile or
   devcontainer — today the working recipe is not in the repo.
5. **Contributor scaffolding.** `CONTRIBUTING.md` (the gates, the design rules,
   the pool budget), `SECURITY.md` (the plaintext credentials; how to report),
   issue templates, and tagged releases with a changelog.
6. **Build output out of the root.** The root `Makefile` writes ~20 binaries
   into the repo root, each needing its own `.gitignore` line (one was committed
   by accident in September). Build into `build/`.
7. **Static analysis in CI.** `-Wextra`, `cppcheck`, and ASan/UBSan over the
   host gates (today only the RSS parser has an ASan run).

---

## Parked — offered, NOT approved (do not build without a yes)

- **Opt-in CORS-proxy RSS fetch in the web emulator.** Feed servers send no
  `Access-Control-Allow-Origin`; the routes are a public proxy (fragile) or a
  self-hosted one (infra). Emulator parity only.
- **Coach's optional hardware** (`COACH_DESIGN.md` §9): the 8:00 knock (an RTC
  alarm pin to an RTC-capable GPIO), a piezo tick (one GPIO, a second LEDC
  channel), co-working over ESP-NOW (needs Wi-Fi resident during a session —
  measure heap and battery first).
- **BLE + a companion iOS app: dropped, not parked** — see `PRODUCT_PLAN.md` §2.

## Decided — kept so the reasoning is not relitigated

- **No RTC part for timekeeping.** Drift on battery is under a minute a day and
  idle clears 24 h (measured 2026-08-27). An RTC survives only as the enabler for
  the parked 8:00 knock. If one is ever fitted it shares SPI2 with the SD card,
  and a breakout whose MISO does not go high-Z will corrupt every SD read.
- **The LVGL pool is 32 KB** (`31100 bytes total` on hardware), and `make -C sim
  poolparity` fails if the sim and the device drift. Older prose saying 24 KB is
  stale; the gate is the authority. `smoke32` is 32-*bit*, not 32 KB.
- **The Japanese trainer is frozen at Tier 2** (product decision 2026-07-19).
- **P6 (a one-shot Assistant onboarding) is superseded by the W phase.** A sync
  with no credentials does what it can and says so; it never pushes iCloud.

## Someday

Dark mode.
