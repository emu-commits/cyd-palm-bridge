# Japanese trainer — feasibility analysis + Tier 1 build notes

A Decuma-style Japanese learning app that reuses the Graffiti trainer's
deterministic spaced-repetition engine. The user asked for a five-tier plan,
evaluated against the device's real constraints *before* building, with Tier 1
built in full. This document is that evaluation plus a record of what Tier 1
shipped.

## Platform constraints that govern the design

- **Flash:** 4 MB, a single 3 MB app partition. Generous for code + fonts.
- **RAM:** no PSRAM. ~35 KB free heap with Wi-Fi + TLS + LVGL resident; the LVGL
  draw pool is a hard **24 KB**. This is the real ceiling — anything bulky lives
  on **SD**, loaded on demand (the same offline-first model as the PIM/RSS apps).
- **Widgets:** pool-safe only (labels, lists, tables, buttonmatrix, I1 canvas).
  `lv_bar/slider/arc/meter` allocate draw layers → 24 KB pool exhaustion → WDT
  freeze. The trainer and this app use labels + (Tier 2+) one I1 canvas only.
- **System font:** Latin-only bitmap (glyphs 32–255). It cannot render a single
  Japanese character — kana/kanji display is the crux, addressed per tier below.
- **Recognizer (`graffiti.c`):** a `$1` unistroke matcher — **single-stroke,
  position-normalized, direction-sensitive**. It has no concept of stroke count
  or order. Left completely untouched by this app.

## The architectural insight (makes Tiers 2/3/5 feasible)

We do **not** need a multi-stroke recognizer, and we must **not** touch the Latin
Graffiti path. Because the app *shows the numbered model and enforces official
stroke order*, a multi-stroke character decomposes into *N ordered single-stroke
problems*: the user draws stroke 1 → we match it against the **one expected
stroke #1** (shape *and* position/scale within the character box) → accept/advance
or reject; then stroke 2, etc. This:

- **reuses the existing `$1` engine** almost verbatim (the only change is keeping
  position/scale instead of normalizing it away) in a **separate module**, so the
  Latin recognizer is unaffected — **zero risk to English**;
- **enforces stroke order for free** (we advance stroke by stroke);
- means **no in/out "kana mode" button** on the system keyboard is needed — the
  practice lives inside the app on its own canvas. (A kana mode on the *general*
  keyboard would be a real change and a real risk — explicitly out of scope.)

This is the Decuma "trace-the-model" interaction, and it is the RAM-frugal path.

## Tier-by-tier verdicts

| Tier | Challenge | Verdict | Gating risk |
|------|-----------|---------|-------------|
| 1 | Kana → **sound** (draw romaji) | **GO — DONE** | only new work was *showing* the kana |
| 2 | **Write the kana** (multi-stroke, ordered) | GO — low risk to English | on-device stroke accuracy (unprovable in sim) |
| 3 | Kanji **kun'yomi** + English gloss | CONDITIONAL — gated on Tier 2 passing on hardware | dataset pipeline + CJK rendering |
| 4 | **Vocab** → draw kana | GO if 2/3 land | reuses Tier 2; no new recognizer |
| 5 | **Write the kanji** (stroke order, all readings) | Feasible to build; highest accuracy + layout risk | 15–20 stroke glyphs on resistive touch; 2.8″ layout crunch |

### Tier 1 — sound (BUILT)
Fully feasible, almost all reuse. The *answer* (`ku` = draw `k` then `u`) uses the
existing Latin recognizer and font — no new glyphs, no recognizer change. The only
real work was **displaying the kana prompt**, solved with a compact bitmap subset
font (below). The SRS is the trainer's deterministic engine, keyed per kana.

### Tier 2 — write the kana
GO via the per-stroke sequenced approach above. Kana are 1–4 strokes; decomposition
handles them. The full kana reference set (~150 glyphs × ~3 strokes × ~12 points) is
**~30 KB on SD**; runtime holds only the current character. Layout: numbered model
on the top canvas, draw zone below — pool-safe. **Caveat:** whether per-stroke
recognition *feels good* on the resistive panel is only provable **on-device** —
this is the Tier 2 → Tier 3 gate.

