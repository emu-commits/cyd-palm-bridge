# Session handoff — context for the next session

Living scratch doc: overwrite it each session. Captures where things stand so the
next session can pick up without re-deriving. Authoritative detail lives in
`docs/BACKLOG.md` (roadmap + changelog) and `docs/KANA_TRAINER.md` (the Japanese
trainer's full Tier 1–5 analysis).

_Last updated: 2026-07-21 (Sudoku; Mines timer/high-score; Wordie font/legend/streak; lock-screen seven-seg clock + world-clock settings; game-state persistence)._

## This batch (2026-07-21)

Four areas, one PR:

- **Mines** -- renamed the title to "Mines" (matches the icon). Added a live play
  timer (starts on first dig, freezes at win/lose, 1 Hz tick) and a persistent
  best-time high score, shown "Time m:ss  Best m:ss" below the board. Both ride in
  `mines.sav` (magic MSG2).
- **Wordie** -- new cleaner 5x6 letter font (10x12 px, centred with a margin so it
  no longer touches the tile bottoms). New mono state language + an on-screen
  **legend** under the grid: CORRECT = filled tile (SPOT), PRESENT = corner tab
  (WORD), ABSENT = slash (NONE). Added a persistent **streak** counter (`wordie.sav`
  magic WDG2).
- **Sudoku** (NEW, third game) -- `firmware/main/sudoku.c/.h`, pure C + host gate
  (`make -C sim sudoku`): a seeded generator that guarantees a UNIQUE solution
  (fill a random solution, dig holes with a solution-counting check), rules, and
  solved detection. View in `ui.c` (`show_sudoku`): 9x9 board + number pad on ONE
  I1 canvas; clue cells carry a corner tab and reject edits, the selected cell gets
  a thick border, conflicts are slashed. **On-brand input: the Graffiti digit strip
  fills the selected cell** (number pad does the same by tap). Persists to
  `sudoku.sav`. `icon_sudoku` in `palm_icons.c`; in `GAMES[]`/`GAME_ICONS[]`.
- **Lock screen** -- replaced the blocky hero clock with a **seven-segment** renderer
  (bevelled, mitred bars). Moved the two world clocks to their own row **below** the
  clock so a two-digit hour's AM/PM can't overlap them (the reported bug). New
  **Preferences -> "Lock screen..."** sub-screen picks each world-clock zone (shared
  zone picker, with "(off)") and toggles **12h/24h**. New Config fields
  `world1`/`world2`/`clock24` round-trip in `config.ini` (covered by the config host
  gate). NOTE: grouping the three dashboard settings behind ONE sub-screen row (not
  three inline Preferences rows) was necessary -- three extra `lv_list` buttons
  pushed the Preferences list past the 24 KB pool and the brightness popup then
  crashed on the 32-bit build. `smoke32` peak 612 B.

## Game state persistence (2026-07-20)

Both games now remember their in-progress state across leaving the app. `MsGame`/
`WdGame` are plain PODs, so each is saved as a magic-tagged blob of the whole struct
(`/sdcard/mines.sav`, `/sdcard/wordie.sav`) after every move/keystroke and on New,
and restored when the screen reopens (`ms_save/ms_load`, `wd_save/wd_load` in
`ui.c`). A stale/foreign file is rejected by the magic + a light sanity check
(board dims for Mines; uppercase answer + row count for Wordie). Reopening a game
lands exactly where you left it (or on the finished result); New starts fresh.
Smoke gate: `mines_persist`/`wordie_persist` screenshots must byte-match the
pre-exit `mines_dug`/`wordie_scored`. This does NOT yet cover win/streak *stats*.

## Wordie (2026-07-20)

A five-letter, six-guess word game -- the second game in the Games folder. Renamed
for trademark (NYT owns "Wordle").
- **`firmware/main/wordie.c/.h`** -- pure C, host-testable: a 923-word answer bank,
  deterministic daily word (`wd_daily(g, day)`, day = `time()/86400`, wraps mod bank)
  + a seeded "New" puzzle, letter entry, and Wordle two-pass scoring (correct/present/
  absent with proper duplicate handling). Host gate: `make -C sim wordie`
  (`sim/tests/wordie_test.c`), wired as a CI step.
- **`firmware/main/ui.c`** (search `show_wordie`) -- the guess grid AND an on-screen
  QWERTY keyboard are drawn mono on ONE 1-bpp canvas (`wd_buf`, static BSS -- not the
  pool), so it's pool-cheap (smoke32 peak ~600 B). A hand-rolled 5x7 uppercase font
  (`WD_FONT`) draws letters. Taps hit-test into keys via `LV_EVENT_PRESSED`; the
  physical Graffiti strip ALSO types (backspace=del, newline=submit).
- **Mono state language** (grid tiles and keys share it): CORRECT = solid black tile,
  knockout letter; PRESENT = double border; ABSENT = letter with a diagonal slash.
- `icon_wordie` (a 3x3 guess grid) in `palm_icons.c`. Registered in `GAMES[]`/
  `GAME_ICONS[]` next to Mines.
- The in-progress puzzle persists across exits (see "Game state persistence"), and
  a consecutive-solve **streak** counter is shown + persisted (2026-07-21 batch).
- NOT YET: a guess-validity dictionary (any 5 letters is accepted as a guess) --
  a reasonable follow-up.

## Latest cycle (2026-07-20)

Three user-reported fixes on the games/launcher structure:
- **Minesweeper taps did nothing on the device.** The board canvas used `LV_EVENT_CLICKED`,
  which the resistive panel suppresses when a tap jitters (reads as a scroll). Switched to
  `LV_EVENT_PRESSED` + cleared `LV_OBJ_FLAG_SCROLLABLE` on the canvas -- the same
  resistive-robust pattern the News reader uses. (Worked in the sim either way; device-only bug.)
- **Games is now an icon sub-folder**, not a text-button list: `show_games()` renders an icon
  grid mirroring the app launcher. Each game is a tappable icon + label (`GAMES[]`/`GAME_ICONS[]`
  -- extend both to add a game). Minesweeper got its own `icon_mines` (spiked mine, `palm_icons.c`).
- **Kana folded into Graffiti.** Removed the top-level Kana launcher app; the Graffiti trainer
  now has a compact "あ" button (top row, left of Drill/Train) that opens the kana trainer, and
  the kana screen has an "ABC" button back to the Latin drill. Graffiti is the handwriting hub.
  Launcher is back to 8 apps (row 3 = News, Games).

## Direction change (read first)

The project pivoted from "extend the Japanese trainer" to **making a sellable
consumer device** (Etsy). The full strategy + decisions are in
**`docs/PRODUCT_PLAN.md`**. Locked decisions: **BLE/companion-app dropped** (Wi-Fi
only), **strictly mono Palm** aesthetic, **lock-screen dashboard first**. The
**Japanese trainer is frozen at Tier 2** (it teaches recognition, not pen-to-paper
writing) — it stays as a launcher app, not a headline. Roadmap now: dashboard →
alarms/battery → games (Minesweeper, a Word clone, Sudoku, Zip — all renamed) →
web-flasher/OTA/first-run polish.

## Where things stand

- **Default branch `main`** auto-deploys the **web emulator** to
  https://emu-commits.github.io/cyd-palm-bridge/ on every merge (the "Deploy to
  Pages" CI job; it fails on feature branches by design — ignore that red mark).
- **Working branch:** `claude/project-review-recommendations-x1ihdn`. Workflow:
  after each PR merges, restart it from `origin/main` (keep the name), one PR per
  cycle. Merge method **merge**. **Wait for the firmware ESP-IDF gate to go green
  before merging** (a user-stated preference).
- **GitHub Actions status API lags/caches** minutes behind reality and its
  `list_workflow_runs` output is huge — parse the saved tool-result file, and
  cross-check job status via `list_workflow_jobs` on the specific run id.

## Shipped this session (all merged, or merging)

1. **Graffiti G/? fixes** (PR #23, merged): capital-G separated from O; `?` given a
   downward tail.
2. **Japanese trainer Tier 1 — kana → sound** (PR #24, merged): new **Kana**
   launcher app (8th icon, あ). Shows a kana; you answer its Hepburn romaji by
   drawing Latin letters in the Graffiti strip (Latin recognizer untouched).
3. **Japanese trainer Tier 2 — write the kana** (PR #25, this cycle): a Sound/Write
   toggle. Write shows the numbered KanjiVG stroke model; you redraw it stroke by
   stroke with **enforced stroke order**. Correct strokes **lock in solid**; a
   **wrong stroke restarts from stroke 1** (and is a miss). Per-stroke matching is
   a *separate* `$1` module — the Latin recognizer is untouched.

## The Japanese trainer — key files

- `firmware/main/ui.c` — the Kana app: two-challenge SRS (Sound + Write), the
  Sound/Write toggle, the numbered-model I1 canvas, the per-stroke capture loop.
  Search `Kana Trainer (roadmap #3`.
- `firmware/main/lv_font_kana.c/.h` — kana bitmap subset font (IPAGothic via
  `lv_font_conv`); the system font is Latin-only.
- `firmware/main/kana_data.c/.h` — ordered gojūon table (kana ↔ romaji).
- `firmware/main/kana_strokes.c/.h` — 92 kana stroke polylines **in official order**
  from **KanjiVG** (CC BY-SA). Regenerate with `tools/gen_kana_strokes.py`
  (needs `pip install svgpathtools` + network; caches SVGs in /tmp/kvg).
- `firmware/main/kana_write.c/.h` — self-contained `$1` per-stroke matcher.
  `KW_THRESH` (currently 26) is the accept distance.
- `firmware/main/graffiti.c` — added one read-only accessor `graffiti_raw_stroke()`;
  recognition logic otherwise untouched.
- SRS state persists to `/sdcard/kana_train.dat` (magic `KT02`, both challenges).
- `icon_kana` in `firmware/main/palm_icons.c`. `NOTICE` credits IPAGothic + KanjiVG.

## Lock-screen dashboard — BUILT this cycle (emulator-verified)

The hero feature of the product pivot. Full-screen mono glance view, shown on boot
and re-raised on every wake; **swipe up to unlock** into the launcher. Tiles: big
clock (drawn on a 1-bpp canvas via a 4×7 pixel font — no large font needed), two
world times (DST-aware), cached weather (temp / air / a 6-hour temp+rain strip),
battery, next event + next due, sunrise/sunset, moon phase (drawn). Pool-safe (one
I1 canvas + labels; sim heap peak 600 B). Key files:

- `firmware/main/dash.c/.h` — pure C (host-testable): the SD weather-cache blob
  (`WxCache`, `WX_PATH`) + load/seed, moon-phase math, sunrise/sunset math.
- `firmware/main/ui.c` — the Lock-screen section (search `ui_show_lock`): the canvas
  graphics (`dash_paint` draws EVERYTHING — clock, separators, rain bars, moon — since
  `fill_bg` wipes the canvas each refresh), the labels, the swipe handler, the
  next-event/next-due queries.
- `firmware/main/clock.c` — `clock_zone_hhmm()` world-time helper (saves/restores TZ).
- `firmware/main/power.c` + sim stub — `power_battery_pct()` (device returns -1 until
  the ADC is calibrated → the tile shows "USB"; the sim returns a sample 72%).
- `firmware/main/lvgl_port.c` — re-raises the lock on device wake (`idle_step`).
- Weather currently renders from a **sample** blob seeded by `dash_weather_seed_sample`
  (no network in the sim / before a fetch). The real Open-Meteo fetch is device-later.

## Next steps / open gates (in priority order)

0. **DEVICE-LATER for the dashboard (before the product is "done"):** the live weather
   fetch (Open-Meteo HTTPS during HotSync, *after* DAV frees its TLS buffers, stream-
   parsed into `WX_PATH`) and the battery ADC on GPIO34 (calibrate, then return a real
   `power_battery_pct()`). Neither is sim-verifiable. Then: **alarms that fire** and the
   **games**.

1. **Tier 2 on-device tuning — THE GATE before any kanji work.** The emulator proves
   the *mechanics* only; whether the per-stroke matcher *feels* right on the physical
   resistive panel is unproven. On real hardware, tune `KW_THRESH` (and consider
   per-stroke leniency for tiny/short strokes). Do **not** start Tier 3 before this.
2. **Quick UX follow-up if the user asks** (not yet approved): Sound mode uses strict
   Hepburn (`shi/tsu/chi/fu/wo`). If drawing `si/tu/ti/hu/o` should count, add
   accepted alternates in `ka_input` (`ui.c`).
3. **Tiers 3–5 (kanji)** — CONDITIONAL on #1. The KanjiVG→polyline pipeline is now
   proven on kana and extends directly. Needs: WaniKani ordering + vocab dataset;
   CJK **rendering** (draw kanji from stroke data on the canvas, or a subset font);
   the tightest 2.8″ layout is Tier 5. Full plan in `docs/KANA_TRAINER.md`.

## Parked (offered, NOT approved — do not build without a yes)

- **Opt-in CORS-proxy RSS fetch** in the web emulator so News feeds can sync
  in-browser (feed servers/iCloud send no `Access-Control-Allow-Origin`). Off by
  default, configurable proxy URL. User has not said yes.

## Standing platform constraints (don't relearn the hard way)

- **No PSRAM.** ~35 KB free heap with Wi-Fi+TLS+LVGL; **LVGL pool is a hard 24 KB.**
  Bulk data lives on SD, loaded on demand.
- **Pool-safe widgets only:** labels, lists, tables, buttonmatrix, **I1 canvas**.
  `lv_bar/slider/arc/meter` alloc draw layers → WDT freeze. Verify sim heap peak
  stays ~0 in the smoke run.
- **Verify in the sim** via `make -C sim smoke` + screenshots (convert PPM→PNG with
  Pillow). Gates: `make -C sim smoke`, `make -C sim graf`, `make -C sim mines`,
  `make test`, `make ftest`; firmware + wasm build in CI. `emcc` is not installed
  locally. The sim smoke test navigates by pixel coordinates (launcher grid geometry
  matters: adding an app reflows row 3).
- **THE 24 KB POOL TRAP (learned the hard way):** the 64-bit native sim uses a **48 KB**
  LVGL pool (LP64 objects are ~2× bigger), but the **wasm build + the device use the
  true 24 KB**. So a screen with too many widgets can pass the native smoke yet **fail
  to boot on wasm/device** (pool exhaustion → NULL alloc → crash/freeze), and CI only
  *compiles* wasm. **Guard:** `make -C sim smoke32` builds the smoke **32-bit** (real
  24 KB pool) and runs it — now a CI step (needs `gcc-multilib libc6-dev-i386`). Repro
  a suspected pool issue locally with `sudo apt-get install -y gcc-multilib
  libc6-dev-i386` then `make -C sim smoke32`. The lock screen keeps its footprint down
  by **not coexisting with other screens**: it clears the content area and defers the
  launcher, so the pool only ever holds the chrome + one screen.
- **Device-only verifies** (real resistive-panel accuracy, live RSS fetch, sync
  self-heal) cannot be started off-bench.
