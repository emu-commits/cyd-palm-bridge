# Session handoff — context for the next session

Living scratch doc: overwrite it each session. Captures where things stand so the
next session can pick up without re-deriving. Authoritative detail lives in
`docs/BACKLOG.md` (roadmap + changelog) and `docs/KANA_TRAINER.md` (the Japanese
trainer's full Tier 1–5 analysis).

_Last updated: 2026-07-19 (product pivot)._

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
  Pillow). Gates: `make -C sim smoke`, `make -C sim graf`, `make test`, `make ftest`;
  firmware + wasm build in CI. `emcc` is not installed locally — rely on the CI wasm
  job. The sim smoke test navigates by pixel coordinates (launcher grid geometry
  matters: adding an app reflows row 3).
- **Device-only verifies** (real resistive-panel accuracy, live RSS fetch, sync
  self-heal) cannot be started off-bench.
