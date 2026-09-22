# Work backlog — the single source of truth

The one list of what's left to do. Grouped by **where it can be worked**, because
that's the binding constraint on this project (a base CYD with no PSRAM, developed
mostly away from the bench via the browser simulator).

- **`[s]`** — buildable and verifiable *today* in the emulator (`make -C sim smoke`,
  or https://emu-commits.github.io/cyd-palm-bridge/). No hardware needed.
- **`[d]`** — needs the physical CYD (touch, Wi-Fi/TLS, sound, battery) or a live
  iCloud account. Can usually be *written* off-device and flash-verified on return.

> **The docs folder is four files.** This one is what's LEFT. `BUILD_PROGRESS.md` is
> the completed-work changelog + the hardware/RAM reference + the hard-won lessons.
> `PRODUCT_PLAN.md` is the path to shipping. `COACH_DESIGN.md` is the one surviving
> per-app design spec. Retired analyses are in git history:
> `git log --diff-filter=D -- docs/`.

---

## RESUME HERE — state at 2026-09-22

**THE W PHASE IS CODE COMPLETE. W1–W10 are all built and gated**; what is left of
it is `[d]` boxes. Settings is a nine-tile grid, every tile is a real screen, the
Assistant explains each one from the Graffiti strip, Wi-Fi remembers four
networks and finds them by scanning, the clock can be set by hand, Accounts
discovers collections by name, and a sync no longer implies iCloud. **`Q` is the
next phase** — and Q1–Q8 were written under the same design rules, so read them
before starting.

**The strip is a place to stand.** W3's finding, and the one most reusable thing
the phase produced: 240×112 of screen that any non-typing screen has no use for,
addressable by putting an overlay on `lv_layer_top()` at `PDA_H`.
`speaker_aside()` is that, and `content_clear()` takes it down. It costs the four
silkscreen buttons while it is up — one extra tap on Home — which is the open
`[d]` question on it.

**`lv_font_palm` HAS NO SYMBOLS, and the phase hit that wall three times** in one
day: the pick-list marker (a bullet), the Set date calendar's month arrows, and
— years earlier — C7's check mark. It is a 32..255 Latin subset, so anything
outside ASCII draws as an empty box. Two ways out, both used here: pick an ASCII
character, or set `LV_FONT_DEFAULT` on the one widget that needs the glyph.
Anything on `lv_layer_top()` gets montserrat for free, because it inherits
nothing — which is also why the greeting pane had to name its own font.

**The simulator now fakes the radio, not the flow.** Both the Wi-Fi scan and
iCloud discovery run against fixtures in `sim/stubs/hotsync_stub.c`, so the two
screens whose entire purpose is "never type this value" are gated by CI. Before
this, discovery answered "disabled in the simulator" and its screen had never
been rendered by a test.

**A credential leak was found and fixed on the way** (`nosecrets`, 2026-09-21).
Simulator builds were compiling a developer's real `secrets.h` in, because an
include-order "shield" cannot beat C's rule that a quoted include resolves
relative to the including file's own directory. CI was never affected and could
never have caught it — which is the lesson worth keeping.

**Seventeen items arrived on 2026-09-21 as two phases.** **W — Settings and its
nine wizards** is now code complete (above). **Q — quick entry and polish**
(tap-not-type entry in the Date Book, quick-add rows, a Graffiti stroke
reference, the lock-screen restyle) is the active phase from here.

**Read the W phase's design rules before building anything in it.** Two of them
overturn habits this codebase already has: *no scrolling if it can be avoided*
(swipe scrolling is poor on this hardware/OS pair) and *tap to pick, don't type* —
the keyboard is for server addresses and passwords only.

**The bench device is AHEAD of `main`.** Flashed 2026-09-22 with
`f0fcfb0-dirty` — W1 and W2 as merged, **plus W3 uncommitted from the working
tree**, flashed so W3 could be judged on glass before being committed. Boot
verified clean (`panel init done`, `SD mounted`, `LVGL up`, `lvgl pool: 31100
total, 24100 free`, battery 4228 mV). **If the working tree has moved on and you
are reasoning about what is on the glass, re-flash rather than guess — `-dirty`
means the build is not recoverable from a commit hash.**

It also still carries **P10** (new portraits, the greeting on the week screen,
tap-anywhere), whose **two on-glass questions have NOT been answered** — the user
chose to merge and report later. The web emulator deploys from `main`, so it does
**not** serve W3 yet.

**Nine `[d]` boxes are open** and every one is flashable-and-checkable in a single
sitting on the code already on the device. **The user has said they will test
later — do not tick those boxes for them.**

**P6 is superseded**, not done — see §P6. Item 17 of the new work contradicts its
primary trigger outright.

---

## THE WHOLE LIST — everything left, across all three docs

Concise index. Detail for each is below, or in the named doc.

### A. On the glass now (the device is flashed; these only need eyes)
1. **The nine open `[d]` checks** — P2 launcher, P3 Guru shell, P4 Guru treadmill,
   P5 Guru week, P7 lock restyle, P8 HotSync cancel, P9 list restyle, P10 the new
   portraits + tap-anywhere under a thumb, plus Kana Tier 2's `KW_THRESH` feel.
   §P-checks. **P10 supersedes P1's 2026-09-18 confirmation** — that was the old
   greeting layout.
2. **Coach on glass, round 2** — session dim, hollow marks, give-up hold. §Device.
3. **Wake + lock-screen notes, round 2** — flash length, wake cleanliness, the
   indefinite reflect hold. §Device.
4. **Games ship gate** — Zip drag feel, `New` latency, clocks across a power cycle,
   heap re-measure. §Device.
5. **Live-network verifies** — RSS fetch, weather fetch, sync self-heal, large
   collection, href relocation, `config.ini` round-trip. §Device.

### B. Code to write
6. **W — Settings and its nine wizards. CODE COMPLETE 2026-09-22**, `W1`–`W10`.
   Only `[d]` boxes left: the Wi-Fi join and scan against real APs, a real iCloud
   login, the clock write, an accountless sync, and the two questions the
   Assistant's pane raises. §W.
7. **Q — quick entry and polish. THE ACTIVE PHASE.** Eight groups, `Q1`–`Q8`: Date Book `New` +
   calendar + time list, Graffiti stroke reference, lock-screen restyle, quick-add
   rows in Address / To Do / Memo. §Q.
8. **P6 — Assistant onboarding. SUPERSEDED by W**, kept for its three decided
   entry points, one of which item 17 retires. §P6.
9. **"About this screen" help panel on every screen `[s]`.** §Sim.
10. **Graffiti writing *feel*** — ink-trail / char-echo UX; thresholds still want
    device telemetry. §Sim. (The stroke *reference card* is separate: `Q4`.)
11. **Engine: external merge sort** — turns "refuses safely" into "handles a real
    account". §Engine.
12. **Engine: shrink the sync working set** 23.5 KB → ~7 KB by streaming `g_objbuf`
    and `g_body`. §Engine.
13. **Engine: three small correctness items** — `pdb_read` failing loudly, the
    mass-delete guard's untested positive path, `st->pushDel` mislabelling.
    §Engine.

### C. Tidy-ups (small, known, not urgent)
14. **Move `dash.c` / `dash.h` into `bridge/`** — removes the backwards include path
    at `firmware/components/bridge/CMakeLists.txt:35`. **Verified still open.**
15. **CI simulator-smoke has no `timeout-minutes`.** One line; it hung 1h55m once.
    **Verified still open — there is no `timeout-minutes` anywhere in `ci.yml`.**
16. **Delete or fix the dead boot-SNTP code** at `app_main.c:289-292` — it sits
    below `lvgl_port_run()`'s `while(1)` and can never execute. §Stubbed.
17. **Gate the `[dav]` firmware telemetry** behind a flag, as the host `[sync]`
    lines already are. Needs an ESP-IDF compile to confirm no unused-var warnings.
18. **iCloud data hygiene** — one-time removal of seed contacts / duplicate events
    left in the real account from the broken-sync era.
19. **`heap[wifi-up]` is ~13 KB lower after a session of app use** than after a
    fresh boot (44.9 KB at 18 min uptime). Not chased. The next thread if feed
    fetches fail intermittently.

### D. Hardware decisions (see `PRODUCT_PLAN.md` for the ship context)
20. **C3 — Sound.** No speaker/DAC wired. Gates real alarms and game feedback, and
    is the highest perceived-charm-per-byte item on the list.
21. **U8 — Battery: two loose ends.** Does the gauge track a real discharge? Is the
    divider on tolerance (`BAT_TRIM_PERMILLE`)? §Device.
22. **U8b — Screen-on power.** Idle is *solved*; the charge goes to the backlight.
    §Device.
23. **Hardware button → sleep, not power-off.** Firmware-or-soldering depends on
    which button. §Device.
24. **U9 — Case.** Out of software scope, gating for launch.

### E. Parked — offered, NOT approved (do not build without a yes)
25. Opt-in CORS-proxy RSS fetch in the web emulator.
26. Coach's optional hardware: the 8:00 knock, the piezo tick, ESP-NOW co-working.
27. **S5 — real sync in the sim.** Still open, but its strongest consumer (M2) is
    done, so it now only buys emulator parity. §Stubbed.

### F. Someday
28. Dark mode. *(A Preferences icon in the launcher used to sit here; **W1**
    answers it from the Menu instead, which is where Palm put it.)*

---

## PHASE: the three speakers (CODE COMPLETE — only `[d]` boxes left)

The portraits and everything built on them are on `main` (PR #52, then #55). **P6,
the one group that was never built, has been superseded by the W phase** — the
Assistant's onboarding job is now the Settings wizards, not a standalone flow. So
nothing in this phase is waiting on code; nine `[d]` boxes are waiting on eyes.

**Working agreement.** Tick a box only when it is *verified*, not when it is
written — `[s]` = sim-verified (`make -C sim smoke` + the shot checked by eye),
`[d]` = verified on the glass. Keep each numbered group to its own commit, branch
off `main`.

> **Health content disclaimer (P4).** The Guru task pool is widely-discussed
> consumer wellness practice, NOT medical advice. No dosages, no disease claims;
> categories are named for the *habit*, not for an outcome. The disclaimer ships in
> Guru's Menu > About.

### Done in this phase — `[s]` only unless noted
- **P0** — `speaker_say()` lifted out of Coach; the since-unlock flag; non-repeating
  greeting-line pools.
- **P1 — Coach greeting.** Portrait + bubble on first launch after unlock, tap
  anywhere to advance. **`[d]` confirmed by the user 2026-09-18** — but that
  confirmation is of the *old* layout, which **P10** has since replaced. It does
  not carry over; see P10's box.
- **P2 — launcher reorder + Guru icon.** Nine apps, three rows, nothing below the
  fold; Graffiti moved into Games.
- **P3 — Guru app shell.** `guru.c`/`guru.h` pure and clock-injected; `daycal.h`
  holds the local-day arithmetic; `make -C sim guru`, 61 assertions.
- **P4 — the task treadmill.** 35 tasks in `guru_pool.txt` (**edit the .txt, never
  the .c**), five frozen category indices, an append-only log keyed by stable
  numeric ID, and a daily target = rolling 7-day user average, floor 1. The whole
  pool is one `lv_table`, so it costs one object — no pagination needed.
- **P5 — Guru week analysis.** Per-day figures from the ring, per-category split
  from the log; advice codes `GA_*` in fixed priority.
- **P7 — lock screen restyle.** Three declared zones, six rain bars on one shared
  baseline, zero new widget classes; the vertical budget is `DASH_Y_*`/`DASH_H_*`.
- **P8 — HotSync cancel.** One button, three states; `hotsync_cancel()` is a
  request, not a kill, taken at safe points only.
- **P9 — the lists stop looking like tables.** One `list_table_style()` + one
  `LV_EVENT_DRAW_TASK_ADDED` hook for all five lists; a **drawn** checkbox (this is
  what closed **C7** without touching the font); hairlines, banded Guru headings,
  struck-through completed To Dos, four aligned To Do columns. Heap peak *fell*
  2216 → 2120 B.
- **P10 — hello on the week screen, and the page is the button.** New Coach and
  Guru portraits (1:1, back in through `--from-exact`). Both greetings now stand
  on the speaker's own week screen — shared `co_week_page()`/`gu_week_page()`, so
  hello and the report cannot drift apart. `tap_anywhere()` replaced the "back"
  button on both week screens; the old one sat below the fold, so *leaving* the
  report required scrolling to find it.

### P-checks — the nine open `[d]` boxes
- [ ] **P2** on glass.
- [ ] **P3** on glass.
- [ ] **P4** on glass.
- [ ] **P5** on glass.
- [ ] **P7** on glass.
- [ ] **P8** — **the stop itself is device-only.** The sim's sync is synchronous and
      finishes before any button could be pressed, so `hotsync_busy()` is never 1
      there and the confirmation can never open.
- [ ] **P9** — **this one genuinely needs glass.** `COL_RULE` `0xC8C8C8` and
      `COL_DIM` `0x8C8C8C` are greys chosen in an emulator, and a real ILI9341
      behind a resistive panel is exactly where near-white and near-black stop being
      distinguishable. If the hairline vanishes, darken it; if the meta text reads
      as broken rather than quiet, darken that. Also confirm the 13 px box is a
      comfortable tap target for a finger, not just a mouse.
- [ ] **Kana Tier 2** — tune the per-stroke accept threshold `KW_THRESH` on the real
      resistive panel and confirm the feel.
- [ ] **P10** — the two new portraits at 1:1 on a real ILI9341 (they are ink-on-
      transparent outlines, and a thin outline is exactly what a resistive panel's
      diffusion softens), and **tap-anywhere with a finger**: confirm that scrolling
      a long week does *not* dump you back to the home screen. That is the one
      failure the sim cannot rehearse — a mouse drag is clean, a thumb drag is not,
      and if LVGL's scroll-vs-click threshold is wrong for a finger the report
      becomes unreadable rather than merely awkward.

### P6 — Assistant onboarding — SUPERSEDED BY THE W PHASE (2026-09-21)
Do not build P6 as written. Its whole job — the Assistant explaining Wi-Fi and
CalDAV — is now **W2/W3/W5/W6**, reached from the Settings screen instead of from
a one-shot flow. What is worth carrying forward, and what is not:

- **RETIRED. "Sync Now with no credentials starts the guided flow."** This was
  P6's *primary* trigger, and **item 17 contradicts it outright**: HotSync must
  work with no iCloud setup and must not prompt the user into it. A sync with no
  credentials now does the work it *can* do (clock, RSS) and says so plainly.
  See **W10**.
- **CARRIED → W1.** *Menu > Setup Assistant, permanent and re-runnable after a
  wrong password.* This survives as Menu ▸ **Settings**, which is permanent by
  construction and re-entrable per wizard — a better answer than one linear flow.
- **CARRIED, SOFTENED → W10.** *The launcher's demo-data hint becomes a button.*
  The dead-end text ("edit config.ini on the card, or tap Menu > Preferences") is
  still wrong and still wants fixing. But item 17 says not to push iCloud, so the
  hint should point at **Settings**, not at an account flow, and it should read as
  information rather than as an unfinished-setup nag. **OPEN:** whether the hint
  should appear at all once sync works without iCloud.
- **CARRIED → W6.** Hand off to the existing **Discover collections** flow
  (`hotsync_discover_*`) rather than asking for UUID paths.
- **STILL TRUE.** The account half is `[d]` **device-only** — a real Wi-Fi join,
  the user's Apple ID and an app-specific password. Nothing in W changes that.

### Parked until the W phase lands — the Assistant's other jobs
Ideas only, cheapest first. **Do not build in this phase.** Sync failure explainer
(probably the highest value — sync errors are raw status text today); first-boot
welcome; SD missing/corrupt explainer. *(Two former entries are now W work:
"preferences that still expect pasted paths" is **W6**, "time zone / clock drift
setup" is **W8**.)*

---

## §W — PHASE: Settings and its nine wizards (CODE COMPLETE — only `[d]` left)

Requested 2026-09-21, items 1–8 and 17 of seventeen. The whole configuration
surface is rebuilt: **Menu ▸ Preferences becomes Menu ▸ Settings**, which opens a
nine-icon screen in the launcher's own idiom, and each icon opens a wizard. **The
old Preferences list is retired only when all nine tiles exist** — until then both
routes stay reachable, because the list is the only way to reach a field whose
wizard is unbuilt.

### The design rules — read these before building any W group
They overturn habits this codebase already has, so they are written down once,
here, rather than re-argued per group.

1. **Tap to pick, never type — unless precision demands it.** Every value that
   comes from a known set (zones, times, policies, brightness, a discovered
   collection) is chosen from a list or a grid. **The on-screen keyboard is for
   server addresses and passwords only**, where the value is arbitrary and a typo
   is silent. This is the single rule most likely to be violated by copying the
   existing `pf_edit` single-field editor, which is a textarea for *everything*.
2. **Do not scroll if it can be avoided.** Swipe scrolling is poor on this
   hardware/OS pair — resistive panel plus LVGL's drag threshold. Design each
   wizard step to fit the 240×184 content area outright. Where a list genuinely
   cannot (a zone list, a 27-row time list), it is a *list* being scrolled, which
   behaves far better than a scrolled *page*. **OPEN, and worth settling on
   glass:** the Coach and Guru week screens scroll today (P10), and if item 4's
   complaint applies to them too, `tap_anywhere()` on a scrolling page is the
   exact combination most at risk.
3. **The Home button is the way out.** No Back buttons — P10 established the
   pattern and the reason (a Back button below the fold makes *leaving* the
   hardest thing on the screen). Home returns to the launcher from anywhere in
   Settings.
4. **Pool-safe widgets only.** Unchanged and non-negotiable: no `lv_bar`,
   `lv_slider`, `lv_arc` or `lv_meter` — they allocate draw layers and live-lock
   the fixed 32 KB pool into a WDT freeze. Labels, lists, tables, button matrices
   and the I1 canvas. A brightness *stepper* already exists and is the model for
   any numeric control.
5. **Every wizard is re-entrable.** A wrong password must be fixable by opening
   the same wizard again, never by clearing `config.ini` by hand. This is what
   P6's "permanent Menu ▸ Setup Assistant" bullet was protecting, and it survives.

### The nine tiles — decided 2026-09-21
Every tile maps to configuration that exists today, so none is a dead icon.
**Location is the notable win:** `latitude`/`longitude` are currently reachable
only from a Lock Screen sub-screen, which is why weather silently does nothing for
almost everyone.

| Tile | Covers | Config fields |
|---|---|---|
| **Wi-Fi** | four remembered networks | `wifi_ssid`/`wifi_pass` → ×4 + order |
| **Accounts** | CalDAV/CardDAV login + hosts | `dav_user`, `dav_pass`, `dav_base`, `dav_card_base` |
| **News** | the feed list | `feeds.h` |
| **Date & Time** | time, zone, 12/24h, world clocks | `timezone`, `clock24`, `world1`, `world2` |
| **Display** | brightness, backlight timeout | `brightness`, `backlight_sec` |
| **Location** | weather location | `latitude`, `longitude` |
| **Sync** | conflict policy + collections | `policy`, `cal_coll`, `todo_coll`, `card_coll` |
| **Owner** | name shown on the lock screen | **NEW FIELD** — nothing exists yet |
| **About** | version, GPLv3/PumpkinOS provenance | the existing About panel |

### The groups — one commit each, branch off `main`
- [x] **W1 — Settings replaces Preferences `[s]`.** DONE 2026-09-21. Rename the menu entry; build
      the nine-icon screen on `show_launcher()`'s pattern (a flex `ROW_WRAP` grid
      of 68×52 cells, which is already proven to seat nine icons in three rows
      with nothing below the fold). Home exits. **Every tile opens a real panel
      — none falls through**, which went further than planned: the nine tiles
      between them cover every field the old list held, so each one got a small
      filtered list rather than a stub. `W5`–`W9` now *replace* those lists with
      tap-first wizards instead of building them from nothing.
      Gated by `settings_grid`, `settings_accounts`, `settings_display`,
      `settings_about` and `prefs_list`.
- [x] **W2 — nine icons in early-Palm-OS style `[s]`.** DONE 2026-09-21, ticked
      once `settings_grid` put them on screen. ~24×22 A8, generated the
      way `gen_guru_icon.py` and `gen_zip_icon.py` generate theirs — **one
      `tools/gen_settings_icons.py` for all nine**, not nine scripts. Follow the
      house idiom that `gen_guru_icon.py` documents: the Palm icons are a **solid
      disk with the subject knocked out in white**, ink-as-disk not
      subject-as-ink. Remember P10's lesson — **trim blank rows top and bottom**,
      they are not free.
- [x] **W3 — the Assistant greets Settings `[s]`.** DONE 2026-09-22. First entry
      after an unlock only, rotating non-repeating lines, tap to dismiss. It is
      **not** the Coach/Guru contract in one respect: her greeting does not
      replace the screen, it stands on `lv_layer_top()` **in the Graffiti strip**
      over a finished, live grid — all nine tiles stay visible and tappable, and
      dismissing her rebuilds nothing. The Coach arrangement was built too and
      rejected on the screenshots (her shoulders on the About tile, the balloon
      across the silkscreen row, and a full-screen overlay eating the first tap).
      1344 B of the 31100 B pool while up, all returned on dismiss. The bit is
      spent when she is SHOWN, since she cannot block navigation. **Her lines
      must stay under ~78 characters — the balloon clips, it does not wrap.**
      Gated by `settings_grid` and `settings_greet_gone`.
- [x] **W4 — the Assistant narrates each wizard `[s]`.** DONE 2026-09-22, on
      **every** screen, and the OPEN question is closed by where the keyboard
      lives: the I1.2 tap keyboard is an `lv_buttonmatrix` inside the *content*
      area, so there is no screen in Settings — not even entering a password —
      where she and the input want the same pixels. `SET_BLURB[]` says what each
      tile is FOR; the wizards' own screens each say their piece. She costs the
      step no height at all, so design rule 2 is untouched.
      **Two `[d]` checks W3 opened** remain: does the pane read as her standing
      in front of the writing area or as the screen having grown taller (the one
      hairline at `PDA_H` is all that says so, and P9 is already asking whether
      greys survive the panel), and is losing Home/Menu/Find/Calc for one tap
      per unlock session acceptable or annoying with a thumb.
- [x] **W5 — Wi-Fi wizard: four networks `[s]`, `[d]` OPEN.** DONE 2026-09-22. `Config` grows to four
      SSID/password pairs plus a last-connected marker; `config.ini` gains a
      round-trip for them (**host-gated** — `bridge/config.c` is in `make test`).
      HotSync tries them **in order of last successful connection**, most recent
      first — the array order IS the try order and a join promotes its slot, so
      there is no separate "last used" key to disagree with the list. Four fits
      one non-scrolling list. **An SSID is never typed:** `wifi_scan_*` scans and
      the user taps a name known to exist; the password is the only typed value.
      `config.ini` keeps the unnumbered `wifi_ssid`/`wifi_pass` for slot 1, so a
      card written before this still loads. **`[d]`: the join itself, the scan
      against real APs, and whether 8 s is enough for networks 2–4.**
- [x] **W6 — Accounts wizard `[s]`, `[d]` OPEN.** DONE 2026-09-22. Apple ID and
      app-specific password via keyboard (rule 1's exception), then **Find my
      calendars...** hands off to `hotsync_discover_*` and collections are picked
      by name. The two server addresses moved behind **Advanced** — they default
      to iCloud and read as required fields when they sit in the account flow.
      **Discovery now runs in the simulator** against a pretend account, so the
      flow is gated; before this the one screen that exists to stop people
      pasting UUID paths was never rendered by CI. **`[d]`: a real login.**
- [x] **W7 — News Feeds wizard `[s]`.** DONE 2026-09-22. The tile opens the feed
      list itself (it used to hold one row whose only content was the name of the
      next screen), with no Back button — Home is the way out, like every other
      tile. Picking was already tap-first; what was missing was a way back from a
      mistake, so **Built-ins** restores any deleted built-in without disturbing
      a user's own feed or re-enabling one deliberately switched off.
- [x] **W8 — Date & Time wizard `[s]`, one `[d]`.** DONE 2026-09-22. **The clock
      could not be set by hand at all before**, which matters because a device
      with no RTC wakes from a flat battery in 1970 and the fix (SNTP in a sync)
      needs the Wi-Fi you may be here to set up. Four stepper arrows and an
      AM/PM toggle; the minute steps by five, because 59 taps to cross the hour
      is not a control. The date uses the **same** `lv_calendar` as the Date
      Book's due picker. **`[d]`: the write itself** — `settimeofday()` is
      refused in a container, so the sim gates the screen and not the clock.
- [x] **W9 — the five small panels `[s]`.** DONE 2026-09-22. One shared
      pick-one-of-N screen serves all three choices (backlight timeout, conflict
      policy, location) with the current value marked — a cycling row cannot show
      you the options you are *not* on. **Screen off** was in `config.ini` and
      nowhere in the UI, despite deciding most of the battery life. **Location**
      is now a list of cities: the zone table carries each city's coordinates, so
      the same list the clock uses answers "where are you". **Owner** renders on
      the lock screen, sharing the bottom line with the unlock hint.
- [x] **W10 — HotSync works without iCloud `[s]`, `[d]` OPEN.** DONE 2026-09-22.
      **The engine already did the right thing** — the account stages are skipped
      with a reason and the clock and feeds run regardless — so this was entirely
      about what the user *reads*. The launcher's demo-data hint is **deleted**:
      it dead-ended at "edit config.ini on the card", read as an unfinished-setup
      nag (which item 17 rejects), and sat below the fold where nobody saw it
      anyway. What it was for now sits on the **HotSync screen**, saying what
      *this* sync will do given what is configured, and naming the account as the
      thing that ADDS records rather than a precondition that is missing.
      **`[d]`: confirm a real accountless sync reports clock + news honestly.**

---

## §Q — PHASE: quick entry and polish (ACTIVE)

Items 9–16 of the same request. Independent of W, mostly small, each its own
commit. Design rules 1–4 above apply here too — item 4's tap-don't-type and
don't-scroll instructions were given for the whole request, not just for Settings.

- [ ] **Q1 — Date Book day view gets a `New` button `[s]`.** At the bottom of the
      day's list, opening the new-event screen. Today a new event is reachable
      only through Menu ▸ New (`act_new`, `ui.c:3013`), which is exactly the kind
      of thing nobody finds.
- [ ] **Q2 — new event: pick the date on a calendar `[s]`.** **Most of this is
      already built.** `due_open()` (`ui.c:4030`) is a working calendar popup with
      Today / Tomorrow / 1 Week quick buttons, and it carries a hard-won fix —
      `lv_event_get_current_target()`, because the bubbled `VALUE_CHANGED` hands
      back the button matrix and `lv_calendar_get_pressed_date()` will happily
      cast it and return a date assembled from unrelated memory. **Reuse it; do
      not write a second one.**
- [ ] **Q3 — new event: pick the time from a list `[s]`.** Every 30 minutes from
      8:00 AM to 9:00 PM — 27 rows — with typing as the fallback for anything
      outside that window. A 27-row `lv_list` is one object and scrolls as a list,
      not as a page (rule 2). **OPEN:** does this cover the end time as well, and
      should end default to start + 1 hour?
- [ ] **Q4 — Graffiti stroke reference `[s]`.** A button in the Graffiti app
      showing every character stroke on one page for quick reference. **OPEN:**
      item 12 says "swipeable", which fights rule 2 — propose paged with explicit
      next/prev taps and confirm. The stroke data already exists (the recogniser's
      templates); this is a rendering job, and the I1 canvas is the pool-safe
      surface for it.
- [ ] **Q5 — lock-screen restyle `[s]`.** Item 13: the three black zone bars
      (`CONDITIONS` / `AHEAD` / `SUN & MOON`, drawn at `DASH_Y_WX`,
      `DASH_Y_AGENDA`, `DASH_Y_SUN`) go **grey**, and the zone headers stop being
      bold — `lv_font_palm` already exists alongside `lv_font_palm_bold`, so the
      un-bolding is a style swap, not a font build. **Scope confirmed 2026-09-21:
      the lock screen only.** The inverted app title bar stays black for now;
      whether it follows is a question for the glass, since grey-on-grey through a
      resistive panel is exactly what an emulator judges badly. Pick the grey with
      `COL_RULE` `0xC8C8C8` and `COL_DIM` `0x8C8C8C` in mind — P9's open `[d]` box
      is already asking whether those two are distinguishable on the real panel,
      and this adds a third grey to the same question.
- [ ] **Q6 — Address: shorten the lookup box, add `New` `[s]`.** Item 14.
- [ ] **Q7 — To Do quick-add `[s]`.** A static field pinned at the top with a
      `New` button at its right: type, tap, a row appears. Item 15.
- [ ] **Q8 — Memo Pad quick-add `[s]`.** The same control as Q7, and it should be
      literally the same helper — two quick-add bars that merely resemble each
      other will drift, which is the mistake P10 was built to avoid. Item 16.

---

## §Sim — what can still be done without hardware

- **A short "About this screen" help panel on EVERY screen.** Prompted by a real
  first run: someone opened Zip, connected all the numbers, and had no way to learn
  from the device why it still said "4 left" (the cover-every-cell rule).
  **Shape:** extend the existing **Menu ▸ About** so it is present on every screen
  and shows a couple of short paragraphs *for that screen*; keep the global About
  reachable (it carries the GPLv3/PumpkinOS provenance and the C6 honesty lines).
  **Pool-safe:** one scrollable label in the existing overlay pattern, text in one
  static table keyed by screen, so it costs flash not RAM. Must cover the four
  games' rules, the four PIM apps, Graffiti, HotSync, News and the lock screen.
  Smoke-gate one screen's panel so the overlay can't regress.
- **Graffiti writing feel.** The accuracy work is done and gated (letters 99.7% mean
  at 3 px jitter, digits and punctuation 100%; `make -C sim graf`). What is left is
  the *feel* — ink-trail / char-echo UX — and final threshold tuning, which wants
  real on-device `graf` telemetry rather than the synthetic model. For a hand the
  built-ins still misread, **Train mode** already records a per-device template that
  wins when closer. **Not this item: the stroke reference card is `Q4`** — a
  rendering job against the same template data, needing no telemetry at all.

---

## §Device — needs the bench

### On-glass verifies of things already written
- **Coach, round 2.** Round 1's six notes all shipped. Still open, and still what
  the simulator structurally cannot judge:
  - Does the 15% session dim read as intentional, or as a fault? `CO_DIM_PCT`.
    *Not raised in round 1 — may be fine.*
  - **Hollow (gave-up) marks are faint.** At 30 px the 50% parity stipple leaves
    ~1 px of ink on a 2 px stroke. A thicker stroke for hollow marks, or a
    different "gave up" treatment (a slash through the cell?). Visible in
    `coach_marks_hollow`.
  - The five-second give-up hold (too long? too short?), whether the exported Date
    Book block is welcome or noise, and whether the ritual's three questions are
    the right three.
- **Wake + lock screen, round 2.**
  - Is 1.4 s × 10 phases at 100% the right flash? `CO_FLASH_MS` / `CO_FLASH_N`. It
    ends early on first touch, so the ceiling only costs something if nobody is in
    the room.
  - Does the wake land clean? If any of the previous screen flickers through, the
    one-pass backlight deferral needs a second pass, not a longer one.
  - **The reflect screen holds the display indefinitely, by request** — a session
    that ends face-down must be answered. If that reads as annoying rather than
    insistent, the fix is a timeout that banks the session as unrated, not a lock
    over the top of it.
  - `WX_STALE_MIN` 24 h is a guess. The agenda is deliberately NOT gated — those
    are the user's own records and stay true offline; only readings of the outside
    world are hidden.
- **Games ship gate.** All four are sim- and host-verified; four things only the
  real panel settles. **(a)** Zip's drag feel — the bridge-through-a-free-neighbour
  rule was tuned against a mouse; confirm a fast diagonal fingertip sweep draws the
  intended path and retracing rewinds cleanly (`ZP_CELL`, 24 px, is the knob).
  **(b)** Zip's `New` latency — 2.4 ms on the host, so expect ~40–80 ms on device;
  confirm the button feels instant and the WDT stays quiet (lower the `count_paths`
  budget in `zp_new` if a seed stalls). **(c)** The play clocks across a real power
  cycle — the save holds a *paused* snapshot, so a game resumed after a battery pull
  must show banked time, not the wall-clock gap; best times must survive.
  **(d)** Re-measure heap/BSS after the shared game canvas (expect ~12.5 KB more
  free).
- **Should a game's clock pause when the backlight times out?** Today a game left on
  the desk keeps counting — `ui.c` still considers the screen open. Would need a
  hook from `idle_step()` (`lvgl_port.c`) into `games_pause_clocks()`. Deliberately
  not built blind: decide it on glass, where the real timeout is visible.
- **UX on glass.** Sync-awake screen, brightness stepper, and everything built in
  the sim this cycle (C1 ink, C2 HotSync dialog, C4 forms, I1.2 keyboard, inverted
  title bar, toasts, Week view). *The due-date picker's own bug is fixed* — what is
  left here is whether a date picked on glass survives a real HotSync round-trip to
  iCloud, which the sim's fake sync cannot answer.
- **Coach: retire the `UI_DEVTOOLS` seal escape.** Sealed mode is deliberately
  unreachable from the menu, so CI has no route to the reflect/note screens without
  waiting out a real 25-minute session; today's answer is an invisible
  `UI_DEVTOOLS`-only corner target that jumps the clock. It compiles out of release
  builds, but it is an escape hatch inside the one feature whose premise is that
  there isn't one. Better: a build-time short session for CI, or driving `coach.sav`
  from the harness.

### Live-network verifies
- **RSS fetch.** The whole reader is feature-complete and host-gated; only the live
  network GET is unexercised. Confirm a feed fetches, items appear in News, sync
  stays quick, and heap holds during the fetch.
- **Weather fetch.** `bridge/wxfetch.c` + `fetch_weather()` are built and gated
  against verbatim live responses. Confirm `weather.dat` is actually written and the
  lock screen renders real numbers — which needs `latitude`/`longitude` in
  `config.ini`. With no location the fetch does nothing and says `no weather (no
  location set)`, deliberately loud.
- **Sync self-heal.** Confirm the always-full-reconcile heals To Do (out 2 → 3,
  pulling the orphaned test todo) and that a 2nd sync is idempotent
  (`push=0 pull=0`). Capture the `[sync]` line.
- **Large collection.** >24-record collection round-trips (the `MAXR` cap is gone
  via streaming; confirm on real data).
- **iCloud href relocation.** The idempotency fix for a relocated object whose
  GET-for-UID truncates on the 8 KB no-PSRAM buffer — verify against a photo-heavy
  contact live (no delete/dup/loss).
- **config.ini round-trip.** Edit the Preferences form, run a live Discover →
  assign → Save against a real iCloud account.

### Power and hardware
- **U8 — battery, two loose ends.** The gauge, instrumentation and `power.log` all
  shipped 2026-08-22 and the readings are honest (rested-only sampling; a plugged-in
  device always reads full, because the TP4054 holds the rail and no `CHRG` line is
  broken out).
  - **Does it track a discharge?** Readings across a run down from full, to confirm
    the curve is not wildly off through the flat middle (3.84 → 3.80 V is a tenth of
    the pack).
  - **Is the divider on tolerance?** `BAT_TRIM_PERMILLE` is at unity. One multimeter
    reading at the `JP2` pads against the logged `power: battery:` line settles it.
- **U8b — screen-on power.** **Idle is answered and is not the problem:** >24 h on a
  1100 mAh cell, under 46 mA average (2026-08-27). Clock drift on battery is under a
  minute a day. A sync costs about half a percent, so one a day is not worth
  optimising. **So the target is screen-on time**, ranked, none measured:
  1. **Backlight** — the largest single draw and already a runtime setting
     (`brightness`, `backlight_sec` in `config.ini`). Halving brightness roughly
     halves its share at zero code risk. Try this before touching anything.
  2. **DFS without light sleep** — `CONFIG_PM_ENABLE=y` with
     `esp_pm_configure(.light_sleep_enable = false, .min_freq_mhz = 80)`. **This has
     never been tried.** What was tried and reverted was *automatic light-sleep*,
     which gates APB between LVGL frames and flashes the panel; pure frequency
     scaling does not gate APB on this part, so that failure may not apply. Needs
     one on-device look.
  3. **CPU 160 → 80 MHz fixed.** Costs slower LVGL and a slower TLS handshake — and
     a slower handshake keeps the radio up longer, so the net is genuinely unclear.
     A/B it with `power.log`.
  4. **Panel sleep** (ILI9341 `SLPIN` 0x10) when the backlight blanks. Needs
     `SLPOUT` + 120 ms and a full repaint on wake.

  **Not yet instrumented, and it should be:** current draw is inferred from the
  cell's voltage slope, which is coarse in the flat middle. If numbers come back
  ambiguous, an inline meter on the USB/battery lead is the honest next instrument,
  not a longer log.
- **Make the hardware button sleep, not power off.** Today the firmware reads **no**
  button and has no software power-off path; the only way off is cutting the rail.
  Worth building for UX (instant wake, state preserved). **Which button decides
  whether this is firmware or soldering:** RESET/EN can never be intercepted;
  BOOT/GPIO0 is RTC-capable and works as both sleep trigger and `ext0` wake source;
  the slide switch is a hard cut. **UX consequence:** from deep sleep, tap-to-wake is
  gone — touch is read via pressure z1 over SPI and SPI is dead in sleep. Either wake
  on the button only, or rewire touch IRQ as the wake source. **Measure first:** this
  board's real sleep current, where the LDO and USB-serial chip dominate the ESP32's
  microamps.
- **C3 — Sound.** PalmOS clicked on taps, chirped on HotSync, and alarmed on
  appointments. Needs the CYD's audio out (DAC/I2S + speaker). Also unlocks Date
  Book alarms actually *alarming* — VALARM already syncs.
- **U9 — Case.** Printed enclosure.

---

## §Engine — the sync engine's remaining work

**Collection size: the safety holds, the ceiling stands.** Both silent failure modes
are fixed and gated (`tests/toobig.c`): `sortFile()` returning unsorted on a failed
malloc, and `pdbw_rec()`'s unchecked return dropping records that the *next* sync
read as local deletions. A too-big collection is now refused with `-6` before the
merge — local data untouched, map not republished, status line says so.

- **The ceiling is roughly a few hundred records per collection.** `SV_RAW` is
  ~60–80 B/record and must be sorted in one contiguous block; with ~30 KB of
  largest-free-block during the sort that is ~400 records. Date Book at 73 is
  comfortable; a decade of calendar history is not.
- **Raising it is an external merge sort** — sort fixed-size runs that fit RAM, write
  them to SD, k-way merge back. Every input to the reconcile is already a
  line-per-record file, so nothing else in the engine changes. **This is the piece of
  work that turns "refuses safely" into "handles a real account."**
- **Shrink the working set, 23.5 KB → ~7 KB.** `g_objbuf` (8192, `dav_get` into RAM —
  the news phase already spools to SD and parses from the file; the account sync
  should do the same) and `g_body` (8192, the emit buffer — `pushRec` already dumps
  straight to `BODY_TMP`, so emitting to the file removes it). `g_lrec` (4096) is
  `PALM_REC_MAX`, a format limit — keep. `g_state` (3076) is two 1408-byte sync
  tokens, probably shrinkable. Then the budget in `hotsync.c` can be made honest —
  an honest reserve including the TLS handshake is ~29.7 KB — and left on.
- **Three small correctness items.**
  - `pdb_read` caps at `PDB_MAX_RECS` 20000 and returns -1 above it, which reads as
    an empty local database. The mass-delete guard covers it, but it should fail
    loudly on its own.
  - The mass-delete guard's *positive* path (it fires and holds deletions back) has
    no gate — it is covered only by the other checks staying silent. Forcing it needs
    a fixture where the local PDB is emptied behind the map.
  - `st->pushDel` is incremented on the "deleted on both sides" branch, which never
    touches the network. It reads as a push in the status line; it is not one.

---

## Stubbed — previously planned, overtaken by what we now know

Kept so the reasoning isn't relitigated, cut down to the part that still matters.
Full text in git history.

- **A real RTC part — the timekeeping argument is CLOSED; only the alarm survives.**
  A large pin-census and part-selection analysis (SPI DS3234 on the P3 seat with
  `IO27` as CS, power spliced from P4, `IO35`/`RTC_GPIO5` for the alarm) was written
  on the assumption that RC-oscillator drift made the clock unusable. **The
  2026-08-27 field measurement killed the premise: drift on battery is under a minute
  a day, and idle already clears 24 h.** No part is needed for timekeeping. What
  survives is that an RTC *alarm pin* is the enabler for Coach's 8:00 knock, which is
  parked and unapproved (§E-26) — and `PRODUCT_PLAN.md`'s 2026-08-17 note that an RTC
  is in the production BOM stands on that basis, not on drift. **Open risk if it is
  ever fitted:** the RTC would share SPI2 with the SD card, and a breakout whose MISO
  does not go high-Z when CS is high will corrupt every SD read.
- **Boot-time SNTP as an RTC fallback — reduced to a dead-code cleanup.** The full
  plan (boot ordering, draw-buffer-first allocation, hard timeout, don't let it grow
  into a second sync path) was contingent on rejecting the RTC, which is now moot.
  **What is still real and worth one commit:** `app_main.c:289-292` calls
  `wifi_connect()` / `clock_sync()` *below* `lvgl_port_run()`'s bare `while(1)`, so it
  is unreachable. Delete it, or make it reachable deliberately — but do not bring
  Wi-Fi up during interactive use (`lvgl_port.c:63-66` documents the priority-4 sync
  task starving the LVGL wake-poll). **Verified still present 2026-09-20.**
- **Measure Mode B headroom — largely ANSWERED by the 2026-08-20 heap work.** The
  old `~20 KB (est., UNMEASURED)` figure predates the four fixes that returned the
  heap: measured free at `wifi-up` is now **44.9 KB**. The never-built draw-buffer
  teardown that this item was holding in reserve **was built** — as a shrink (40 rows
  → 6 for the duration of a sync, ~16 KB back) rather than a teardown. What is still
  genuinely unmeasured is the **handshake peak with the UI resident** — read it as the
  drop in `min_ever` between `pre-tls` and `post-tls` in `hs_heap()`, remembering that
  `min_ever` is since *boot*, so an unmoved value means the run was inconclusive
  (reboot and sync immediately for a clean bracket). Folded into §C-19, the 13 KB
  post-session creep, which is the same measurement.
- **C7 — the ✓ glyph in To Do. DONE 2026-09-20, and the blocker was never real.** It
  assumed the tick had to be a *glyph*, which the Palm font lacks in 32–255, so it sat
  behind a font regeneration nobody wanted. A cell can only hold a string, but the
  cell is not the only thing that can paint: the checkbox is now **drawn** in a
  draw-task hook, off a per-cell flag LVGL already stores. No font work, and it
  applies to every list at once.
- **M2 — tear down LVGL draw buffers during sync. DONE 2026-08-20**, and it was not
  optional: with 23 KB free the mbedTLS handshake bottomed out at 48 bytes and every
  HTTPS request failed. Shrunk, not torn down (`BUF_ROWS_SYNC`), which keeps the
  HotSync status line drawable.
- **S5 — real sync in the sim.** A `fetch()`-based DAV transport behind the `dav.h`
  seam. Still open and still gated on how credentials would be handled in a browser
  (today they are deliberately never persisted). **Weakened:** it was partly
  justified by needing a way to exercise M2, which is now done on device. It buys
  emulator parity and little else. Needs a decision before any effort.
- **The 24 KB LVGL pool.** **The pool is 32 KB** and has been since the
  `sdkconfig.defaults` change — measured on hardware as `lvgl pool: 31100 bytes
  total`, and `make -C sim poolparity` now fails if `sim/lv_conf.h` and the device's
  sdkconfig drift apart. Older prose in these docs saying 24 KB is stale; **the gate
  is the authority.** (`smoke32` is named for 32-*bit*, not 32 KB — it is the
  pointer-width parity gate.)
- **Japanese trainer — FROZEN at Tier 2** (product decision 2026-07-19, reasoning in
  `PRODUCT_PLAN.md`). Tiers 1 and 2 shipped and are emulator-verified end to end.
  **Tiers 3–5 (kanji) are NOT planned**; the five-tier analysis is in git history and
  the KanjiVG→polyline pipeline would extend directly if the route reopens. One open
  device item only: `KW_THRESH` (§A-1).

---

## Parked — offered, NOT approved (do not build without a yes)

- **Opt-in CORS-proxy RSS fetch in the web emulator.** Feed servers send no
  `Access-Control-Allow-Origin`, so an in-page `fetch()` is blocked; the routes are a
  public proxy (fragile, third-party) or a self-hosted one (infra). Off by default
  with a configurable proxy URL. **The user has not said yes.** On device the fetch is
  direct and needs no proxy, so this only ever buys emulator parity. Public feeds are
  low-risk (only the URL is exposed).
- **BLE + a companion iOS app: dropped, not parked** — see `PRODUCT_PLAN.md` §2.
- **Coach's optional hardware** (`COACH_DESIGN.md` §9). Specified, **not approved**,
  and nothing in Coach depends on any of it:
  - **The 8:00 knock** — an RTC alarm wakes the ESP32 and the device asks "Career. 25
    minutes. Ready?" Flips it from a passive object into a habit trigger, the single
    biggest lever on *essential*. The ask is routing an alarm pin to an RTC-capable
    GPIO — a trace, not a part. Unknown: deep sleep is untouched here, and `esp_pm`
    light sleep is disabled because it glitches this display.
  - **Piezo tick** — one GPIO and a second LEDC channel (the driver is already in
    `REQUIRES`). Buys a real end-of-session alarm, and a once-per-minute tick while a
    session runs: a ticking object on a desk has presence, and in a shared room it
    signals "I'm in a session" without saying so.
  - **Co-working over ESP-NOW** — two devices share one session; give up and yours
    hollows out for everyone. `esp_now.h` ships inside the already-linked `esp_wifi`,
    sidestepping the BLE-controller problem entirely. **Unmeasured:** needs Wi-Fi
    resident during a session (heap + battery), and coexistence with a HotSync is
    unexamined. Measure before committing.

---

## Someday / nice-to-have

Dark mode. *(A **Preferences app icon in the launcher** used to sit here; **W1**
answers it from Menu ▸ Settings instead, which is where Palm put it and which
leaves the nine-app grid alone. The Graffiti case model is settled: one stroke set
of 26 capital-style letters, lowercase output, upstroke = shift-next, two = caps
lock.)*
