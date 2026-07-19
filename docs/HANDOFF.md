# Session handoff — context for the next session

Living scratch doc: overwrite it each session. Captures where things stand so the
next session can pick up without re-deriving. Authoritative detail lives in
`docs/BACKLOG.md` (roadmap + changelog) and `docs/KANA_TRAINER.md` (the Japanese
trainer's full Tier 1–5 analysis).

_Last updated: 2026-07-19._

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

## Next steps / open gates (in priority order)

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