### Tier 3 — kanji (kun'yomi + gloss)
Architecture scales linearly with stroke count. Two new constraints, both solvable:
- **Dataset:** only the WaniKani subset, introduced level by level, and only the
  *resampled polylines* (not SVG). ~2000 kanji × ~10 strokes ≈ **< 1 MB on SD**,
  loaded per level. Work: a build-time pipeline (KanjiVG parse → resample → pack),
  plus its CC BY-SA attribution / share-alike terms.
- **Rendering:** the kanji *character* can be drawn from the same stroke data used
  for the writing challenge (thick polylines on the I1 canvas), avoiding a large
  CJK glyph font. Kana readings use the Tier 1 subset font.

### Tier 4 — vocab → kana
No new recognizer — reuses Tier 2. Vocab word + always-on English gloss at the top,
kana draw zone below. Feasible if 2/3 land.

### Tier 5 — write the kanji
Same engine, more strokes. Two real risks: **(a) accuracy** on 15–20-stroke glyphs
on a noisy resistive digitizer (enforced stroke order *helps* — each stroke matches
exactly one target); **(b) layout** — the tightest screen (numbered model + draw
zone + kun'yomi + on'yomi + English at once on 2.8″). Feasible to build; won't feel
dialed-in until on-device tuning.

## Rendering / font decision

Two options, and the design uses a hybrid:
- **Render from stroke data on the I1 canvas** (thick polylines) — zero font cost,
  reuses the trainer's guide renderer, and doubles as the Tier 2+ dataset. Used for
  the numbered model and (later) the kanji character itself.
- **A small embedded subset bitmap font** — only where crisp inline text mixed with
  Latin is needed. **Tier 1 uses this for the kana prompt.** A kana subset is
  ~130 KB at 38 px (see below); a WaniKani-kanji subset would be ~200–400 KB — both
  fit the 3 MB partition, grown per level.

## What Tier 1 actually built

- **`firmware/main/lv_font_kana.c` / `.h`** — a 38 px, 1-bpp LVGL bitmap font
  subset (hiragana U+3041–3096, katakana U+30A1–30FA, plus the dakuten marks and
  the prolonged-sound mark), generated from **IPAGothic** (IPA Font License) with
  `lv_font_conv`. Latin glyphs stay in `lv_font_palm`; this font carries only kana.
- **`firmware/main/kana_data.c` / `.h`** — the ordered gojūon table: all 46 basic
  hiragana, then all 46 basic katakana, each paired with its Hepburn romaji. (92
  entries; dakuten/handakuten/combos are out of scope for the Tier 1 sound drill.)
- **`firmware/main/palm_icons.c`** — `icon_kana`, a 25×22 A8 launcher icon (あ
  rasterized from IPAGothic).
- **`firmware/main/ui.c`** — the `Kana` launcher app (8th icon). It shows a kana +
  script + level, notes the romaji answer on a kana's **first introduction and on
  every wrong attempt**, and grades the drawn romaji **letter by letter** against
  the Hepburn reading: a correct prefix advances the echo, a full match promotes /
  burns, a divergence demotes + re-reveals + retries. Same deterministic SRS as the
  Graffiti trainer (level 1–5, burn past 5, smallest-due pick), persisted to
  `/sdcard/kana_train.dat`. **Menu → Reset progress** wipes it. Pool-safe (labels
  only; sim heap peak 0).

### Verified in the simulator (screenshot + gates)
- Launcher shows the Kana app with the あ icon; the app renders the kana with the
  bitmap font, the level, and the "sound:" note.
- Drawing a caret `^` (= `a`) answers あ → "Correct! a (Lv 2)" and deterministically
  advances to い; a wrong letter shows "was: i - try again" and re-reveals.
- Gates green: `make -C sim smoke`, `make -C sim graf` (Latin recognizer unchanged),
  `make test`, `make ftest`. Firmware ESP-IDF build + wasm build run in CI.

### What Tier 1 cannot prove off-bench
Nothing in Tier 1 depends on stroke recognition (the answer is *typed* romaji via
the already-tuned Latin recognizer), so Tier 1 is fully verifiable in the sim. The
first thing that needs a real device is **Tier 2's per-stroke kana recognition** —
that is the gate before any kanji work begins.
