# Build history — what was built, and the lessons that cost time to learn

Three things live here: a **changelog** of completed work, the **hardware/RAM
reference**, and the **hard-won lessons**. The last two are not history — they are
load-bearing facts you cannot re-derive from the code, and they are why this file is
longer than a changelog needs to be.

> **For what's LEFT to do, see `BACKLOG.md`**, which carries the consolidated list for
> all three docs.
>
> The blow-by-blow (step logs, raw serial captures, dead-end diagnoses) is in git
> history — this file was compacted on 2026-07-17 and again on 2026-09-20.
> `git log --follow docs/BUILD_PROGRESS.md` recovers the long-form entries.

---

## Changelog (newest first)

### 2026-09-23 — bench follow-ups, and the robustness list worked through

**Fast typing lost keys, and the cause was the sample rate.** The panel was read
every 33 ms, so the lift between two quick taps could fall between samples;
LVGL then saw one press sliding from one key to the next, and a button matrix
cancels a press that slides off its key. The calculator lost the second digit;
the phone keypad, which typed on release, lost both. Now: 10 ms reads, and while
a keypad is up a press that jumps further than a finger moves in one read is
split back into two taps (`tapsplit.h`, run by both the device and the simulator
input paths, so the smoke reproduces it). Side effect worth knowing: a faster
read rate changes how far a drag scrolls, and one smoke tap had to be re-aimed.

**Sync with no Wi-Fi goes where it gets fixed** — Settings ▸ Wi-Fi, with the
Assistant saying what happened and what to tap. `hotsync_wifi_problem()` says
which failure; the simulator stub decides reachability from its pretend
neighbourhood, so the flow is gated.

**The robustness proposals, all seven:**
- **Crash-safe writes** (`bridge/safefile.h`). Every durable file — the four
  databases, `config.ini`, the feed list, the weather cache, the demo manifest,
  the Graffiti templates, every `.sav` — is written to `.tmp`, fsynced, and
  swapped in; every reader recovers first. `pdbw_commit()` had a second bug the
  swap also fixes: a failed read-back left the half-built PDB as the database.
  `tests/safefile_test.c` builds each crash state by hand, including a MemoDB
  caught between remove and rename (Memo has no server copy).
- **Tombstones.** The engine already understood Palm's delete bit; the device
  never set it. A delete in an app that syncs now keeps the record, flagged,
  until the sync pushes it; the UI's readers hide it (`live_read()`). Memo, and
  apps with nothing to sync to, still delete outright, so tombstones cannot pile
  up. The engine now pushes a tombstone even while the mass-delete guard is up,
  because a flagged record can only be the user's delete — which finally lets
  the guard tell a bulk delete from a bad card read. Gated in `massdel` (engine)
  and the new `make -C sim data` (device).
- **The demo seed is never pushed** (`sync_set_hold()`), and a user's new record
  never takes a uid inside the seed's range, or it would never sync either.
- **A release profile.** `UI_DEVTOOLS` was compiled into every firmware build;
  it is `CONFIG_CYD_DEVTOOLS` now, off by default.
- **`secrets.h` is gone.** Nothing is compiled in. It turned out to be the only
  definition of `SYNC_PDB`, which the firmware build caught.
- **The pool is watched on the device**: a UART line at each new low-water mark
  naming the screen, and a warning under the smoke's 3 KB floor.
- **The passwords are off the card** (`secretstore.[ch]`, NVS). A password found
  in `config.ini` is moved on boot and the file rewritten; if the store refuses,
  the file keeps it rather than losing it. Not encryption — see `SECURITY.md`.

### 2026-09-23 — phase R: the speakers step aside, and the week becomes a score

Fifteen refinements from one request (`BACKLOG.md` §R), on
`feat/r-phase-polish`. The ones worth remembering:

- **R4 — the week screens are a score now, not a report.** This week against
  last ("3 to beat last week" rather than "-2"), a seven-day chart with the
  target as a dashed line (solid bar = reached it, outline = did not, today
  underlined), the streak against the best streak, and the split as
  proportional bars — Guru shows all five categories and an empty one is a
  dotted track, because the gap is the point. One heap canvas plus labels;
  days are bucketed by LOCAL day (`cal_window_slot()` in `daycal.h`, gated in
  `guru_test`), so the columns are Monday, Tuesday... rather than 24-hour
  slices starting at the current time of day.
- **R3 — Coach and Guru greet from the strip**, portrait on the right, over a
  landing page that is live underneath (`speaker_aside_ex()`). The stacked
  portrait-over-balloon layout and its scrolling page are gone.
- **R12 — swipes: measured, then fixed.** A new harness set draws hurried
  swipes; the old rule caught **76.8 %** of them, the new one **99.5 %**, with
  letters, digits and punctuation unchanged at every size tried.
- **R8/R9 — fields know what they are for.** A per-field mode decides
  auto-capitalisation (Each Word / first letter) and whether a tap opens the
  phone keypad. The smoke's `k` goes through the same rule, so it is gated.
- **R11 — the lock over the Calculator**, and a harness verb `L` that raises
  the lock the way a screen sleep does, which made it testable at all.

**`lv_layer_top()` inherits nothing, and twelve modals had been drawing in
montserrat_14 for months.** The Options menu, the Calculator, About, every
confirmation and picker — which is why "Remove demo data" clipped in a menu
sized for the Palm font. The fix is one line on the layer; the one widget that
needs montserrat (a calendar's `LV_SYMBOL` arrows) now asks for it on its two
buttons only — setting it on the whole header made "September 2026" clip.
**The cost of the fix was every scripted menu tap**: Palm rows are shorter, so
each item moved up a few pixels per row and the smoke's taps (absolute
coordinates) landed on the wrong rows. The run stayed green throughout; the
screenshots are what showed it.

**`lv_pct(100) - 20` is 80 %, not 100 % minus 20 px.** A percentage is a tagged
coordinate, and arithmetic on it moves the percentage. That was the whole of
R1's "blank row" — 17 px nobody chose, under Guru's header.

**Making something non-modal changes what every scripted tap does.** The old
greetings blocked the screen, so the smoke dismissed them by tapping anywhere.
Once the greeting stood aside, that same tap landed on the live list and opened
a habit — and every later Guru shot drifted, still green.

**Also fixed on the way:** `gu_build_header()` created a new streak label on
every tick (a pool object per habit ticked); the About text had hard line breaks
tuned for montserrat; and every top-layer modal other than the Calculator now
closes when the lock rises, instead of floating over it.

### 2026-09-22 — Q5–Q8, the engine's correctness items, and a guard that never restored

- **Q5 — the lock screen's zone bars are grey.** The grey could not come off the
  canvas: it is I1, two palette entries, and index 1 is every other mark on the
  screen. So the bar is a background on the heading label that was there anyway
  (`dash_zone_hdr()`), which costs nothing from the pool. The fill is
  `COL_RULE`, the hairlines' value — no third grey.
- **Q6/Q7/Q8 — one top bar for Address, To Do and Memo** (`list_top_bar()` +
  `quick_add_cb()`, one predicate `list_bar_app()`, one height `LIST_BAR_H`).
  On Address the field is a filter and `New` opens a blank form; on To Do and
  Memo the field *is* the record. `New` on an empty field opens the full form,
  and a quick-added record is filed under the category the list is showing.
  **Sharing went one step too far and the gate caught it:** the field inherited
  Look Up's 23-character cap and clipped "Pick up the dry cleaning" to
  "…cleanin". The cap is a parameter now.
- **The smoke can type.** `k <text>` writes into the focused field through
  `lv_textarea_add_char`, one character at a time, as the recogniser does. The
  clipped quick-add was invisible until then: a screen whose behaviour depends
  on what is IN a field could only be photographed empty.
- **Tidy-ups.** `dash.c` moved to `bridge/` (the backwards `../../main` include
  is gone); every CI job has a `timeout-minutes`; ~150 lines of unreachable boot
  code below `lvgl_port_run()` deleted, including a drifted second copy of the
  effective-host logic, so `app_main.c` no longer includes `secrets.h`.
  The `[dav]` telemetry was left ON deliberately: the serial line is the only
  window into a sync, and one of those lines found the geoip bug.
- **Engine.** `pdb_read` names every failure on stderr instead of reading as an
  empty database; deleted-on-both-sides counts as `bothDel`, not a push.
- **THE MASS-DELETE GUARD NEVER RESTORED ANYTHING.** It declined to delete and
  wrote nothing for the missing records, so a device whose card read short
  stayed empty and every later sync reached the identical conclusion — server
  keeps everything, device keeps nothing, forever. Its comment promised a
  restore it had never performed. It survived months of green CI because it
  had **no test**; `tests/massdel.c` failed on its first run. The guard now
  pulls the server's copies back in the guarded run. **The trade:**
  `data_delete()` drops a record rather than tombstoning it, so a real bulk
  delete and a bad card read look the same from inside the engine, and
  restoring undoes a genuine bulk delete. That is the right way round.

### 2026-09-22 — Q1–Q4: the Date Book stops asking you to type, and the
### strokes get a reference sheet

- **Q1** — a `New event` button on the day view, fixed at the bottom rather than
  the list's last row: a full day would push that row below the fold, and "add
  an event" must never be hidden by how busy the day is. It lands on the day you
  are looking at, which `default_appt()` already knew how to do.
- **Q2/Q3** — the date and time were TYPED, in formats you had to know
  (`M/D/YYYY`, `h:mm`) and parsed with `sscanf`. **A typo did not fail: it
  silently kept the record's previous value**, so a mistyped date looked saved
  and was not. Both are picked now, and a picked value cannot be malformed.
- **Q4** — every stroke the recogniser knows, on one scrolling sheet, each with
  a filled dot where the pen starts.

