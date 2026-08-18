# Coach — design spec

_Written 2026-08-17. A ritual-based focus timer for a device that physically cannot
notify you. You draw a mark, the screen seals itself, and the mark fills with ink
while you work._

**Status: BUILT + merged (PR #39, `f487801`), flashed and boot-verified on the bench.**
Costs in §8 are measured off the built ELF, not estimated. The on-glass notes pass is
open — see `BACKLOG.md`. §9 (RTC / piezo / ESP-NOW) is specified but **not approved**;
nothing in §1–§8 depends on it.

Published design: https://claude.ai/code/artifact/e272936c-dd92-451c-922e-9a7152f0689d

## TL;DR

Every Pomodoro timer is a commodity. The clock isn't the product — the *ritual* is,
and the hardware is what makes the ritual stick. A phone timer sits next to the thing
that interrupts you. This one sits on your desk, runs your pre-flight questions, locks
itself shut, and has no radio pointed at your attention.

The lock-screen dashboard sells the device at rest. Coach is what makes someone pick
it up every morning — and the wall of marks it accumulates is what makes them do it
tomorrow.

**Design target:** one session costs **six taps, one stroke, and no typing** — under
fifteen seconds of interaction around twenty-five minutes of work. Everything below is
subordinate to that.

**Cost (measured on the built firmware):** 194 bytes of static DRAM out of the
37,328 free, +12 KB of flash, and **zero new canvas buffers**. Coach is the cheapest
app in the firmware.

---

## 1. The loop

Selectors auto-advance: one tap chooses *and* moves on. There is no Next button
anywhere in the ritual. That single decision is what turns a form into a ritual.

```
Launcher ─▶ Coach
              │
              ├─ Today      streak · today's count · [Start]
              │       │
              │       ▼   tap 1        tap 2         tap 3
              ├─ Ritual    Energy? ─▶ Domain? ─▶ Intention?
              │       │
              │       ▼   one stroke
              ├─ Sigil     draw your mark for this session
              │       │
              │       ▼   screen seals, dims to 15%, then sleeps
              ├─ Running   the mark inks in from the bottom
              │       │    give up = 5-second press-and-hold
              │       ▼   T-0, or Finish early
              │                     tap 4          (tap 4b)
              ├─ Reflect   Result? ─▶ (Blocker? if Struggled)
              │       │
              │       ▼   tap 5
              ├─ Note      [Skip] or Graffiti, <=64 chars
              │       │
              └───────┴─▶ Today   mark joins the wall · streak held
                             │
                             └─▶ Memo Pad record + Date Book block ─▶ iCloud

Menu ▸ Coach ▸  Marks · Weekly report · Session length · Day goal · Reset
```

---

## 2. The Sigil

After the three ritual taps, you draw **one Graffiti stroke**. Not text — a mark.
Whatever you want this session to mean.

That mark is rendered large on the running screen as a hollow outline, and it **inks
in from the bottom** as the session elapses. At T-0 it is solid black. That is the
progress bar.

This is the feature that cannot be cloned by an app, and it's the reason to build
Coach on *this* device rather than any other. It exists only because there's a stylus
digitizer and a unistroke recognizer already sitting in the firmware. It photographs
beautifully in mono. And it compounds: every finished mark is kept, so a year of focus
becomes a wall of your own handwriting.

**Already built:** `graffiti_raw_stroke(int16_t *out_xy, int max)` hands back the
captured polyline and is already in use by the Kana writing trainer. Coach calls it
from a `graf_capture_hook`, keeps up to **39 points**, and stores them as `int8` pairs
normalised to a 0–100 box — **80 bytes per session**.

```
Running — full-screen takeover, 240 x 320
┌──────────────────────────────────┐
│                                  │
│              23:41               │
│                                  │
│          ▁▁▁▁▁▁▁▁▁▁              │
│        ╱            ╲            │
│       │              │           │
│       │   ▓▓▓▓▓▓▓▓   │           │  <- ink line
│       │ ▓▓▓▓▓▓▓▓▓▓▓▓ │              rises with
│        ╲▓▓▓▓▓▓▓▓▓▓▓▓╱               elapsed time
│          ▓▓▓▓▓▓▓▓▓▓              │
│                                  │
│       Career · Finish · High     │
│                                  │
│                 ·                │  <- hold 5s to give up
└──────────────────────────────────┘
```

Rendering is cheap by construction: the outline is stroked once when the session
starts, and the fill advances **once a minute** — twenty-five repaints across a
twenty-five minute session. Only the `MM:SS` label ticks at 1 Hz, and it's a label,
not a canvas.

Abandoned sessions keep their mark but stay **hollow** on the wall. The wall is
honest, which is the only way it stays meaningful.

---

## 3. Sealed mode

During a session Coach owns the entire display, lock screen included. The silkscreen
Home, Menu, Find, and Calc buttons go inert. There is no launcher. There is no Sudoku.

Giving up requires a **five-second press-and-hold** on a deliberately small target,
with the countdown drawn as it fills. It logs `CO_RES_ABANDONED`, it hollows the mark,
and it breaks the streak.

This is the lockbox mechanic in software, and it's the thing people describe to each
other — *"it won't let me check anything."* It's also the honest argument for owning a
dedicated device instead of using a phone.

**Consequence:** because you cannot navigate away mid-session, Coach's sigil canvas has
**uncontested use of `game_cv_buf`** — the 4,940-byte I1 buffer the four games already
share. The feature pays for its own buffer, and Coach adds no canvas storage at all.

### The dim is part of it

You tap *High · Career · Finish*, draw your mark, and the screen fades to 15%. Nothing
else changes, but the desk goes quiet. Then the idle timer blanks it entirely and
you're alone with the work. At T-0 the backlight pulses back to full three times.

This costs nothing — `power_set_brightness()` already exists for the config screen —
and it's the moment that sells the device in a live demo.

> **Hardware limit.** There is no speaker, buzzer, or DAC on the current board.
> Session end is **visual only**: backlight pulse plus the completed mark. On a desk in
> your eyeline that works; in a bag it does not. See the piezo entry in §9, and
> disclose the limitation on the product page until it lands.

---

## 4. Where the data goes

> **Correction to the original brief: there is no SPIFFS partition on this device.**
> `partitions.csv` is `nvs` (24 KB) + `phy_init` + a 3 MB `factory` app — that's all of
> the 4 MB. Persistent app data lives on the **SD card via FatFs**, which is what every
> other app already does (`graf_train.dat`, `sudoku.sav`, `weather.dat`). Coach follows
> that convention exactly; adding a SPIFFS partition would cost app-partition space for
> no gain.

| Path | Shape | Written | Size |
|---|---|---|---|
| `/sdcard/coach.sav` | magic + one POD struct | on each phase change | ~52 B |
| `/sdcard/coach.log` | header + N x 12 B, append-only | once per finished session | 26 KB/yr |
| `/sdcard/coach.sig` | N x 80 B, append-only, index-aligned to the log | once per finished session | 60 KB/yr |

Six sessions a day is 552 bytes a day. Neither append-only file needs rotation on an SD
card, and both are **streamed on read, never fully buffered** — the same discipline the
DAV enumeration rewrite landed on. The only thing resident while reading is a ~120-byte
accumulator, plus one 80-byte mark at a time while painting the wall.

The live sigil rides in RAM during the session and is appended to `coach.sig` at the
same moment the record is appended to `coach.log`, so the two files can never drift out
of index alignment. A truncated `.sig` just renders those sessions as plain filled
squares.

**Reuse:** the pausable timer is already written. `playclock.h` is pure, clock-injected,
POD, host-tested in `sim/tests/clock_test.c`, and designed to live inside a save blob.
Coach wants the opposite default from the games, though — a Pomodoro must keep counting
when you leave the screen — so Coach simply does *not* register with
`games_pause_clocks()`. Only the Pause button calls `pc_pause()`. No new timer code.

---

## 5. Advice, as a pure function

Rules run in fixed priority order, first match wins. Deterministic, no floating point in
the decision path, no randomness — so `sim/tests/coach_test.c` can pin every branch on
the host before it ever runs on the board.

`coach_advise()` returns a code; `ui.c` owns the wording. Keeping the copy out of the
engine means the tone can be tuned without touching logic or invalidating a single test.

| | Rule | Condition |
|---|---|---|
| R0 | `CA_NONE` — under five sessions | "Three more sessions and I'll have something useful to say." Never advise on noise. This rule protects the credibility of the other five. |
| R1 | `CA_EARLIER` — shift sessions earlier | The worst time-of-day bucket struggles at >= 2x the rate of the best, and it falls later in the day. Needs >= 3 sessions in each bucket compared. |
| R2 | `CA_SHORTER` — try shorter sessions | Struggle rate above 40% *and* summed actual/planned below 0.8 — you're bailing out early, not just having bad days. |
| R3 | `CA_REDUCE` — reduce intensity | Low-energy sessions are >= 40% of the week and more than half of them struggle. You're grinding while depleted. |
| R4 | `CA_INCREASE` — increase intensity | Great rate above 70%, completion at essentially 100%, and the session length hasn't moved in two weeks. It's too easy. |
| R5 | `CA_STEADY` — same again | The fallthrough. A coach that always finds a correction is a coach you stop believing. |

The energy-to-performance correlation is R3 plus the report's `Energy High 86% / Low
31%` line — a 3x3 energy x result table folded down to the one comparison a person can
act on.

The timezone offset is **injected, not read** — same discipline as `playclock.h` and
`dash_sun_times()`. It's what keeps the morning/afternoon/evening bucketing testable on
a host in any locale.

---

## 6. Running while the screen is off

The port layer blanks the backlight after `backlight_sec` of inactivity, and the first
touch afterwards wakes the screen, is swallowed, and raises the lock screen. Sealed mode
resolves this cleanly: **during a session, Coach *is* the lock screen.** Waking
mid-session shows the mark and the countdown, which is exactly what you wanted to see
anyway. There's no tile to design and no ambiguity about what the device is doing.

One thing is still structurally required: **a global 1 Hz tick.** `coach_tick` is
registered once in `ui_init()` alongside `ms_tick`/`sd_tick`/`zp_tick` and runs
regardless of which screen is up. At T-0 it calls `power_backlight(1)`, pulses, and
raises the Reflect card — so a session ends correctly even if something unexpected is on
screen.

> **No RTC yet.** The current board has no battery-backed clock. The epoch is restored
> from an NVS checkpoint at boot and re-anchored by SNTP on each HotSync, so a power
> cycle mid-session can move time by the whole off-duration. An RTC is committed for the
> production hardware, which removes this — but Coach must not *depend* on it, because
> the units already in the field don't have one.
>
> **Handling:** on entering Coach, if `phase == RUNNING` and elapsed already exceeds
> `planned_min`, the session finished while the device was off — go straight to Reflect
> with the real elapsed. If elapsed exceeds `planned_min + 60`, the clock is not
> trustworthy: record `CO_RES_ABANDONED` silently and reset. The streak survives; the
> fake four-hour session never gets logged.

---

## 7. What leaves the device

Coach writes into two apps that already sync, so a week of focus turns into records on
your actual phone. Neither costs new engineering — the sync engine is built and tested.

**The note becomes a Memo.** Saved through `data_save_memo()` as a real Memo Pad record,
so the next HotSync pushes it to iCloud:

```
Coach · 2026-08-17 · Career · Great
Finally untangled the parser. Keep going here tomorrow.
```

**The session becomes a calendar block.** Each finished session is also written as a
Date Book record via `data_save_cal()` — a real block at the time it actually happened:

```
09:05-09:30   Focus · Career · Finish
```

This is the quieter of the two, and the more *essential*. Your calendar ends up showing
the deep work you did rather than the deep work you scheduled, and for anyone who bills
hours that's a defensible record. Both are opt-in from Menu ▸ Coach, default on.

---

## 8. What it costs

**These are measured on the built firmware, not estimated.** Baseline before Coach:
37,328 bytes free in the static DRAM window, 32 KB LVGL pool, ~155 KB free heap.

| Budget | Coach | Notes |
|---|---|---|
| Static `.bss` | **194 B** | `g_co_sig` 80 + `g_co` 44 + 70 in widget pointers and flags |
| Total DRAM delta | **336 B** | window went 37,328 -> 36,992 free |
| Canvas buffers | 0 B | sigil reuses `game_cv_buf`; sealed mode guarantees exclusivity |
| LVGL pool, 24 KB gate | **passes** | `make -C sim smoke32` runs the whole Coach tour at device pool size |
| Sim heap peak, full tour | 2,236 B | of a 147,456 B budget, with every app exercised |
| Flash | **+12,141 B** | image 1,460,739 -> 1,472,880; 53% of the app partition still free |

I estimated ~148 bytes when writing this spec; the build says 194. The gap is the
widget pointers, which I had waved at rather than counted. Recorded here as measured
rather than quietly left as the estimate.

Coach is the cheapest app in the firmware -- cheaper than Wordie, cheaper than the
dashboard, even with the sigil. That isn't luck: it's mostly text, counters, and one
1-bpp canvas that already exists. Compare the BLE mesh analysis: that failed the link
by 24,064 bytes before any application code existed. This asks for 194.

One thing to watch: the weekly report has the highest label count in the app. If it
grows past what's drawn here, it converts to an `lv_table` — the same move that fixed
the record list — rather than adding labels.

---

## 9. Optional backlog

None of the following is required for Coach to ship, and nothing in §1–§8 depends on any
of it. Each is a hardware or radio commitment that should be decided on its own merits —
kept here so the design doesn't quietly assume them.

### The 8:00 knock — needs RTC + alarm pin

A DS3231's alarm interrupt can wake the ESP32 from deep sleep. The device sits dark on
your desk, and at your chosen hour it lights up with one question — **"Career. 25
minutes. Ready?"** Two taps and you're working.

This is the largest single lever on *essential*: it flips the device from a passive
object you might remember to use into a habit trigger that initiates. If the RTC is going
into the BOM regardless, routing its alarm pin to an RTC-capable GPIO is a trace rather
than a part, and much cheaper to decide now than to respin for later.

_Unknowns:_ deep sleep is untouched territory here. `esp_pm` light sleep is disabled
because it gates APB and glitches the display; deep sleep shouldn't share that problem
since the panel is re-initialised on wake, but that is an assumption, not a measurement.

### Piezo tick — needs one GPIO + $0.30

A second LEDC channel on a driver already in `REQUIRES`. It buys the obvious thing — a
real end-of-session alarm, closing the gap disclosed in §3 — and a better one: a
**once-per-minute tick while a session runs**.

A ticking object on a desk has presence; it's the difference between a device that's
asleep and one that's working alongside you. In a shared room it's also a social signal
that you're in a session, without you having to say so.

_Cost if adopted:_ negligible — one channel config, a few dozen lines. The tick must
respect the session's dim and be switchable from Menu ▸ Coach for shared offices.

### Co-working over ESP-NOW — needs Wi-Fi up during sessions

Two or more devices on a table join one session. Each screen shows the shared countdown
plus a row of sigils, one per person. When someone gives up, their mark hollows out and
everyone sees it.

This is the one that produces a *group* photo, and the reason someone buys a second
unit. `esp_now.h` ships inside the `esp_wifi` component already linked, so it sidesteps
the entire BLE controller problem — none of the 56 KB reserved DRAM region that put
bitchat 24,064 bytes over the link.

_Unmeasured._ ESP-NOW needs Wi-Fi resident during a session, which costs heap and
battery, and the coexistence story with a HotSync is unexamined. Measure before
committing — the same discipline that produced the bitchat answer, rather than an
estimate.

---

## 10. Deliberately not building

- **Badges, XP, levels, focus points.** The audience for a hand-made Palm-style device
  is precisely the audience that finds gamification insulting.
- **Leaderboards, accounts, social feeds.** We're selling a device that can't reach you.
  Don't build the thing you're selling against.
- **Generated encouragement copy.** Six deterministic rules that stay quiet until they've
  earned the right to speak are far more credible than infinite personalised praise.
- **Streak freezes and repair tokens.** A streak you can buy back isn't a streak — and it
  would undo the whole point of sealed mode.

---

## 11. Open risks

- **No audible alarm on current hardware.** Visual-only session end. Fine on a desk,
  wrong in a bag. The piezo in §9 is the fix.
- **Clock drift across a power cycle** on pre-RTC units. Bounded by the +60 min sanity
  gate, but a user who power-cycles mid-session will occasionally lose one.
- **`backlight_sec = 0`** means the screen never blanks, so a 25-minute session would
  burn the backlight at full brightness. The 15% dim covers this, but it must apply even
  when idle blanking is disabled.
- **Sealed mode needs an escape hatch that isn't a feature.** A firmware hang mid-session
  must not brick the device into a locked screen. The five-second hold is the user-facing
  exit; a hardware reset always clears `phase == RUNNING` through the §6 recovery path.
- **Sigil legibility at 24 px.** A mark that reads well at 200 px may be mush on the
  wall. Needs a real look on hardware, and possibly a minimum stroke-extent check at
  capture time.
