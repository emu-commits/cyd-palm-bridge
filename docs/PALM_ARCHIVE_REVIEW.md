# What the Palm OS developer archive can teach CYD Palm

_Written 2026-10-15. Source: [github.com/ardiri/dev-palmos](https://github.com/ardiri/dev-palmos)
(commit `a4fb37d`, cloned into a scratch directory, nothing copied into this repo). Line numbers are as of `5150be6`._

> **Status (2026-10-15):** everything in §3 is built, on branch
> `claude/palm-behaviours` (one commit each, P1 to P8 in `BACKLOG.md`). The
> bench checks are that list's `[d]` items. The speaker connector (§3.6) is
> one of them.

**In short.** The archive is a 1.7 GB historical collection: Palm OS SDKs (3.0
to 5 R3), the 3.5/4.0/5.0 reference PDFs, conference slides from 1999 to 2004,
"limited release" OS source for 3.0, 3.5 and 4.0, a few desktop utilities and
68K/display datasheets. Nearly all of it targets a platform we only imitate,
so it gives us no code. What it does give us is the two design books Palm
handed developers: the **Palm OS User Interface Guidelines** (2001) and the
**Zen of Palm** (2001), in `palmos-doc/!ui_guidelines/` and
`palmos-doc/!zen_of_palm/`. This review measures the firmware against them.

We already follow most of the philosophy. What remains is a small number of
**behaviours** that make a Palm *feel* like a Palm. The biggest one is cheap,
and right now we get it backwards: **you come back to where you were.**

---

## 1. What we may take, and what we may not

| Material | Use |
|---|---|
| UI Guidelines and Zen of Palm (©2001 Palm, Inc.) | **Ideas and rules only.** We may cite them and reimplement behaviour, but must not copy text or figures into the repo. |
| `!os_source (limited)/*.zip` (e.g. `UI/Src/Find.c`, "Copyright (c) 1994-2000 Palm, Inc. All rights reserved.") | **Do not read for implementation, and copy nothing.** The archive's own README says inclusion grants no rights. Our implementations stay clean-room, from the published guidelines. |
| SDK zips, reference PDFs | Read-only reference for how a behaviour worked (Attention Manager, Find, menu shortcuts). |
| Hardware datasheets (MC68328, SED1375…), `utilities/` | Not relevant to an ESP32. |
| `palmos-doc/arm-writing_efficient_c.pdf` | Generic ARM advice, mostly superseded by what GCC does for the Xtensa. Skip. |

**For the Etsy plan** (`PRODUCT_PLAN.md`): the archive's README also reminds us
that the trademarks still have owners. "Palm", "Graffiti" and "HotSync" are
trademarks, and so is the archive's Palm OS logo. Nothing here changes what
we may ship, but take that into account in the listing copy and on the box.

---

## 2. Where we already match Palm

These need no work. They're listed so they aren't "fixed" by accident.

- **Perceived speed and the 80/20 rule.** The lock screen's AHEAD zone answers
  "what's on today" in zero taps. The Zen book's frequency table puts "check
  today's schedule" at one tap; we beat it.
- **Sharing the work with a companion computer** (Zen, "Let a companion PC
  application do the heavy work"). This is exactly `bridge/`, `tools/mkcourse.py`
  and the calendar window: the computer builds courses and the server keeps
  the full calendar.
- **Date Book opens on today** (`show_app`, `ui.c:2273`), and New uses the day
  you're looking at.
- **Delete is hidden in the menu and confirmed** (`ask_delete`, `ui.c:1068`).
  New is a visible button. Palm called this "stapler on the desk, staple
  remover in the drawer".
- **Address Look Up** in the list's top bar, and quick-add bars for To Do and
  Memo. These are Palm's "write in place" behaviour.
- **Categories with a protected Unfiled**, a title-bar category pop-up, and a
  per-record Details sheet.
- **A Graffiti shift indicator** for case and punctuation (`ui.c:6735`),
  including a way out of the punctuation shift.
- **Global Find** across all four databases (`show_find`, `ui.c:5808`).
- **Progress as text, not a bar** (HotSync). Palm's progress dialog was a
  message that changed, too.
- **Mono, black on white, and colour never the only cue.** This is Palm's
  chapter 8 rule, and the product plan's.

---

## 3. Recommendations, most valuable first

Each item gives the Palm rule, what we do now, the change, and its cost
against the constraints in `CLAUDE.md` (no PSRAM, a 32 KB LVGL pool, about
19.9 KB of static DRAM free).

### 3.1 Come back to where you were, and never lose a half-written record — **do this first**

**Palm:** *"Most applications should return to the last form that was being
displayed"* (Ten Things #5). *"If the user has been editing data, save the data
before the user exits the form… perform the least destructive operation"*
(Fitting In, "Exiting the Application"). The Memo Pad reopens on the memo you
were reading. The guide asks for a minimum of one character before a record
is worth saving.

**Now:** when the screen sleeps, `lock` calls `content_clear()`
(`ui.c:7600`) to keep the pool small. Unlocking then sees an empty content
area and builds the **launcher** (`lock_release_cb`, `ui.c:7408`). So:
- every wake lands on the launcher, wherever you were;
- **an edit form open when the screen times out is thrown away.** The same
  happens on Home (`home_cb`, `ui.c:224`). Neither path saves the form;
  only Done does.

The comment in `lock_release_cb` ("Later wakes re-lock over whatever app is
showing") predates the `content_clear()` and is now wrong.

**Change:**
1. Before the lock's `content_clear()`, and in `home_cb`, if an edit form is
   open and has at least one character, save it as Done would. The edit
   form's Cancel stays the one explicit way to discard.
2. Keep a small resume record: which screen (Day view and its date, an app
   list and its category, a record's detail by uid, Study's dashboard). This
   is about 16 bytes of static RAM. On unlock, rebuild that screen in place
   of the launcher.
3. The pool budget is unchanged. The lock still frees the app, and the
   rebuild happens after the dashboard is deleted, exactly as the launcher's
   lazy build does now.

**Fit:** small, entirely in `ui.c`. The smoke tour needs a step (open a
memo, write, force a lock, unlock) and a screenshot showing the memo again.
Bench: `[d]`, an edit survives auto-off on the device.

### 3.2 Date Book: start writing to make an event

**Palm:** *"Just write a 3 to create an event that starts at 3:00"* (Zen,
"Include power features discreetly"). A letter written first makes an untimed
event. The New button stays for people who don't know this.

**Now:** in the Day view no field has focus, so a stroke on the Graffiti pad
is recognised and **dropped** (`ui.c:6800`). The pad is always on screen
there, inviting exactly this gesture.

**Change:** in the Day view, the first recognised character opens New event
for the day shown. A digit sets the start hour; Palm read it within the
working day, so 3 means 3:00 pm. A letter becomes the first character of the
description. This mirrors `quick_add_cb` (`ui.c:968`) for To Do and Memo.

**Fit:** a few lines in `ui.c`, with no new state. The smoke tour can drive
it through the existing typing hook.

### 3.3 The menu: open it by tapping the title, and remember the last item

**Palm (3.5 and later):** tapping the form's title opens the menu bar. The
Zen book holds this up as the model of a feature that costs nothing and helps
experienced users. The menu also reopens on the **last item chosen**, for as
long as that form is up.

**Now:** the menu opens only from the silkscreen Menu button, or the
title-bar Menu in full screen (`menu_open`, `ui.c:5672`).

**Change:** make the title label clickable and send it to `menu_open()`.
Keep one static pointer to the last action and highlight it when the menu
reopens; clear it in `content_clear()`.

**Fit:** trivial.

### 3.4 Alert and button conventions

These are small copy and layout fixes that add up to "it feels right".

- **The affirmative button goes on the left.** Palm's alerts put the verb
  first ("Delete", then "Cancel"), and every button row is left-aligned.
  `ask_delete` has Cancel on the left and Delete on the right
  (`ui.c:1115–1121`). Swap them. The backdrop tap stays Cancel: it's the
  least destructive default, as Palm requires.
- **Buttons repeat the verb of the question.** We already do this
  ("Delete this memo?" with Delete and Cancel). Keep it as the rule for new
  alerts, and give each alert a title that names the operation ("Delete
  Memo").
- **Never dim a button; tap it and be told why.** *"Never dim or gray out a
  button… display an alert dialog that explains why the button does not
  apply."* Study disables Lessons and Review when there are none
  (`ui.c:11707–11708`). Leave them live, and have a tap show the reason in
  her speech bubble ("No reviews are due; the next is at 6 pm"). This fits our
  rule that copy explains the app. HotSync's disabled button while stopping
  (`ui.c:1827`) is a progress state, which is a fair exception.
- **The edit form's buttons.** Palm's are Done, Details… and Note, with no
  Cancel, because leaving already saves. Once 3.1 lands, Cancel could become
  a menu item ("Discard Changes"), which frees the third button. Low
  priority; take it only if 3.1 ships.

**Fit:** copy and alignment only. **Check the smoke tour's absolute tap
coordinates** for any row that moves.

### 3.5 Find: match the start of words, not anywhere

**Palm:** *"a prefix-only case-insensitive search on each word… 'the' finds
'Then' and 'There', but not 'Lathe'."* The guide also says not to "improve" on
this, because users know the built-in behaviour.

**Now:** `ci_find_n` (`bridge/find.c:20`) is a plain substring match, so
"the" also finds "Lathe". On a growing iCloud account that means more noise
in a 60-row result cap.

**Change:** accept a match only at offset 0 or after a non-alphanumeric
byte, and extend `tests/find_test.c` with the Lathe case. Because the change
is in `bridge/`, the full gates apply.

**Fit:** a few lines, host-tested.

### 3.6 Reminders that wait for you: Palm's Attention Manager

**Palm 4.0:** past-due reminders collect in **one list**, shown once and
dismissed together, instead of a pile of alerts after a day switched off. The
list must be dismissed explicitly; a hard key doesn't clear it. Palm says it
is **not** for To Do items.

**Now:** VALARM syncs and the Details sheet sets it, but nothing fires.
`PRODUCT_PLAN.md` #3 marks this blocked on sound (C3).

**Change:** sound isn't needed for the Palm half of this. When the device
wakes, if any event's alarm time has passed since the last wake, show a
"Reminders" list (time and description, with OK) over the lock. Sound or a
backlight pulse can come later. Build the list from the windowed Date Book
when it opens, and free it on OK, so it costs no resident RAM.

**Hardware note, worth a bench look before C3 stays "blocked":** most
ESP32-2432S028R revisions have a 2-pin speaker connector driven from GPIO26
through a small amplifier. If ours is populated, an LEDC chirp is a few
lines.

### 3.7 The Graffiti command stroke and an Edit menu (later; power users)

**Palm:** a diagonal stroke from lower left to upper right, then a letter,
runs a menu command. The standard letters are N (New), D (Delete), U (Undo),
C, X and P (Copy, Cut, Paste), S (Select All), K (Keyboard), G (Graffiti
Help) and R (Preferences). Every form with a text field has an Edit menu.
Shortcuts on destructive commands are allowed only when a confirmation
follows.

**For us:**
- The cheapest piece is an **Edit menu**, shown when `active_ta` is set,
  holding just **Graffiti Help**. It opens the stroke reference that already
  exists (`ui.c:2529`), the most Palm-like help we could offer. Undo and
  Select All can follow; a clipboard isn't worth its RAM yet.
- The **command stroke** is a recogniser change (`graffiti.c`, plus its
  template tests) and a "Command:" status line. Take it only if the bench
  shows people writing a lot, because the guide notes that a button above the
  pad is quicker than a shortcut.

### 3.8 A Tips "i" on the few dialogs that hide something

**Palm:** modal dialogs carry an "i" that shows a short tips text. It
describes each field and, in particular, the **hidden shortcuts**. Address
Book's tips are where people learned to tap to the right of a name to add a
note.

**For us:** after 3.2 and 3.3 we have hidden gestures worth teaching
(write to make an event, tap the title for the menu). One tips string on the
Date Book's Details sheet and one on the Find screen would do it. Write them
to our copy rule: say what the gesture is for, not which screen comes next.

---

## 4. What not to take

- **The 160×160 pixel tables** (button 12 px tall at y=147, and so on).
  Take the *proportions and positions* (actions in one left-aligned row at the
  bottom, nearest the pad), not the numbers. Our resistive touch at 240×320
  needs the bigger targets we already use. Palm made its alarm dialog's
  buttons taller for fingers for the same reason.
- **Beam.** There's no IR, and the guide's own Books example argues against
  beaming for its own sake.
- **Exit and Save commands.** We have none; keep it that way. Coach's sealed
  screen is the deliberate exception, and it is in the product plan.
- **Launcher categories.** Palm needed them for dozens of apps; our nine
  tiles fit on one screen.
- **Private records and Security.** These were real Palm features, but there
  is no counterpart in the vCard data we sync, and a PIN screen on a desk
  device is clutter. Revisit only if someone asks.
- **Scroll bars and the scrolling hard keys.** We have no hard keys, and
  touch-drag lists already show their position.
- **Anything from the OS source zips** (see §1).

---

## 5. Suggested order

1. **3.1 Resume and save on leave.** This fixes data loss, so it isn't
   optional polish.
2. **3.2 Write to create** and **3.3 title-tap menu**. Each is tiny and
   pure Palm.
3. **3.4 Alert conventions** and **3.5 prefix Find**, as one tidy-up commit
   each.
4. **3.6 Reminders**, after a bench look at the speaker connector.
5. **3.7 and 3.8**, if the bench says people want them.

Each one is a single group of work under the commit rule in `CLAUDE.md`.
None adds a resident buffer larger than a few dozen bytes.