**Two clock formats, obeying nobody.** The day list printed `18:00` and the week
list printed `6:00p`, so the same event read differently depending on which way
you had zoomed into it — and Settings ▸ Date & Time changed neither. Then the
title bar, the weather strip and Rise/Set turned out to be hard-coded too. One
`fmt_hm()` (plus `fmt_hour()` for the strip's narrow columns) now serves them
all. The day list is the subtle one: the data layer zero-pads `HH:MM` *precisely
so a lexical sort is a chronological one*, so the rewrite happens in the UI
**after** the sort. The sortable form and the readable form are different jobs.

**`lv_malloc` IS THE POOL, NOT THE HEAP.** The stroke sheet needs an 11.5 KB I1
canvas, and the first version asked `lv_malloc` for it — which on this device
means the fixed 31 KB LVGL pool, not the ~140 KB system heap. It left the pool
too thin for the 36 labels that came next and **segfaulted the moment the screen
opened**. They are two separate budgets and only one of them is scarce; plain
`malloc` is right for a raw buffer, and every LVGL *object* still comes from the
pool. The comment above that allocation now says so, because the comment I had
written confidently asserted the opposite.

**The rule about scrolling got sharper, from the bench.** Rule 2's objection is
to scrolling a page you must **select** from, where a drag that lands as a tap
picks the wrong thing. A page you scroll to **read** has no such failure. So the
stroke sheet is one scrolling page rather than three you lose your place in.

### 2026-09-22 — the fixtures were invented, so the gate agreed with the bug

The location lookup did not work on the bench, twice, and the reason was in the
test file rather than the code. `ip-api.com`'s CSV endpoint **answers in its own
field order and ignores the order the query asks for**:

```
asked:  /csv/?fields=status,lat,lon,timezone,city
got:    success,Bloomfield,40.803,-74.1909,America/New_York
asked:  /csv/?fields=city,status,timezone,lon,lat      <- deliberately shuffled
got:    success,Bloomfield,40.803,-74.1909,America/New_York      <- identical
```

So the parser read a town name where a latitude belongs, `coord_ok` rejected it,
and the sync reported "reply not understood" — correctly, and uselessly.

**Every fixture in `geoip_test.c` had been written in the shape the author
assumed**, and all of them passed. Worse, the gate asserted *"the URL asks for
exactly the fields this parser reads, in order"* — an assertion that checked the
assumption against itself and returned a confident green while the device failed.
**An invented fixture tests the author, not the service.** The fixtures are now
verbatim captures, and the assertion about field order is gone: what replaced it
is a test that the parse does not DEPEND on order.

The fix is the **JSON endpoint**, on a device that deliberately avoids JSON —
because only the named form says which value is which. Nothing parses JSON in
the general sense: it is a bounded search for `"key":` and a copy of what
follows, which is all a five-key flat object needs and costs no heap, no
tokenizer and no recursion. `fields` still trims the reply; it just cannot
dictate the order.

Two smaller things came out of the same bench report:

- **The sync result and the "what this will do" line were drawn on top of each
  other.** A finished sync's status is several lines and grows downwards into
  the explanation's space. The explanation answers "what happens if I tap this",
  so the tap retires it — gated now by a `hotsync_done` shot.
- **The reply is logged verbatim with its HTTP status.** It is a coordinate and
  a town, not a credential, and "reply not understood" left nobody able to say
  what the server had actually sent. Had that line existed one build earlier,
  this would have been a five-minute fix.

### 2026-09-22 — the flag that froze every device already in the field

Reported from the bench: a sync still did not move the coordinates. The cause was
the DEFAULT chosen an hour earlier, not the mechanism.

`loc_auto` had two states, and a card written before it existed has no such key.
Reading that silence as "pinned" protected hand-edited cards — and froze **every
device whose location came from the city list**, which is every device that had
used W9. The rule protected a minority and broke the majority it was built for.
The opposite default would have been worse: silently overwriting numbers a person
typed.

So the flag has THREE states, and `-1` means "the file has not said". It is
settled once, at load, by the only evidence that exists: **the pickers can only
ever write a built-in city's coordinates, verbatim.** An exact match against the
table means a picker put them there (refine it); anything else means a person
chose those digits (leave them). `config_save` always writes 0 or 1, so a card is
asked at most once. The one case it gets wrong is someone who typed, by hand, a
coordinate matching a built-in city to the digit — and they get refined to the
same town they typed, which is the harmless direction to be wrong in.

**The deeper fault was that the device said nothing.** A lookup that never ran
looked, on the glass, exactly like a lookup that ran and changed nothing — and
the only explanation went to the serial log, where a person holding the device
cannot read it. The sync's status line now carries what the location lookup did
or why it declined: `located Boston`, `location kept as set`, `no location
(unreachable)`, `no location (reply not understood)`. **A sync that silently
declines to do a thing has to say so where the person tapping Sync can read it.**

### 2026-09-22 — a location can be approximate, and a sync may improve it

Same day, caught by the user: pick "New York" off the city list and a sync would
never refine it. The rule as first written was "never overwrite a location the
user chose" — but **choosing a city from a list of two dozen is not choosing New
York, it is choosing the nearest one on offer.** Somebody in Boston taps New
York, and the code then treats that coarse guess as sacred. The one user who
most needs the IP refinement was the one it refused to run for.

So a location now carries where it CAME FROM (`loc_auto`):

- **Approximate** — from the zone, the city list, or a previous lookup. A sync
  re-derives it, which also means the weather follows a device that travels.
- **Pinned** — coordinates somebody typed. Never touched.

**The default is PINNED, and that direction is the part that protects people.** A
`config.ini` written before the flag existed has hand-entered coordinates and no
`loc_auto` key, and the safe reading of that silence is "a human put these here".
Everything that fills the location automatically sets the flag on its way past,
so silence can never mean "help yourself". `config_test` gates the direction, not
just the round-trip.

**The panel says which kind it is holding** ("Updates: when you sync" / "kept as
set"), because otherwise the behaviour is invisible: two devices showing the same
place would behave differently on the next sync with nothing on screen to say
why. It doubles as the off switch for anyone who wants the forecast somewhere
other than where the device is.

**A refined coordinate matches no city in the built-in table** — being better
than all of them is the point — so the reply now carries the place name too, and
`loc_name` is what the panel shows. Without it the display would fall back to raw
numbers at the exact moment it got more accurate, which reads as a regression.
The name is the LAST field in the CSV on purpose: a place name may contain a
comma ("Washington, D.C.") and a field read to end-of-line cannot be cut by one.

### 2026-09-22 — the location stops being two numbers you type

Latitude and longitude were the last values in Settings that could only be
entered as numbers, and a wrong one fails in the worst way available: silently.
Weather simply never appears, and nothing on screen says why. W9 made them a
list of cities; this makes them something nobody has to answer at all.

- **A zone pick places the device.** Setting a time zone is unavoidable, and
  W9 gave the zone table each city's coordinates, so an unplaced device places
  itself for free, offline, with no taps and no network. It is a ZONE and not a
  town -- America/New_York from Boston is a forecast 300 km away -- so it fills
  an EMPTY location only and never replaces a city the user picked.
- **The first sync corrects it.** `locate_by_ip()` asks ip-api.com's **CSV**
  endpoint, for the same reason wxfetch asks Open-Meteo for `&format=csv`: this
  device has no JSON parser and no heap to spare for one. The reply is one short
  line. It runs ONLY while the location is unset, so it is one request in a
  device's life rather than one per sync, and it runs BEFORE fetch_weather() so
  the forecast in that same sync uses what it found -- the alternative is telling
  somebody their new device will have weather tomorrow.
- **The timezone comes back in the same reply** and is taken on the same terms:
  only if the device does not already have one. A device that has never been
  configured has no zone either, and this is the one moment it can learn both.
- **Plain HTTP, deliberately.** The free tier serves no TLS, and there is nothing
  here worth protecting: the request carries no identity beyond the source
  address every server already sees, and the worst a man-in-the-middle achieves
  is the wrong town's weather. The TLS handshake is the largest single allocation
  a sync makes on this device; paying it for that would cost more than it buys.
- **Wi-Fi positioning stays rejected** (PRODUCT_PLAN, 2026-08-19) and W5's scan
  does not reopen it: a forecast resolves to kilometres, Mozilla's free service
  retired in 2024, and Google's key would ship inside the device where it leaks.

**The gate holds the URL and the parser together.** The reply is POSITIONAL, so a
field added or reordered in the query string silently shifts every column --
`geoip_test` asserts the URL asks for exactly the fields the parser reads, in
order, alongside the parse itself. The fixtures include the two failures that are
normal rather than exceptional: a carrier NAT's `fail,private range`, and a
captive portal's HTML. Both must leave the location untouched, because empty is
how "not set" is spelled and half-set would look deliberate.

### 2026-09-22 — the rest of the W phase: W4–W10

Seven groups in one sitting, all `[s]`-gated. The common thread is the phase's
two design rules — **tap to pick, never type** and **do not scroll** — applied
until the only things left typed are a password, an Apple ID, and a feed URL.

- **W4 — she explains every tile.** `SET_BLURB[]` says what each setting is FOR,
  never what to tap next. **The plan's open question is closed by where the
  keyboard lives:** the I1.2 tap keyboard is an `lv_buttonmatrix` inside the
  *content* area, so there is no screen in Settings — not even a password — where
  she and the input want the same pixels.
- **W5 — four Wi-Fi networks, and the SSID is never typed.** The array order is
  the try order and a join promotes its slot, so there is no "last used" key to
  disagree with the list. `wifi_scan_*` scans; the user taps a name known to
  exist. **`STA_START` no longer auto-connects** — with one network that was the
  same as connecting on purpose, with four it burns the first network's retry
  budget joining `""`. Slot 1 keeps the unnumbered `wifi_ssid`/`wifi_pass`, so
  cards written before this still load. Two security gates widened to all four
  slots: `nosecrets` and the wasm password scrubber.
- **W6 — Accounts is two fields and a button.** "Find my calendars..." hands off
  to discovery and collections are picked by name. Server addresses moved behind
  **Advanced**; in the account flow they read as required fields.
- **W7 — the News tile opens the feed list**, not a panel holding one row that
  names the next screen. **Built-ins** restores a deleted feed: the URL is the
  one thing here nobody can retype.
- **W8 — the clock can be set by hand at all.** It could not before, which
  matters on a device with no RTC: it wakes from a flat battery in 1970 and the
  fix needs the Wi-Fi you may be standing there to configure. Minutes step by
  five — 59 taps to cross the hour is a punishment, not a control.
- **W9 — one pick-one-of-N screen** for the backlight timeout, the conflict
  policy and the location. A cycling row cannot show you the options you are NOT
  on. **Screen off** had been in `config.ini` and nowhere in the UI, despite
  being the setting that decides most of the battery life. **Location** became a
  list of cities by giving the zone table each city's coordinates. **Owner** now
  renders on the lock screen, which is the only reason to collect a name.
- **W10 — a sync stops implying iCloud.** The engine already skipped the account
  stages with a reason; this was about what the user *reads*. The launcher's
  demo-data hint is deleted — it dead-ended at "edit config.ini on the card",
  read as a nag, and sat below the fold. What it was for now sits on the HotSync
  screen, saying what *this* sync will do.

**`lv_font_palm` has no symbol range, and this phase hit that wall twice more.**
The pick-list marker was a bullet and the Set date calendar's month arrows are
`LV_SYMBOL` glyphs; both drew as empty boxes. It is the same wall C7 hit looking
for a check mark. Two ways out, both used: choose an ASCII character, or set
`LV_FONT_DEFAULT` on the one widget that needs the glyph. Anything on
`lv_layer_top()` gets montserrat for free because it inherits nothing — which is
also why the Date Book's calendar has always looked right by accident.

**The simulator now fakes the radio, not the flow.** The Wi-Fi scan and iCloud
discovery both run against fixtures. Discovery used to answer "disabled in the
simulator" with zero results, so **the one screen that exists to stop people
pasting UUID paths had never been rendered by CI.**

**Three times in this phase a tap in the smoke script missed and the run stayed
green** — the Date & Time rows shifted 56 px under an inserted row, a Cancel tap
fell between two buttons, and the role popup's header ate a tap meant for its
first option. Each produced a screenshot of the *previous* screen under the new
screen's name. Measure off the PNG; never guess a coordinate.

### 2026-09-22 — the Assistant greets Settings, from the Graffiti strip (W3)

- **A greeting that does not take the screen away.** Coach and Guru greet you *over
  their own week screen*, which works because they have one. Settings has nine tiles
  and no such page, and the portrait-plus-balloon pair is **164 px tall against a
  184 px content area** — so the Coach arrangement would have buried the grid it was
  introducing. She stands on `lv_layer_top()` **in the Graffiti strip** instead:
  240×112 that this app has no use for, because a grid of nine icons is not something
  you write into. All nine tiles stay visible **and live**, and dismissing her
  rebuilds nothing — the screen behind her was already finished and correct.
- **The arrangement that was tried and rejected** was the Coach one unchanged, pushed
  down until the balloon landed on the strip. It needed no new geometry and cost the
  same 1.3 KB. Rendered, it put her shoulders on the About tile, laid the balloon
  across the silkscreen row with the hint text colliding with Menu and Calc, and —
  the real fault — **a full-screen tap-anywhere overlay swallows the first tap**, so a
  tile tapped while she was up did nothing at all. Both were built and screenshotted
  before either was argued about, which is the only reason that was obvious.
- **One set of parts, two placements.** `speaker_say()`'s innards came out as
  `spk_portrait()` / `spk_bubble()` / `spk_tail()`, and the wedge is now **one painter
  drawn in its own u/v coordinates and transposed** — upright for a balloon under the
  face, on its side for one beside it. Two wedges that merely resembled each other is
  exactly the drift P10's shared week page was built to prevent.
- **Her own balloon swallowed the tap that dismisses her**, first build. That is
  precisely what `tap_anywhere()` exists for — an `lv_obj` is clickable by default, so
  the one place you would naturally aim was the one place that did not work. The fix
  was to call it, not to write anything.
- **The greeting bit is spent when she is SHOWN, not when she is tapped** — where this
  parts company with Coach and Guru on purpose. Theirs *is* the screen, so a tap is
  the only way past it. Hers sits over a live grid: you can open a tile and never tap
  her, and a hello that came back because you took the other route is a nag.
- **`content_clear()` closes the pane.** It is on `lv_layer_top()`, so `lv_obj_clean()`
  cannot reach it — the same trap Coach's seal documents, and it is what stops her
  being left hanging over the lock screen when the display sleeps.
- **Cost, measured on the true device-sized pool** (`smoke32`, 31100 B): **1344 bytes
  while she is up, all of it returned on dismiss** (12976 B free before and after). No
  draw layers — the portrait is flash-resident A8 recolored like the launcher icons,
  the tail reuses the shared I1 buffer, the balloon is a plain bordered rect.
- **Keep her lines under ~78 characters.** The balloon is a fixed height so short and
  long hellos are the same object, and a line over that does not wrap — it **clips**,
  top and bottom. Gated by `settings_grid` (now with her in it) and
  `settings_greet_gone`.

### 2026-09-21 — Preferences becomes Settings, and it is a launcher (W1)
- **Menu ▸ Preferences is now Menu ▸ Settings**, opening a nine-tile icon grid instead
  of a fourteen-row list. The grid is `show_launcher()`'s geometry *deliberately* —
  same 68×52 cells, same `ROW_WRAP`, same `SPACE_EVENLY` — so the tiles land on the
  same centres as the nine apps, and Settings looks like what it was on Palm: an app.
- **Every tile opens a real panel; none falls through.** The plan allowed stubs, but
  the nine tiles between them cover every field the old list held, so each got a small
  filtered list. `W5`–`W9` now *replace* those with tap-first wizards rather than
  building them from nothing — the grid is the seam that makes that one tile at a time
  instead of one rewrite.
- **A screen's return address is the caller's business, not the field's.** The field
  editor used to infer where "back" went from *which field* was open (lat/long → the
  Lock Screen panel, everything else → the Preferences list). W1 broke that: latitude
  is now reachable from both the Lock Screen panel and the Location tile, so the field
  no longer knows. `g_set_ret` is set on the way in and read on the way out. The zone
  picker had the identical bug for the identical reason.
- **The smoke gate lied again, and louder this time.** Every old Preferences tap still
  landed on *something* after the grid arrived, so the run still exited 0 — while the
  screenshots quietly became pictures of the Apple ID editor with the brightness drags
  typing junk into it. **The brightness stepper, whose regression HANGS the run, was
  not being exercised at all, and nothing said so.** Re-pointed, with `settings_grid`,
  `settings_accounts`, `settings_display`, `settings_about` and `prefs_list` added.
  Second time in one day: *the exit code is not the gate, the pixels are.*
- **A tap at y=45 is not row two.** List rows start at y=37 and are 28px apart, so
  y=45 is still inside row one — it opened the system Time Zone picker instead of the
  World Clock one, which in a thumbnail looks close enough to pass unnoticed.
- **The old one-list view is kept, behind Settings ▸ About.** It is the only screen
  that shows every setting at once, which is what you want when a `config.ini` is
  wrong and you need to see why. It stays gated (`prefs_list`) rather than drifting
  into untested code.

### 2026-09-21 — The simulator was compiling real credentials in, and CI could never have caught it
- **`sim/Makefile` claimed `sim/include` "shields the build from a real (gitignored)
  `secrets.h`" by sitting first on the include path. It cannot, and never did.**
  `appcfg.c` lives in `firmware/main` and includes `"secrets.h"` **in quotes**, and C
  resolves a quoted include *relative to the including file's own directory* before it
  searches any `-I` path. `firmware/main/secrets.h` therefore won every time and
  `sim/include/secrets.h` was never once opened. Every simulator binary built on a
  machine with real credentials had the Wi-Fi password and the Apple app-specific
  password compiled into it, with the SSID and Apple ID legible in the smoke
  screenshots. Confirmed with `strings sim/build/sim_host`.
- **The shield is now `-DSIM_NO_SECRETS`**, which `appcfg.c` honours by not including
  the header at all, so every `WIFI_SSID` / `DAV_PASS` macro is simply undefined and
  `seed_from_secrets()` compiles away. *An include-order trick cannot beat the
  language's own lookup rule; only not including the file can.*
- **Why it survived: it was invisible precisely where anyone would look.** `secrets.h`
  is gitignored, so CI has no such file, so CI builds were always clean and every
  published artefact was genuinely safe. The exposure existed only on developer
  machines, only in build outputs nobody diffs. **A green CI was evidence of nothing
  here** — the one environment guaranteed not to reproduce the bug was the only one
  being watched.
- **The gate has to fail where the bug lives.** `make -C sim nosecrets` points
  `CFG_PATH` at a file that cannot exist, so `config.ini` cannot overlay the seed and
  any non-empty credential field can only have come from a compile-time seed. It
  passes trivially in CI (nothing to leak) and fails on exactly the machines that have
  a `secrets.h`. Verified in *both* directions before being believed: built without
  the flag it reported all four fields seeded; with it, clean.
- **A gate that catches a credential must not then publish it.** `nosecrets` prints the
  field's *name and length* and nothing else. The failure output of a security check
  ends up in CI logs and terminal scrollback forever.
- **The flag is defined once, as `SIMDEF`.** `smoke32` rebuilt `CFLAGS` from a
  hand-copied list, which is how a build flag silently stops applying to half the
  gates — and a security flag that applies to *some* builds is not a fix.

### 2026-09-21 — Nine Settings icons, knocked out of one disk (W2)
- **`tools/gen_settings_icons.py` draws all nine in one file**, not nine files. The two
  icon generators before it are a script each, which was right for one icon and is wrong
  for nine: these share a drawing toolkit, and far more importantly they share a *look*.
  Separate files drift — one author picks a 7.4 disk and the next picks 8.6, and the grid
  ends up with icons that are individually fine and collectively a jumble.
- **The house idiom held.** A Palm launcher icon is a solid disk with the subject knocked
  out of it in *white*; detail is then painted back in ink inside the hole. Drawing the
  subject as ink on nothing is the obvious approach and the wrong one — it reads as
  spindly line art beside the PumpkinOS originals.
- **Painting ink back in outside the disk grows a wart, and knowing that did not prevent
  it.** The hazard is written at the top of the file; the Owner tile walked into it within
  the hour, because the ink gap keeping its head off its shoulders is a full-width row and
  a full-width row does not stop at the disk edge. It rendered as a bar straight through
  the icon. The fix is a `clip()` applied to every tile on the way out, not a rule to
  remember at the ninth icon — *"remember to clip" is not a rule that survives nine
  icons.*
- **Three designs died at 24x22, and the sizes are why.** A *screen on a stand with a sun
  lit inside it* needs ~10px of width to read as a screen, which is the entire width the
  disk has at that height — so its top edge sheared the disk's crown off. *Three Wi-Fi
  arcs* need three 2px whites plus two ink gaps plus a dot, which is more radius than
  there is; the third ate the last gap and the fan became a crescent moon. *Two chasing
  arcs* for Sync leave their only surviving ink at the far left and right, exactly where
  the arrowheads have to go, so the interior flooded white. Two arcs, a sun, and two
  straight arrows replaced them.
- **The measurement that decides a knocked-out shape is the INK GAP, not the shape.** A
  sun with body 3.4 and rays starting at 4.7 leaves 1.3px of ink, and a 1.7px ray simply
  bridges it — the sun fused into an asterisk and the centre row ran white edge to edge.
  Body 3.0, rays from 5.0, 2px of gap: holds. Same lesson in the Owner tile's 2px chin
  gap and the Wi-Fi fan's 1.5px between arcs.
- **Look at the pixels, twice.** ASCII preview catches geometry bugs (a wedge where an arc
  should be); only a rendered PNG catches *reading* bugs — Owner passed ASCII inspection
  and was plainly a bowl once drawn. Both are in the tool: `--preview` for the first, and
  the disk being pure 0/255 A8 means any PNG writer does the second.

### 2026-09-21 — Hello happens on the week screen, and the page is the button
- **New Coach and Guru portraits**, revised at 1:1 and taken back through
  `--from-exact` — brows on both, a moustache that reads as hair rather than a second
  mouth, and the Guru's third eye dropped a row to sit between the brows. No reduction
  was involved: these arrived as 60 px art, which is the round trip working as designed.
- **The greeting is no longer a screen of its own.** Coach and Guru now say hello
  standing on their own *week* screen — same stat column, same portrait, the hello in
  the balloon where the verdict goes (`co_week_page()` / `gu_week_page()`, shared by
  both callers so the two cannot drift into layouts that merely resemble each other).
  Hello used to be an empty frame to get through; it now spends the one moment you are
  certain to be looking at the speaker showing you the numbers they are there for.
- **`tap_anywhere(page, cb)` replaced the "back" button** on both week screens. The old
  button sat below the fold on a two-domain week, so leaving the report *required*
  scrolling to find it — the smoke had to drag before it could tap. A label says what
  the screen does instead.
- **The re-learned lesson: trailing blank rows are not free.** The incoming `coach.png`
  carried six of them and the Guru one at the top. Left in, the descriptor's bottom edge
  is no longer the chin, so the tail hangs off a phantom shoulder six pixels low. The
  importer now trims top and bottom; the 60 px width is a frame and is never trimmed.
- **A scroll must not also be a tap.** The whole risk in a tap-anywhere page is LVGL
  following a drag with a `CLICKED`, which would throw you off the report the moment you
  tried to read the bottom of it. It does not, and `coach_week_scrolled` is now the gate
  that says so — reaching that shot at all proves the drag was not taken as a tap.

### 2026-09-20 — The lists stop looking like tables (PR #52 → `main`, merge `145bdf1`)
- **One shared `list_table_style()` + one `LV_EVENT_DRAW_TASK_ADDED` hook**
  (`list_draw_cb`) styles all five lists — To Do, Memo/Address, Guru, News feeds, the
  zone picker. One door, not five.
- **A drawn checkbox** instead of the typed `"[x]"` — hollow when open, solid black
  when ticked — painted off LVGL's own per-cell `CUSTOM_*` bits. This is what closed
  **C7** without touching the font.
- Hairline under each row instead of a box around each cell; Guru's category headings
  banded and merged across the box column; completed To Dos grey and struck through;
  To Do's row is four aligned columns (box, priority, description, due date).
- **Cheaper, not dearer:** no screen gained an object and a flag-only cell is smaller
  than the string it replaced. Smoke heap peak 2216 → 2120 B.
- **Bug: the To Do due-date picker read its date out of the wrong object.** The
  calendar's button matrix carries `LV_OBJ_FLAG_EVENT_BUBBLE`, so
  `lv_event_get_target()` returned the *matrix* and `lv_calendar_get_pressed_date()`
  cast it straight to `lv_calendar_t *`. With `LV_USE_ASSERT_OBJ` off (it is off in
  both builds) nothing caught the wrong type: tapping a day **segfaulted the host sim**
  and on-device wrote a junk date. `lv_event_get_current_target()` is the one that
  returns the calendar. The smoke now walks To Do → Edit → Due → pick → Done.

### 2026-09-18/19 — The three speakers: Coach greeting, the Guru app, lock restyle, HotSync cancel
- **`speaker_say(portrait, text, y)`** lifted out of Coach so three speakers share one
  bubble; the tail is still exactly one static I1 canvas buffer.
- **Coach greeting** on first launch after unlock, keyed off a since-unlock bitmask
  (the lock re-raises over a running app on sleep, so "first open" means *since the
  last unlock*, not since boot). Confirmed on glass by the user.
- **Launcher reordered** to nine apps in three rows with nothing below the fold, after
  two wrong turns: a fourth row for HotSync was built twice, once by shrinking every
  cell to fit it and once by hiding it behind a swipe. Graffiti moved into Games.
- **The Guru app** — `guru.c`/`guru.h` pure and clock-injected, mirroring `coach.c`;
  the local-day arithmetic came out into **`daycal.h`** rather than being copied (floor
  division, not `/`, or a 20:00 EDT record files under tomorrow). 35 habits, five
  frozen category indices, an append-only log keyed by **stable numeric ID, not list
  position**, and a daily target = rolling 7-day user average, floor 1, rounded half
  up. Two fairness rules are pinned in `guru_test.c` because an edit would break them
  silently: **today is excluded from its own average**, and **a skipped day counts as
  a zero**. The streak counts days with *any* check, deliberately not days that met
  target — a target-based streak breaks exactly when someone improves enough to raise
  their own bar.
- **The whole pool fits on one screen.** The fear was 40 rows × (row + label) ≈ 80
  objects against the pool. The list is **one `lv_table`** instead, so it costs one
  object plus its cell strings, and `smoke32` renders it pixel-identically. The
  one-category-per-page fallback is not needed and should not be built.
- **Lock screen restyle** — three declared zones (CONDITIONS / AHEAD / SUN & MOON),
  six rain bars grown to **one shared baseline** (the only rule on the screen doing
  real work: a common baseline is what lets six columns be compared at a glance),
  **zero new widget classes**. The vertical budget is `DASH_Y_*`/`DASH_H_*` constants,
  and that split is load-bearing: the furniture is painted in `dash_paint()` (must
  survive the per-tick clear), the labels on it are built in `ui_show_lock()` (must
  not) — only a shared constant makes their agreement checkable.
- **HotSync cancel** — one button, three states (Sync Now → Cancel → "Stopping…"
  disabled). The third earns its keep: a cancel can take until the end of the current
  collection, and a button still reading "Cancel" invites a second press and reads as
  broken. **`hotsync_cancel()` is a request, not a kill** — it raises a flag and
  returns, because the task can be mid-write and a half-written PDB is worse than a few
  seconds' delay. A cancelled run reads as **cancelled**, never "Done" (claims work
  never attempted) nor "failed" (sends you debugging a fine network).
- **Content pools belong in data files.** `guru_pool.txt` is the source and
  `guru_pool.c` is generated — one grammar, two parsers, diffed in CI. Edit the `.txt`.

### 2026-09-18 — Two more speakers, and the Coach grows a neck
Portraits for the Assistant and the Guru; `gen_coach_face.py` became `gen_faces.py`.
- **50% ink coverage is a recipe for solid-ink art, not outlines.** The Coach is filled
  shapes and reduces cleanly at 50%. The two new sources are thin-outline drawings: at
  15:1 a source line covers ~7% of a target pixel, so 50% erased every outline and left
  a hollow face with a broken jaw. They come down at 25% — which is why the threshold
  is an argument, not a constant.
- **The crop is the framing, not a margin trim.** Cropping to the ink bbox puts a whole
  body in 60 px and leaves a head half the Coach's size. Take a crop that frames the
  head and let the shoulders run off the sides.
- **The art is the artwork; the sources are not kept.** All three were finished by hand
  after reduction, so re-deriving one would throw away a drawn neck, a rebuilt lens
  rim, a mirrored eye. `--from-image` is the importer for the *next* portrait;
  `--from-exact` is the door back into these. A table of source paths would only
  promise a reproducibility that does not exist.
- **Features three pixels apart get redrawn, not rescued.** The Guru's pearls are light
  grey and reduce to nothing, so they are drawn back on their arc at every other pixel;
  solid, they read as a neckline rather than beads.
- **Trailing blank rows are not free.** `ui.c` hangs the bubble off the portrait's
  bottom *edge*, not its last inked row, so five transparent rows put a five-pixel
  phantom gap between shoulders and tail (and 300 bytes of nothing in flash).
- **The round trip is the safety net.** `--preview NAME f.png 1` out and
  `--from-exact NAME` back, pixel for pixel, verified after every retouch.

### 2026-08-27 — The forecast steps through the day, and the gauge stops lying under load
- **The weather was frozen at sync time.** The cache now holds **24 hourly rows**
  (`WX_HOURS`) with a per-hour WMO code, and the dashboard walks it on the 15 s tick, so
  the strip advances on the hour with no network in between.
- **Hour-of-day cannot index a 24-row cache, and `dash_test` caught it.** Matching
  `hr[i].hour24` against the current hour works for six rows and is silently wrong for
  twenty-four, because at that size every hour is present, so a clock a day and a half
  past the fetch matches a row from the wrong day and reads as current. The cache now
  carries `hr0_epoch` and the index is arithmetic, bounds-checked, `-1` past the end.
  Computing that epoch needs the **feed's** UTC offset, not the device's — read from
  the CSV's own header block; no offset means no anchor, and it refuses rather than
  guessing.
- **The gauge was reporting sag as depletion.** Reported drain was 6% per HotSync and
  ~1%/minute of use. Neither survives arithmetic: 6% of 1100 mAh is 66 mAh over a
  65–85 s radio window, which demands ~2800 mA. The cause is IR drop — a small pouch
  cell is 200–400 mΩ, idle→sync is a ~200 mA step, so ~60 mV, and near the top of the
  curve 60 mV *is* six percent. **A real sync costs nearer half a percent.**
  The reported percentage is now refreshed only from samples taken with the screen
  blanked, the radio down, and the load settled 30 s — "as of the last rest", the only
  number a voltage gauge can honestly report. The instantaneous value is still logged
  with a `load` column, and the Power screen names it ("rested 4210 mV (-58 sag)").
- **Field results:** idle on a 1100 mAh cell is **well over 24 h** (under 46 mA
  average), and clock drift on battery is **under a minute a day**. Those two numbers
  closed the RTC-for-timekeeping question and redirected the power work to screen-on
  time. See `BACKLOG.md` §Stubbed.

### 2026-08-22 — The battery gauge (U8) and the rig for the power experiment
- A cell on `JP2`, read on ADC1 ch6 (GPIO34) through the board's 2:1 divider. Three
  things stand between a raw read and an honest percentage, and skipping any one
  produces a number that looks fine and is wrong: **the 12 dB attenuator is not
  linear** (go through `esp_adc` line fitting; this part has `cali=eFuse` burnt, and
  the boot line says which is in force); **the rail is noisy** (15 samples, **median**
  not mean, then 3:1 smoothing); and **Li-ion voltage is not linear in charge** (a
  discharge curve — the cells sit near 3.8 V for most of their life, where a linear map
  renders a tenth of the pack as 1%).
- **Implausible readings report -1, not a number.** Outside 2600..4600 mV there is no
  cell (GPIO34 is input-only with no pull, so an unpopulated divider floats) and the
  dashboard says "USB". A floating pin reading "31%" is worse than admitting we can't
  tell. **A plugged-in device always reads full** — the TP4054 holds the rail and no
  `CHRG` line is broken out, so charging is indistinguishable from full.
- **Charge on the launcher title bar**, built and torn down *with* the launcher rather
  than parked on the title bar — parking cost ~1 KB of pool on *every* screen, and the
  screen that pays is the ten-field Address edit form, which `smoke32` segfaulted on.
- **The drain log (`/sdcard/power.log`).** The experiment can't be run over serial,
  because **attaching USB is what ends it**. A voltage series alone wouldn't answer it
  either, so every line carries the **residency** of its interval (`lit_s`/`dark_s`,
  banked on each backlight transition, not sampled). `power_note_sync()` is called
  *before* the radio comes up, so a sync that fails at Wi-Fi still explains its cost.
  The Power screen never invents a rate: under 10 minutes it says "measuring", and with
  no drop it names the charger instead of reporting 0%/hour.

### 2026-08-20 — The sync stops lying, and the heap is the reason nothing worked
A run of "successful" HotSyncs that set no clock, fetched no news, and blamed the
password. Two root causes, neither where the errors pointed.
- **The clock.** `clock_ok()` returned success if the year was ≥ 2024 — and the NVS
  checkpoint restored at boot is ALWAYS ≥ 2024. A device whose SNTP never answered
  reported a good sync while showing a two-day-old clock. *Plausible* and *synced* are
  now separate answers. SNTP also gets three servers, since one name that won't resolve
  was indistinguishable from a clock that was already right.
- **The heap — the real story.** Nothing was wrong with the password, feeds, parser or
  network: there was not enough RAM to complete a TLS handshake. `min_ever` bottomed out
  at **880 bytes**, then **48**. Four claims were released, costliest first:

  | where | free | min_ever | largest8 |
  |---|---|---|---|
  | first run, wifi-up | 23180 | 20460 | 21504 |
  | first run, post-tls | 22324 | **880** | 8192 |
  | first run, sync-done | 21860 | **48** | 7424 |
  | **after fixes, wifi-up** | **44888** | 28304 | 27648 |

  - **hotsync task stack 32 KB → 20 KB.** A task stack IS heap, and this one held a
    third of what was free. It could shrink because `rss.c`'s `emit_item` no longer
    parks 8.7 KB on it. The task logs its own high-water mark (11060 of 20480).
  - **The LVGL draw buffer, lent for the duration of a sync** — 19 KB DMA-capable,
    shrunk to 6 rows while `hotsync_busy()` (`BUF_ROWS_SYNC`). Safe because `flush_cb`
    is synchronous, so no flush is in flight across the swap, and a failed allocation
    keeps the buffer it has. **This is item M2, built as a shrink rather than the
    teardown that was promised.**
  - **Wi-Fi pools halved again** (static RX 4→3, dynamic RX/TX 16→8, AMPDU off). Wi-Fi
    accounted for 26 KB.
  - **The news store's file handles.** Each open file costs a 4 KB FatFs sector cache,
    so `news.idx` + `news.dat` + the spool was 12 KB held across every handshake.
    `news_suspend()`/`news_resume()` close and reopen-at-append around each fetch.
- **Messages that sent us the wrong way**, worth remembering as a class of bug:
  `login failed (HTTP 0)` invented an auth failure out of a connection failure (HTTP 0
  means no response at all); `contacts host resolve failed (HTTP 207)` called a
  Multi-Status *success* a failure.
- **`config.ini` inline comments.** `#` only started a comment at the start of a line,
  but the shipped example has six `key = value   # note` lines. The comment became part
  of the value: `timezone` matched no zone and fell back to UTC silently, and the same
  shape on `dav_pass` appended a comment to the password. A `#` after whitespace now
  ends the value; one with no space before it stays literal, so a password may contain
  it. **Timezone resolution** no longer falls back to UTC in silence and accepts what
  people type — a bare `EST` used to pass through as a POSIX TZ, which newlib reads as
  UTC: a silent five-hour error.
- **News, on first contact with live feeds.** The bold headline WRAPS and the story
  didn't know it (body pinned at y=44, room for one line). `lv_font_palm` covers
  U+0020..U+00FF, so curly quotes and dashes drew as hollow boxes — and feeds send a
  curly quote as raw `E2 80 99` far more often than as `&rsquo;`, so the entity path
  alone would have fixed almost nothing; `rss_html_to_text` needed a real UTF-8
  decoder. Caps went 30/15 → 240/40 with each item carrying a parsed publication date.
  **Undated items are KEPT** — a date format we failed to parse must never silently
  delete a whole feed.
- **Weather exists now.** Every temperature the device had ever shown was
  `dash_weather_seed_sample()`. Open-Meteo with `&format=csv`, ~1.5 KB read a line at a
  time, so no JSON parser and no new dependency. `bridge/wxfetch.c` identifies each CSV
  block by its **HEADER row, never its position**, so a reordered field cannot shift a
  column into the wrong slot; `tests/wx_test.c` runs against verbatim live responses,
  because the device cannot tell a moved column from a plausible temperature.

### 2026-08-19 — Wake straight into the lock screen, and the drift meter
- **The lock screen is raised when the screen SLEEPS, not when it wakes.** It used to
  be built on the wake tap, so the backlight came up on whatever app was open and the
  dashboard slid in a frame later. Blanking first and building behind a dark panel costs
  nothing (nobody is looking) and makes the wake instant; the wake tap refreshes it
  while still dark and defers the backlight one render pass.
- **Which meant the lock was ticking behind a dark screen** — `dash_tick` repaints the
  whole canvas every 15 s, which empties a battery quietly. It now skips while
  `power_screen_off()`.
- **Coach's exemption had to grow.** A session that ENDS face-down unseals itself and
  puts "how did it go" up, and the old code answered the next tap with the dashboard,
  whose `content_clear()` would delete that screen on the way past. The guard is now
  `co_owns_screen()`.
- **The end-of-session flash** got its own timer (1.4 s × 10 phases at 100%, ending
  early on any touch) instead of riding the 1 Hz tick for six ticks at desk brightness.
  **The flash and the idle timer read the same clock in opposite directions:** the port
  layer stands down while `ui_owns_backlight()`, but unlike the HotSync guard beside it
  must NOT call `lv_display_trigger_activity()` — that is the timer the flash reads to
  decide whether it has been noticed, and resetting it stopped every flash after one
  phase.
- **A day-old temperature is not a late number, it is the wrong one.** Everything off
  the synced snapshot is gated on `dash_weather_fresh(&wx, WX_STALE_MIN)`, 24 h. What
  stays is what is still true offline: clocks, date, agenda (the user's own records) and
  the moon, which is arithmetic. **Say which kind of empty it is** — a blank band reads
  as a fault, so it says "Weather is 3d old / hidden until the next HotSync", centred in
  the band the readings vacated. Gate: `make -C sim dash`.
- **The drift meter.** Every HotSync is a free reading of how far the clock wandered:
  `clock_sync_begin/end` logs the SNTP correction, the interval, and the implied ppm to
  `/sdcard/drift.log` (the experiment runs on battery, so a serial-only reading would
  never be read). Samples whose interval contains a power loss are reported but not
  counted — that correction is the outage, not drift.

### 2026-08-17/18/19 — Coach: the ritual focus timer, then two notes passes
The centrepiece app, and the first built from a written design spec
(`COACH_DESIGN.md`) rather than straight into `ui.c`.
- **The sigil — the stroke you draw IS the progress bar.** Captured through
  `graf_capture_hook` on pen-up, which runs *before* recognition and consumes the
  stroke, so the mark never has to resemble a letter. **80 bytes per session.**
  **The fill is a parity test, not a second buffer:** solid below the fill line, 50%
  stippled above (`if(y >= fill_y || ((x ^ y) & 1) == 0)`). "Half inked" therefore needs
  no alpha, no second canvas and no compositing. The fill advances once a minute; only
  the `MM:SS` label ticks at 1 Hz. Abandoned sessions keep their mark but render hollow,
  so the wall stays honest.
- **Sealed mode.** The running screen is a full-screen takeover on `lv_layer_top()`, so
  Home/Menu/Find/Calc are unreachable **because they are underneath, not because they
  were disabled** — there is no re-enable path to forget. Giving up costs a five-second
  hold. **The seal pays for its own buffer:** because you cannot open a game
  mid-session, the sigil canvas reuses `game_cv_buf` outright. Verified byte-for-byte
  in the smoke — the tour taps Home mid-session and the two screenshots are identical
  files.
- **`coach.c` is pure — no stdio, no LVGL, every wall-clock input injected**, which is
  what makes the advice testable on any host in any locale (68 assertions).
  `coach_advise()` runs six rules in fixed priority, first match wins, integer math.
  **R0 stays silent under five sessions**, which protects the credibility of the other
  five, and each rule in the gate is paired with a **near-miss that must NOT fire**.
  `coach_streak_now()` decays a stored streak on its own, and a **backwards clock**
  (SNTP correcting mid-week) resets it rather than inflating it.
- **Exports:** a finished session writes a Date Book block and, if you wrote one, the
  note as a Memo — both ride the next HotSync to iCloud.
- **Storage:** `coach.sav` (written on phase change — six writes per session, not per
  second), `coach.log` (12 B/session), `coach.sig` (80 B/session, index-aligned to the
  log so the wall pairs them by position with no key). Both logs are streamed on read.
  **There is no SPIFFS partition** — `partitions.csv` is nvs + phy_init + a 3 MB
  factory app, and that is all of the 4 MB. SD via FatFs, like every other app.
- **RAM, measured off the ELF, not estimated:** 226 B static, 336 B total DRAM delta,
  +12,141 B flash. Confirmed on hardware — free heap at boot went 155,412 → 155,076,
  exactly the 336 B. The spec had estimated ~148 B; the gap was widget pointers, waved
  at rather than counted.
- **Notes round 1 (on glass):** the launcher icon became a stopwatch, not an hourglass
  ("waiting" vs "working"); Marks and This-week got `back` links (both were dead ends);
  Length and Day goal stopped dismissing the menu, because they are the only rows that
  *change a value* rather than navigate; the app explains itself on the home screen
  (you were being asked to commit to something unnamed, and a session seals the
  display); **six domains, not four** — Family and Relationships joined, and because
  `domain` is a whole byte on disk, appending leaves every existing log readable.
- **The weekly report grew a coach.** The portrait stands in the right-hand margin with
  the advice in a speech bubble. **The portrait is flash, not pool** (60×65 A8 rodata,
  3,900 bytes, recoloured like the launcher icons). **The bubble is a rectangle and the
  tail is 94 bytes** — a bordered radiused `lv_obj` for the balloon, and one 24×26 I1
  canvas for the one shape that is neither rectangle nor glyph, written with **exactly
  one** `lv_obj_invalidate()`. **The balloon is sized to the worst thing the coach can
  say** — all six strings measured with `lv_text_get_size`, every one wrapping to at
  most three lines at any width from 150 to 222 px. **The coach travels with his
  bubble** (positioned from it, `face_y = bub_y - tail - gap - height`), because the
  page scrolls as one piece and a portrait pinned to the top would stretch the tail into
  a wire.

### 2026-07-24 — Zip (the fourth game) + pausable play clocks
- **The game timers pause when the game is off screen.** Mines and Sudoku each stored
  one "started at" epoch, so a puzzle resumed after lunch read 47:00. Replaced with
  `playclock.h` — bank seconds into `accum` on pause, open a fresh `run` on resume.
  Every entry point takes `now` as an argument rather than calling `time()`, which makes
  it deterministic and host-testable (25 assertions, no sleeping). Wired in at
  `kill_kb()`, the one path every screen teardown already takes. **Saves store a paused
  snapshot**, so a flat battery can't charge for time the device was off.
- **Zip** — a 6×6 one-line path puzzle (start on 1, hit the numbers in order, cover
  every cell). **The generator is inverted:** dropping N waypoints along a random
  Hamiltonian path makes *bad* puzzles (10 evenly spaced still left 2–41 solutions), so
  it numbers **every** cell and then *removes* numbers while exactly one solution
  survives — the `sudoku.c` hole-digging pattern. Minimal boards of 5–12 numbers in
  **2.4 ms**. Affordable because of the **connectivity prune**: every unvisited cell must
  still be reachable through unvisited cells, which collapses an exhaustive count on 36
  cells to a few thousand nodes. **Drag input, deliberately forgiving** — `PRESSED`
  starts, `PRESSING` continues (never `CLICKED` — the Mines lesson), retrace to rewind,
  and **bridge through a shared free neighbour** when the touch lands two cells away,
  which is what makes a fast diagonal fingertip sweep work on a resistive panel.
- **One canvas buffer for all four games — 12.5 KB of BSS back.** The game screens are
  mutually exclusive, so four private I1 buffers were 17.5 KB of waste. They now share
  one `game_cv_buf` sized to the largest board; Mines and Wordie just use less of it.

### 2026-07 — The RSS reader, in four stages
- **A — the parser.** `bridge/rss.c`, a streaming RSS 2.0 / Atom parser: a byte-driven
  state machine accumulates only the current item into a bounded buffer, then extracts
  the title and the richest body. `rss_html_to_text` strips tags and decodes entities in
  one pass, crucially treating `&lt;`/`&gt;` as tag delimiters so it handles both CDATA
  raw-HTML and the entity-escaped HTML that `<description>` usually carries.
  Host-gated, plus a sanitized `rss_asan` in `ftest` — it eats untrusted network bytes.
- **B — the reader app.** `bridge/news.c` is an on-SD store (`news.idx` fixed records +
  `news.dat` bodies concatenated), so browsing is O(1) RAM regardless of store size. The
  **News** app shows one article per screen and navigates by **vertical swipe**,
  detected manually from press-Y vs last-PRESSING-Y — the headless host doesn't
  reliably synthesize LVGL's velocity-based gesture, and neither does a real resistive
  panel.
- **C — the HotSync fetch.** `dav_fetch_url()` streams a public HTTPS body straight to
  SD, reusing the existing `esp_http_client`/mbedTLS handle; `fetch_news()` runs after
  the PIM sync while Wi-Fi is still up. Bounded RAM throughout. **Only the live network
  GET is unexercised off-glass.**
- **D — feed management.** `bridge/feeds.c` keeps the source list on SD (`feeds.txt`,
  ~4 KB fixed table, no heap) with **10 reputable world-English feeds** pre-seeded.
  **Preferences > News feeds** is a pool-safe `lv_table` with a checkbox column and a
  URL editor on the tap keyboard. *Replaces the earlier `config.ini news_feed1..3`.*
- **E — the sim HotSync populates News**, so the whole loop (add a feed → HotSync →
  swipe the reader) demos in the browser. The sim has no network, so it rebuilds the
  store from the enabled feeds with sample items.

### 2026-07 — Graffiti: accuracy harness, template fixes, and the SRS trainer
- **An offline accuracy harness as a CI gate** (`sim/tests/graf_test.c`). It synthesizes
  hand-drawn-like strokes from each template (densified polyline + deterministic
  Gaussian jitter at a realistic pixel scale), runs them through the real recognizer,
  and reports per-glyph accuracy + top confusions across **all three sets** — letters,
  digits and punctuation (the two-step punct-shift arm is simulated).
- **Template separation guided by the harness**, letters **97.5% → 99.7%** mean at 3 px
  jitter, no glyph below 92%; digits and punctuation 100%. Then a second pass from
  on-glass feedback: **`G`** went from an inward-crossbar capital (which stayed
  loop-like and read as `O` on-device) to a wide-open C with a full-width mid-bar;
  **`S`** to a proportional two-lobe that survives a fast hand; **`X`** to the real
  single continuous stroke (first diagonal, a bridge up the right edge, then the
  second) rather than a two-stroke cross; **`?`** gained the straight downward tail the
  stroke naturally ends on, without which a natural flick read as `)`. The trainer's
  guides draw straight from these templates, so they updated for free.
- **The Graffiti trainer app** — two modes. **Drill** shows a target glyph and its
  stroke guide (drawn on an I1 canvas from the recognizer's own template, start dot for
  direction), scored by the real recognizer with a graded % from the $1 match distance.
  The schedule is a **deterministic, never-random SRS**: level 1–5 and a due tick;
  correct promotes and past level 5 **burns**; wrong demotes and reschedules
  immediately; the next glyph is always the non-burned one with the smallest due tick.
  **Train** records *your own* stroke as a **per-device template** (~3.3 KB, persisted,
  loaded at boot), and recognition prefers it when it is a closer match — calibrating to
  this hand and this resistive panel.

### 2026-07 — The Japanese trainer (frozen at Tier 2)
- **Tier 1 (kana → sound):** the `Kana` app shows a kana in the `lv_font_kana` bitmap
  subset; you answer the SOUND by drawing romaji in the Graffiti strip (the Latin
  recognizer is untouched). Deterministic SRS per kana.
- **Tier 2 (write the kana):** a Sound/Write toggle; Write shows the numbered KanjiVG
  stroke model and matches each drawn stroke against the expected *next* one, enforcing
  official order. Stroke data ~28 KB from `tools/gen_kana_strokes.py` (CC BY-SA).
  Emulator-verified end to end. **Tiers 3–5 are not planned** — see `PRODUCT_PLAN.md`.

### 2026-07 — The simulator, the review cycle, and product hygiene
- **The simulator (S0–S3).** The real `firmware/main/ui.c` builds to a **native
  headless** host (scripted input + PPM/PNG screenshots, a CI smoke gate) and to
  **WASM** for the browser, live on GitHub Pages. A device-like general-heap ceiling
  (linker-wrapped malloc) + IDBFS `/sdcard` persistence + credential scrubbing.
- **The charm/intuitiveness batch:** C1 Graffiti ink trail + char echo, C2 HotSync
  dialog, the full C4 form contract (Done/Details/Delete bottom bar, Edit Categories,
  Address 10 fields, event Alarm/Repeat), C5 devtools gating, C6 About honesty (the
  Memo device-only / To Do CalDAV-not-Reminders truths), **C7 inverted white-on-black
  Palm title bar**, I1.1 onboarding hint, I1.2 on-screen keyboard, I2 manifest-tracked
  demo seed + "Remove demo data" (so a first HotSync can't push seeds to real iCloud),
  I3 Date Book **Week view** + Day/Week/Month zoom, I4 save/delete/config toasts.
- **The brightness freeze.** The Preferences brightness row was an `lv_slider` — an
  `lv_bar` — a draw-layer alloc that live-locks the pool and fires the WDT. Replaced
  with a pool-safe `[-] NN% [+]` stepper.
- **Product hygiene:** the GPLv3/MIT split + NOTICE (PumpkinOS provenance, Palm
  trademark), CI (host codec/fuzz gates, Radicale sync gates, `idf.py build`, then the
  simulator smoke + wasm + Pages deploy), a newcomer-facing README, and **M1** — sync
  scratch buffers BSS → sync-lifetime heap, returning ~20 KB to interactive mode.

### 2026-07-05 → 2026-07-15 — Sync correctness, then the whole PDA
- **Sync is correct end-to-end on device.** Map frozen by FATFS `rename` (→ `remove` +
  `rename`); push keeps/maps only on 2xx; 412 duplicate-UID conflicts resolved; and the
  OOM-wipes-DateBook data-loss guard (`sync_collection` refuses to overwrite a non-empty
  local with an empty remote).
- **UID-based identity** (iCal/vCard UID hash, not href-derived) killed the
  relocation/dup-UID class. **Streaming reconcile** — a disk-backed 3-way merge-join,
  O(1) RAM, `MAXR` cap gone.
- **Drift self-heal.** Incremental sync could orphan a record whose first pull failed
  (the RFC 6578 sync-token advanced past it). The device now **always full-enumerates
  and never persists a sync-token**; the host keeps the incremental fast path.
- **Streaming enumeration.** An 8 KB response buffer truncated a ~42 KB REPORT and the
  collection was skipped. `dav_sync_report`/`dav_list` now spool to SD and
  sliding-window parse. **TLS keep-alive** — one connection per origin, since the
  handshake was per-request (~2N handshakes to pull N records).
- **iCloud modern Reminders are walled off from CalDAV** (Apple, since iOS 13) and this
  is unfixable by any CalDAV client. **Decision:** To Do stays on the iCloud CalDAV
  VTODO lane, still cloud-backed; view it on iPhone by adding iCloud as an external
  CalDAV account.
- **On-device `config.ini`** — runtime load, a Preferences editor, and Discover (walk
  the CalDAV + CardDAV homes, assign collections to roles).
- **The PalmOS UI, built step by step on hardware:** display (ILI9341 portrait), touch
  (XPT2046, calibrated, NVS), the LVGL app shell + Palm fonts/icons/theme, data views,
  detail + edit, menus/categories/Details, **Graffiti** (full a–z/0–9 + punctuation
  shift), **HotSync**, Calculator, Find, the top-bar clock, a TZ picker + DST, and
  `lv_table` virtualization. **U0** — moving the sync working set from static BSS to
  heap — was the prerequisite that freed the RAM for all of it.

---

## Hardware + RAM reference

The load-bearing facts, salvaged from the retired design analyses (`UI_ROADMAP.md`,
`ROADMAP.md`, `SIMULATOR_PLAN.md`, `REVIEW_2026-07-15.md` — in git history via
`git log --diff-filter=D -- docs/`). These are the things you cannot re-derive from the
code.

### The board (this CYD, ESP32-2432S028R)
- **ESP32-D0WD-V3**, 520 KB SRAM (~320 KB DRAM for data), **4 MB flash / 3 MB app
  partition**, **no PSRAM**. Measured free heap: **129 KB at boot**.
- **Display** ILI9341 (some units ST7789), **portrait 240×320**, SPI3 @ **20 MHz** —
  pins route via the GPIO matrix on SPI3, cap ~26.7 MHz, and **40 MHz crashes init**.
  SCLK 14, MOSI 13, MISO 12, DC 2, CS 15, BL 21, MADCTL 0x48. Layout: PDA area 240×208
  top, Graffiti strip 240×112 bottom.
- **Touch** XPT2046 resistive, single-touch, **bit-banged on its own pins** — CLK 25,
  MOSI 32, MISO 39, CS 33; **IRQ 36 unused, detect via pressure z1**. 3-point affine
  calibration (axis swap/flip/skew in one solve), stored in NVS, re-cal by holding at
  boot. **Pressure threshold 110** — resistive panels read weak near the edges, and a
  300 threshold created a right-edge dead zone.
- **SD** is a THIRD SPI arrangement — SCLK 18, MOSI 23, MISO 19, CS 5. TFT, touch and
  SD don't contend.
- **Battery**: JST header + 2:1 divider on **GPIO34**. The TP4054's `CHRG` status pin
  is **not broken out**, so charging cannot be distinguished from full.
- **No battery-backed RTC**, and **no 32.768 kHz crystal either** — which is the part
  people miss. The WROOM-32 leaves those pins as **GPIO32/33**, and our touch panel uses
  both, so the module demonstrably has no 32K fitted. RTC_SLOW_CLK is therefore the
  internal ~150 kHz RC, calibrated at boot but temperature-dependent. While *awake* the
  clock runs off the 40 MHz XTAL and is fine. **Measured drift on battery: under a
  minute a day** (2026-08-27), which is why no RTC part is needed for timekeeping.
- **`CONFIG_PM_ENABLE` and tickless idle are commented out** in `sdkconfig.defaults`:
  automatic light-sleep gated APB between LVGL frames and the display flashed. So
  "asleep" means a full-speed SoC in the dark — accurate clock, thirsty. Note that what
  failed was *automatic light-sleep*; pure frequency scaling has never been tried.
- **The board is the E32R28T** (LCDWIKI, ESP32-WROOM-32E + ILI9341 + XPT2046); its IO
  table in `5_Schematic/` matches our pin map exactly, so it is authoritative. **`IO22`
  is the red RGB LED and is not brought out at all.** The three 4-pin 1.25 mm seats:

  | seat | pin 1 | pin 2 | pin 3 | pin 4 |
  |---|---|---|---|---|
  | **P2** Serial | +5V | GND | TXD0 (IO1) | RXD0 (IO3) |
  | **P3** SPI Peripheral | MOSI (IO23) | MISO (IO19) | CLK (IO18) | **CS (IO27)** |
  | **P4** Expand Pin | VCC3V3 | **IO35** | *(n/c)* | GND |

  **The pin census forces most hardware answers.** Reachable without soldering: `IO27`
  (the only free *bidirectional* GPIO), `IO35` (**input only**, but `RTC_GPIO5` and a
  valid `ext0` wake source), `IO1`/`IO3` (UART0), and the SD bus. **I2C needs two
  bidirectional lines and there is exactly one**, so an I2C part must take a UART0 pin —
  and `RXD0` is driven by the CH340 whenever USB is connected while `TXD0` carries the
  ROM bootloader's output. **I2C is off the table on this board without soldering.**
  `IO34`–`IO39` have no internal pull-ups.

### Why it fits: two mutually exclusive modes
The rule the whole architecture rests on: **never hold LVGL's draw buffers and the TLS
handshake at the same time.**

| | Mode A — interactive (Wi-Fi OFF) | Mode B — sync (Wi-Fi + TLS) |
|---|---|---|
| IDF + FreeRTOS baseline + stacks | ~55 KB | ~55 KB |
| LVGL partial draw buffer | ~19 KB | **~3 KB** — shrunk to 6 rows for the sync |
| LVGL core + view tree | ~30 KB | ~30 KB — still resident |
| Wi-Fi driver + lwIP | (deinit'd) | ~50 KB |
| mbedTLS handshake peak | — | ~40 KB |
| Sync working set (heap, freed after) | — | ~23.5 KB |
| **Free** | **~160 KB** (est.) | **44.9 KB measured at `wifi-up`** |

**Read the Mode B column carefully — earlier versions of this table were wrong twice.**
First it claimed the draw buffer was two allocations totalling ~40 KB; it is a single
`heap_caps_malloc(240 × 40 × 2, DMA)` with `NULL` as the second buffer. Then it showed
LVGL as "(torn down)", describing a contingency at `hotsync.c:8` that had never been
implemented — so Mode B was really ~20 KB, not the ~70 KB claimed, and **the 78 KB
figure that seemed to confirm ~70 KB was measured headless**, validating only the no-UI
case. The 2026-08-20 work then built the teardown as a **shrink** and released three
other claims, taking measured free at `wifi-up` to **44.9 KB**.

**What is still unmeasured: the handshake peak with the UI resident.** Read it as the
drop in `min_ever` between `pre-tls` and `post-tls` in `hs_heap()` — but `min_ever` is
since *boot*, so an unmoved value means the run was inconclusive, not that the handshake
was cheap.

**Practical consequence: be sceptical of anything that adds to Mode B.** A
full-framebuffer double-buffered GUI would need ~300 KB of buffers; we render in partial
strips instead.

`sim/sim_heap.h` still budgets from the old ~140 KB figure. **Leave it** — the simulator
being *stricter* than the device is the safe direction for a guard.

---

## Hard-won lessons — re-read before touching these areas

### LVGL on a 32 KB object pool
*(`CONFIG_LV_MEM_SIZE_KILOBYTES=32`, measured on hardware as `lvgl pool: 31100 bytes
total`. `make -C sim poolparity` fails if `sim/lv_conf.h` and the device's sdkconfig
drift apart — the gate is the authority, not prose.)*

- **Never use a widget that allocates a draw LAYER.** `lv_bar` (and its subclass
  `lv_slider`), `lv_arc` and `lv_meter` composite their indicator through a layer
  buffer from the fixed pool. The alloc fails, LVGL retries every refresh, IDLE0
  starves, the Task WDT fires → **frozen screen**. This bit us three times: the sync
  progress bar at 66%, a To Do inline `lv_checkbox` behind the menu overlay, and the
  Preferences brightness slider. **Use plain buttons / button-matrix / lists / labels.**
  Progress and steppers are **text**, not bars.
- **`bg_opa`/`bg_color` does NOT force a layer.** Only object `opa`, transforms, or
  blend modes do. `LV_OPA_30` backdrops and a tinted "today" row are safe.
- **A picture belongs in flash, not in the pool.** Anything that never changes: emit it
  as a const `lv_image_dsc_t` (A8, `image_recolor` to the ink colour) the way
  `palm_icons.c` does, and `lv_image` draws it straight from rodata. Coach's face costs
  3,900 bytes of a 3 MB partition and zero pool. Reach for a canvas only for shapes that
  are actually computed — the bubble tail, 94 bytes.
- **You can draw into a cell without allocating anything.** A `LV_EVENT_DRAW_TASK_ADDED`
  hook plus LVGL's per-cell `CUSTOM_*` flag bits paints checkboxes, hairlines and
  strike-through on a plain `lv_table` for free — a flag-only cell is *smaller* than the
  `"[x]"` string it replaces. This is what closed C7 after it had sat blocked behind a
  font regeneration for months.
- **`lv_canvas` is an `lv_image`, so shrinking the OBJECT centre-crops the buffer** —
  not "clips from the top", it takes a band off **both** ends. Coach drew its sigil into
  a 240×164 buffer and called `lv_obj_set_height()` to fit the control, which quietly ate
  ~23 px off the top and bottom of every mark. Hand the height you want to
  **`lv_canvas_set_buffer()`** and never resize the object.
- **LVGL 9.2 `lv_table` tap** clears the selected cell on RELEASED *before*
  `LV_EVENT_CLICKED` fires — read the cell on `LV_EVENT_VALUE_CHANGED` instead.
- **`lv_event_get_target()` is not the widget when events bubble.** A calendar's button
  matrix carries `LV_OBJ_FLAG_EVENT_BUBBLE`, so `get_target()` returns the *matrix* and
  casting it to `lv_calendar_t *` is a silent wrong-type read with `LV_USE_ASSERT_OBJ`
  off (it is off in both builds). **Use `lv_event_get_current_target()`.**
- **Size a fixed container to the WORST string it can hold, and measure it** with
  `lv_text_get_size()` against the real font, over every string the code can produce.
  Guessing yields either a clipped balloon or one that resizes per verdict.
- **`lv_layer_top()` inherits NOTHING from the screen** — not the font, not
  anything. Set what it needs on the layer itself (the Palm font is set there
  in `ui_init()`), or every modal silently falls back to `LV_FONT_DEFAULT`.
- **Arithmetic on `lv_pct()` moves the percentage.** `lv_pct(100) - 20` is 80 %.
  Compute pixels from the constants you built the neighbours from.
- **Scroll the page, not a panel inside it.** A scrolling sub-panel with fixed furniture
  around it puts a scrollbar down the middle of a 240 px screen. Put the whole screen on
  one scrollable child of `content` (never on `content` itself — it is shared and
  `content_clear()` does not reset its flags). Anything anchored to a scrolling page
  must be positioned from the flow: `LV_ALIGN_BOTTOM_LEFT` pins to the *viewport*, so a
  `back` button placed that way scrolls away from its own page.
- **Keep big buffers off the task stack.** A per-record 8 KB stack frame overflowed the
  hotsync task and corrupted the heap.

### The simulator, and gates that pass while the picture is wrong
- **The smoke script navigates by absolute pixel coordinates, and it will NOT tell you
  when they go stale.** The gate asserts **exit code + "screenshots exist"**, never
  pixels. When Coach became the 9th launcher app, row 3 went from two icons to three,
  `SPACE_EVENLY` re-centred it, and nine taps meant for Games opened Coach — a green run
  touring the wrong app through nine sections. Inserting "Power" into a menu shifted
  every tap below it, so an About tap landed on "Remove demo data" and deleted the seed
  the rest of the tour depended on; still green. **After changing `APPS[]`, `GAMES[]` or
  any menu, re-derive the affected coordinates and eyeball the shots.** Grid centres are
  computable: content 240 wide, `pad_all 6`, 68 px cells, `SPACE_EVENLY` → columns at
  **x = 46 / 120 / 194**, rows at **y = 56 / 108 / 160**. Measure off a screenshot
  rather than guessing.
- **The same trap fires *inside* a screen.** Making the weekly report scroll pushed its
  `back` button below the fold, so a fixed tap landed on the speech bubble and the
  screenshot showed the wrong screen. If a change can move a control off-frame, the
  script has to scroll to it and take a shot proving it got there.
- **A green emulator does not mean the firmware builds.** The simulator is LVGL 9.2.2
  and the firmware is 9.5 — an **API split**, not just a version number
  (`lv_table_add_cell_ctrl` → `lv_table_set_cell_ctrl`, for which `ui.c` carries a
  shim). **Run `idf.py build` before believing any UI change.**
- **Pool scaling is pointer-width sensitive.** The 64-bit native sim needs ~2× the pool
  for the same object capacity, so a screen with too many widgets can pass the native
  smoke and still fail on wasm/device. `make -C sim smoke32` is the guard (**named for
  32-*bit*, not 32 KB**) and runs in CI; it needs `gcc-multilib libc6-dev-i386`.
  **Trust wasm over the native host** for memory-limit reproduction.
- **`smoke32` leaves 32-bit objects behind.** It runs `make clean` and rebuilds
  everything `-m32`, so the next plain `make -C sim host`/`smoke` fails to link against
  the 32-bit `liblvgl.a`. **Always `make -C sim clean` after a `smoke32` run.**
- **The sim's `/sdcard` persists between runs.** The smoke appends to real files, so a
  second run starts with the first run's data and screenshots stop matching what the
  script just did. CI is always fresh; locally, wipe the SD root before trusting a
  screenshot.
- **A gate that CI does not name does not run.** `sim/Makefile` has a `games` aggregate,
  but `ci.yml` lists each logic gate as its own step — so adding `coach` to `games` was
  not enough and its 68 assertions never executed. **Add the step to `ci.yml` too.**
- **A board seeded from `time()` differs every run**, so Mines/Sudoku/Zip screenshots
  are not byte-comparable by design. Assert on behaviour, not exact pixels.
- **General malloc is capped too** (`sim_heap.h` + `heap_budget.c`, linker-wrapped) so
  the browser's unlimited memory doesn't mask the device budget.
- **Credentials are never persisted to browser storage** — `sim_scrub_config()` blanks
  the password fields before every IDBFS write.

### Time, clocks and timers
- **A game clock stored as one "started at" epoch keeps counting while the game is
  closed.** Bank the elapsed segment on teardown and open a new one on open
  (`playclock.h`); `kill_kb()` is the reliable "you left" hook, because every screen
  teardown already calls it.
- **Persist a paused snapshot, never a live epoch.** A stored `run` timestamp is stale
  the moment the device powers off, and resuming from it charges the player for the
  downtime.
- **Make the clock injectable.** Passing `now` into every call instead of calling
  `time()` turned an untestable behaviour into a 25-assertion host gate that needs no
  sleeping. Anything time-dependent in this codebase should follow that shape.
- **The sim's `t <ms>` is simulated time** — it moves LVGL's tick only, so `time(NULL)`
  does not budge and timer behaviour is invisible under it. `w <ms>` is a real
  wall-clock wait, and it costs real CI seconds; use it sparingly.
- **Local-day arithmetic floors, it does not truncate.** `a / b` toward zero puts a
  22:00 EDT record on the wrong local day. `daycal.h` is the one implementation; a
  second copy would drift.

### Generating puzzles with a unique solution
- **Start from a fully-constrained board and remove** while a solution counter capped at
  2 still says "1". Placing a fixed number of clues and hoping for uniqueness does not
  work — 10 evenly spaced Zip waypoints still left 2–41 solutions. Both `sudoku.c` and
  `zip.c` land on this same counter-intuitive shape.
- **A strong prune makes exhaustive counting cheap enough to run per removal.**
- **Always give a search a node budget.** Exhausting it means "not proven unique", which
  the generator can act on safely — far better than a watchdog reset on an unlucky seed.
- **Keep recursion frames small:** static scratch and a shared neighbour table instead
  of per-frame arrays (`-fstack-usage` is how you check). LVGL runs on a 12 KB stack.

### The no-PSRAM RAM fight (don't undo `sdkconfig.defaults` / `sync.c` sizing)
- What makes TLS + Wi-Fi + LVGL + the sync working set coexist: the capped LVGL pool,
  `MBEDTLS_DYNAMIC_BUFFER` + free-CA-after-handshake, trimmed Wi-Fi buffers, and a small
  sync set (`MAXR=24`, `ARENA_CAP=8 KB` — `S` + `Out` + `nodes[]` + `Sink` all scale with
  `MAXR` and must coexist with the handshake).
- **Time-multiplex, don't sum.** Rich UI and TLS never run at the same instant. Wi-Fi is
  down during interactive use; the screen shows only a status line during a sync.
  `dav_disconnect()` frees the TLS working set before every heap-heavy sort.
- **Stream everything.** PDB I/O is one record at a time; DAV enumeration and reconcile
  are disk-backed sliding-window / merge-join. Watch the **window-boundary bug**: a
  `<response>` straddling the parse window must keep its tail, or a record is dropped.
- **A task stack IS heap.** The hotsync task held a third of the free heap at 32 KB.

### Sync / iCloud specifics
- **iCloud CardDAV is on a different host** than CalDAV (`contacts.icloud.com`, not
  `caldav.icloud.com`) — contact discovery must start there.
- **iCloud namespaces every XML element** `xmlns="DAV:"`; the parser is prefix-tolerant.
- Auth is an **app-specific password** (with dashes). Never commit, echo or log the
  password fields — `secrets.h` is gitignored and `appcfg.h` marks Config SENSITIVE.
- **No RTC** → **SNTP on Wi-Fi connect** before any HTTPS, or TLS cert validity fails.
- **`data_delete()` returns 1 on success** (`rewrite() >= 0`), not 0 — batch deletes
  must count `>= 1`, and per-record rewrites are O(n²) SD churn, so batch them.
- **Distinguish "no response" from "refused".** `login failed (HTTP 0)` invented an auth
  failure out of a connection failure, and a 207 Multi-Status was reported as an error.
  Both sent us debugging the wrong subsystem for a day.

---

## Resume / reference

```
# device
. ~/esp/esp-idf/export.sh && cd firmware && idf.py -p /dev/ttyUSB0 flash monitor
# secrets in firmware/main/secrets.h (GITIGNORED: Wi-Fi + Apple app password)

# host gates
make test                                        # codec / find / calc / config / streamparse
make ftest                                       # fuzz (ASan + UBSan)
./tests/run_gates.sh                             # full suite from clean (Radicale)

# simulator
make -C sim smoke       # native headless + screenshots (CI gate)
make -C sim smoke32     # the SAME tour at 32-bit pointer width -- then `clean`!
make -C sim poolparity  # sim/lv_conf.h vs the device's sdkconfig
make -C sim games       # every logic gate: poolparity mines wordie sudoku zip
                        #                   clock coach guru gurupool dash
make -C sim graf        # Graffiti recognizer accuracy gate
make -C sim wasm        # browser build
make -C sim clean       # REQUIRED after smoke32 (it leaves 32-bit objects behind)
```

- **Graffiti stroke chart** (rendered from the templates):
  https://claude.ai/code/artifact/d8042fdf-06e2-4cb2-8b8b-1fea75fa45e9
- **Asset converters:** `scratchpad/{palmfont.py, palmicon.py}`; `tools/gen_faces.py`
  (portraits), `tools/gen_kana_strokes.py`; PumpkinOS clone at `scratchpad/PumpkinOS`.
- **The web emulator** deploys from CI, `main` only:
  https://emu-commits.github.io/cyd-palm-bridge/
