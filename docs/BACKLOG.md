# Work backlog — what is left

The one list of what's left to do. **Only open work lives here.** When something
is done it moves to `BUILD_PROGRESS.md` (the changelog, the hardware/RAM
reference and the hard-won lessons) and leaves this file. `PRODUCT_PLAN.md` is
the path to shipping; `COACH_DESIGN.md`, `GURU_HABITS.md`, `SRS_PLAN.md`,
`GAME_PLAN.md` and `DEX_PLAN.md` are per-app plans. Finished items and retired
analyses are in git history (`git log -p -- docs/BACKLOG.md`).

Every item is tagged by **where it can be worked**, because on this project that
is the constraint that binds (a base CYD with no PSRAM, developed mostly away
from the bench):

- **`[s]`** — buildable and verifiable *today* in the emulator (`make -C sim
  smoke`, or https://emu-commits.github.io/cyd-palm-bridge/). No hardware needed.
- **`[d]`** — needs the physical CYD (touch, Wi-Fi/TLS, sound, battery). It can
  usually be *written* off-device and checked on return.

**Working agreement.** Tick a box only when it is *verified*, not when it is
written: `[s]` means the smoke ran and the screenshot was looked at, `[d]` means
someone looked at the glass. **The user runs the bench checks — never tick a
`[d]` box for them.** One group per commit, on a branch off `main`. Before a
push: every gate, not just the smoke (`CLAUDE.md` lists them).

**Consolidated 2026-11-07.** Date Book, Address, To Do and the iCloud
(CalDAV/CardDAV) sync were archived that day (git tag `archive/pim-caldav`;
`BUILD_PROGRESS.md` has the entry), and every item that was about them left
this file. The rest was regrouped by area, with each open check kept once.

---

## RESUME HERE — 2026-11-07: Memos, Settings on the launcher, Guru editing

Built in the simulator on branch `claude/memos-settings-guru-edit` (the
`memo_*`, `planner_*`, `p1_*`, `p4_*`, `p7_*`, `guru_edit*`, `guru_delete*`,
`guru_new*`, `guru_list_end`, `study_*` and `settings_*` smoke shots looked
at). Nothing is on the device yet.

- [ ] The launcher on glass: Settings (the gear, position 1), HotSync, Games /
  News, Memos, Knowledge / Study, Guru, Coach. The gear reads as a gear `[d]`
- [ ] Settings from its tile: the seven tiles, the Assistant's greeting, and
  every panel opens as it did from the menu `[d]`
- [ ] Memos on glass: an empty list on a new card (no demo data), quick add,
  the checklist memo (the [X] button, tick and untick by box and by words),
  Find over memos `[d]`
- [ ] A card from before this firmware: its old Date Book, Address and To Do
  databases are left alone, and its memos (including any demo memos it was
  seeded with) are all there `[d]`
- [ ] HotSync on glass: "Sets the clock, and fetches the news and the
  weather."; a run's status line; Cancel mid-run `[d]`
- [ ] Guru's list: Home at the foot of the list after scrolling to the end, and
  it goes to the launcher `[d]`
- [ ] Guru's Edit: the strip comes up, the category steps on a tap, the name
  and the why take Graffiti, Save hides the strip and the page shows the
  change; `guru.txt` on the card holds it after a reboot `[d]`
- [ ] Guru's Delete (on the form): the confirm, Cancel keeps it, Delete
  removes it; a habit ticked today keeps today's count `[d]`
- [ ] Guru's Menu ▸ New habit: Save with nothing written adds nothing; with a
  name and a why it is added at the end of its category `[d]`
- [ ] Heap after a Guru edit: the edited list is held in RAM for the rest of the
  boot (`gurupool.c`'s arena, about 7–9 KB for the full list). A HotSync and
  Critters still work after editing Guru `[d]`
- [ ] Study's demo without radicals: a card that already had the old demo gets
  the new one (the `.demo` mark holds its size), progress on the kanji kept;
  the first lesson is five kanji, each quizzed on meaning and reading `[d]`
- [ ] The lock screen: AHEAD is one row (STUDY), SUN & MOON has Rise / Set on
  one line and the moon's phase by name on the next, with a bigger disc; the
  disc's lit side matches the sky (a crescent is a sliver, not a full disc) `[d]`
- [ ] Knowledge on glass (the tree, launcher position 6): the icon reads as a
  tree at 1x; the first open writes `/study/kq-numbers/`; a step's card
  scrolls (idea, Builds on, Explanation, Try it); the check shows the step's
  own question and its answer big; "Explored / Newly open / N more beyond",
  and Next step goes on. Is one step at this size pleasant, and does the
  explanation read comfortably? (K0's own question.) `[d]`
- [ ] Owner: review the 40 Knowledge cards (`courses/kq-numbers/nodes.jsonl`,
  all `"review": "draft"`, written by Claude). Set each to `"reviewed"` when
  it's right; when all are, rebuild without `--drafts` and the `-draft` mark
  goes. Also: keep the light reviews (1 d … 60 d), or pure discovery?
- [ ] Owner: the kanji mnemonics that used to point at their radicals were
  reworded (`courses/demo-kanji/items.jsonl`: 一 十 日 月 木 目 田 力 上 下);
  the 40 words were kept — say if Study should teach the kanji alone

Plans written with this change, **not approved, nothing built**:

- `docs/COURSE_CATALOG_PLAN.md` — Study courses hosted on GitHub, a catalog
  refreshed by HotSync, download on the next HotSync (Phase 2).
- `docs/KNOWLEDGEQUEST_PLAN.md` — the KnowledgeQuest brief evaluated against
  this device, and how it could be built on Study (Phase 3). **K0 is built**
  (the Knowledge tile and its 40-step pilot, above); K1 onward is not.

---

## Critters: the work of 2026-11-05 and 11-06

From `claude/critters-backlog`, merged into this file when the branches came
together (2026-11-07). `[s]` items here are built and were looked at in the
simulator; their `[d]` checks are with the rest below.

### Critters: a healing stalemate (reported 2026-11-05; built the same day, option: the healing move costs more)

- [s] **Vamporm can out-heal an equal foe, so a battle never ends.** A level 6 Vamporm against a level 6 Vamporm
  never ended: `refresh` (healing power 3.0, recharge 3) healed a full bar every few turns. **Chosen, for the owner
  to confirm:** the healing move costs more: `refresh`'s recharge is 8 (`move_edits` in `tools/game_region.json`,
  applied by `tools/mkgame.py`, credited to Tuxemon's move data). Measured in the sim with the same battles
  (random moves, both sides): recharge 3 left 32 of 200 battles unended after 2,000 turns; recharge 8 ended all
  1,000 seeds, the longest in 176 turns and the mean in 20. Recharge 7 also ended all 1,000, with a longer tail
  (1,323 turns). `make -C sim monster` still passes: Auto-play beats every keeper, and the new mirror gate
  (`monster_test` check 8, 200 seeds, 400-turn limit) is green. Not measured: a human player choosing moves, and
  the other healers (`recover`, `lifeleech`), which weren't changed.

### Critters: keep the screen awake in Auto-play (reported 2026-11-05; a regression)

- [d] **Auto-play must hold the screen awake.** It used to; the last update broke it, and now the screen auto-sleeps
  while Auto-play is running. Expected: with Auto-play on, the idle screen-off and the automatic light-sleep stay
  off until Auto-play is stopped or the game is touched, as they do on USB.
  **Fixed in code (2026-11-05, not yet on the bench):** `mon_ui_keep_awake()` (`firmware/main/ui_monster.c`) returned
  `g_mon && power_on_usb()`, so it ignored Auto-play. It now returns `g_mon && (g_mon->autoplay || power_on_usb())`.
  A lock or a touch clears `autoplay`, so the screen dims again once the game is no longer playing itself.
  Bench check: run Auto-play on the battery for longer than the idle timeout, with `backlight_sec` set low, and
  confirm the screen stays on; then stop Auto-play (touch) and confirm it dims again.

### Critters: five more lines, 60 critters (drawn by the owner 2026-11-05; built the same day)

- [s] **Heroic, cosmic, crystal, rock and eye lines** (14 critters; `tools/game_creatures/README.md` has each line's
  scales, places and what was drawn by hand). Each drawing's eyes and gems are checked at 10× with `--look` and at game
  size. `mksheet.py` gained `"keep"` (colours with their own place among the 15: the heroic red gems, the crystal and
  rock lines' pink eyes) and `"icon_row"` (icons drawn close under the feet). Neither changes an earlier sheet:
  `mkgame_test` proves each is the same to the byte. `make -C sim monster` still finishes with the Dex full.
- **For the owner:** the names, types, moves, places and Dex text are mine, all to change. Types: Valkit's line heroic,
  Starlet's cosmic, Geodot's earth (Prismyth earth and cosmic), Pebblub's earth (Tectorath earth and shadow), Oglet's
  water and shadow. Many of Tuxemon's heroic and cosmic moves need effects the game doesn't play, so those lines lean
  on Lantern, Punch, Radiance, Crystal, Lineage, Ruby and Dreamwalk.
- [d] **On the panel:** each new critter's front and back in battle read as its drawing at arm's length: the heroic
  gems red, the cosmic eyes cyan, Geodot's one pink and one cyan eye, Prismyth's gold eye, the rock line's pink glow.
  Nebulet, Astrolune, Valorix, Paladrax and Tectorath are the grainiest.

### Critters: Tuxemon's move effects, one at a time (requested 2026-11-05; plan written the same day)

The game plays damage, healing and three statuses; 192 of Tuxemon's 274 moves need effects it doesn't, which
leaves 64 moves (173 move slots, 27 critters) out of this game. **The plan: `docs/EFFECTS_PLAN.md`.** Each effect is
switched on by data (`"effects"` in `tools/game_region.json`), one per commit, measured by a balance run before and
after, and shown on the battle cards as a status badge.

- [ ] **Owner: review the plan's decisions** (§8): the order, the balance gate numbers, splash as plain damage,
  the badge design and drawing our own icons, the left-out list, no new status outlasting a battle.
- [s] **E0, the foundation** (no effect switched on): format 2.3 (the save stays 4: nothing new outlasts a battle),
  the `"effects"` list, the status badge on both battle cards, `tools/gen_status_icons.py`, `sim/tests/balance.c` and
  its baseline run. Built 2026-11-06 (`BUILD_PROGRESS.md`); shot `critters_badges`.
- [s] **E1, stat multipliers**, one step each: exhausted, blinded, enraged, hardshell, sniping, slow, softened,
  focused, chargedup, charging. Built 2026-11-06; one tune (Thunderclap) and two gate refinements for the owner
  (`BUILD_PROGRESS.md`).
- [s] **E2, statuses that cost turns**: noddingoff (asleep), stuck, grabbed, flinching, confused, harpooned, and
  "teams". Built 2026-11-06; asleep ships switched on (the quietest by the run). **Owner:** its review is still yours,
  and taking it off is one line in `"effects"`.
- [s] **E3, over time**: festering, lifeleech, wasting (plague is left out: E6). Built 2026-11-06, with a change to
  Auto-play's policy the game gate asked for (`BUILD_PROGRESS.md`).
- [s] **E4, shields and comebacks**: elementalshield, diehard, revenge, retaliate, prickly, spiky. Built 2026-11-06.
- [s] **E5, effects that aren't statuses**: remove, splash, multiattack, appear/disappear, statchange, switch,
  prop_damage (charging came with E1). Built 2026-11-06. **prop_healing is played but off the list**: it brought back
  the healing stalemate in the Vamporm line (244 unended); Hibernate and Shadow Blast stay out until it's tuned.
- [s] **E6**: the effects left out on purpose, each with its reason in `GAME_FORMAT.md` §2 (charmed, lockdown,
  feedback, lifegift, wild, plague, and the effects swap, run, forfeit, money, scope, transfer, life_swap, life_share,
  photogenesis, foresight, cooldown_modifier).
- [d] **Bench, per phase:** a few battles with the phase's effects on the board, the badge readable at arm's length.

### Critters: the drove road, Kestrel's look, and progress without grinding (requested 2026-11-06; not started)

Three requests from the owner, written as backlog items; nothing built.

- [s] **1. The shortcut between Larkhollow and Kestrel: more reasonable logic and a visible foundation.** Built
  2026-11-06: a locked gate (our CC0 drawing, `tools/gen_gate_sheet.py`) stands across the road at both ends, two
  leaves each, with its own words (the notice: closed to travellers, the herds' road, kept by the Ring, opened on
  the Warden's word); the drovers stand beside the road and give the reason (wild critters took the herds on it, so
  the Ring locked it, and Ivy keeps the key), and once `beat_ivy` is set the gates are gone and one drover at each
  end says what changed. Shots `critters_drove_gate`, `critters_drove_gate_says`, and the open road
  (`critters_drove_road`, `critters_drove_road_home`). **Owner:** the words are my drafts. Today the
  drove road is two warps (`larkhollow` 23,11 ↔ `kestrel` 0,16 in `tools/game_region.json`) with nothing on the
  ground: two drovers stand on the road (Drover Lad in Larkhollow, Tam and Lil at Kestrel's end) and their words are
  the only gate, which the player reads as "a person in the way" rather than a closed road. Make it read as a road:
  a physical barrier (a gate or a chain across the road, drawn in the map) at each end, one reason the story gives
  for it (the Warden's word, so `beat_ivy` opens it), the road drawn as a road with a sight line from either end, and
  one drover who says what changed once it's open. Check the logic against the other route: the tunnel way from
  Larkhollow is long, so the road must be the sensible shortcut after Ivy and not an accident of the map. **(mine:
  a fixed gate that opens by flag, not a drover stood on a tile.)**
- [s] **2. Redo Kestrel's town so it doesn't look like Larkhollow.** Built 2026-11-06 from the same two clean sheets:
  dry earth and sandy roads (`core_outdoor`'s brown family, 16 columns right of Larkhollow's green), the Ring a
  walled flagstone arena with banners below its gate hall, and a market square south of the crossroads (an awning
  stall, fruit crates, a fountain); the town is six rows longer (26 x 27) and its south exit moved from row 20 to 26,
  every other place unchanged. At most 79 tiles on one screen (the cache keeps 96). Shot `critters_kestrel`,
  looked at, before and after. **Owner:** the two houses are still the sheet's only two wooden designs. Both towns use the same two sheets
  (`core_city_and_country` and `core_outdoor`, see `tools/game_maps/larkhollow.json` and `kestrel.json`), so they read
  as one village. Give Kestrel its own look: a different ground and building palette from the sheets (or a second
  sheet, if one fits), its own roof and wall styles, and a town plan that suits a Ring town (a walled arena, stands,
  a market street) rather than a village. Kestrel stays laid out for this game (`tools/game_maps/`), so no Tuxemon map
  is copied. Shots `critters_kestrel` before and after, looked at.
- [s] **3. Progress must not come from walking back and forth.** Built 2026-11-06, for the owner to judge on the
  bench: **the Mere**, a side area of our own off Larkhollow's west road (gated until Keeper Tom), with three tiers of
  optional keepers (8-11 before Ivy; 21-24 after Ivy; 36-38 after her rematch, for Ashe at 40), a grass table that
  steps up with them, Hermit Bryn's rest at its entrance, and Fisher Ola's request, paid in 3 Super Potions (a new
  "gift" person kind). Auto-play now takes the keepers it can beat before training for a harder one. Measured over
  the five games (seed 1): **wild battles fought only to train fell from 676 to 300**, the worst stretch before one
  keeper from 97 to 40, and losses are 101 (E0 had 87). `make -C sim monster` fails past **50** in one stretch
  (`GRIND_MAX`, mine: **owner, set the number**). Still open below: more side areas and quests, and the Reach hunt.
  - **Side areas to explore** off the main road: a path with its own wild table and a reason to go (a cave, a
    shrine, a lake, a mine, a Wren specimen trail), each with a healer and something to find. Each one takes a
    few hours, not a few minutes of walking.
  - **Side quests that pay in levels and items**: a local request per town (a Ring errand in Kestrel, a Woodcutter's
    job in the Brideswood, a delivery from Ned, a specimen from Sam's shop, Mum's errands at home). Each one sends
    the player through a side area and ends at a person, so the xp comes with a story beat, not a loop.
  - **A level curve per beat**: for each keeper, the level the player should have reached by then and the side
    content that brings them there (the `steps` and encounter tables in `game_region.json` already change by act;
    extend that to side areas).
  - **A gate, so this can't come back**: a balance check (`make -C sim monster` or a new one) that fails when Auto-play
    needs more than a set number of consecutive wild battles in one area before a keeper, or when a keeper's level
    comes from one area only. **(mine: the number is the owner's to set.)**
  - **Open question for the owner:** the full-Dex hunt in the Reach (22,000 to 40,000 actions, `[d]` above) is the
    same kind of walking. Is it a post-game chore to keep, a side quest with a reward, or cut?
- [ ] **4. A travel screen between Larkhollow and Kestrel, so the road feels like distance.** Requested 2026-11-06
  (the owner). Today the drove road is two warps, so Larkhollow and Kestrel sit side by side with no sense of the
  miles between them. Add a travel moment on the road: a short scene (a screen or a scrolling strip of the road
  with hills, a ford or a waymark, and the journey's time passing) shown as the player crosses, before they arrive at
  the other town. Keep it short so it adds weight without becoming a walk. It should use the same road as item 1
  (the gate once shut, the open road after `beat_ivy`) and not change the warps' logic. **(mine: the look and length
  are the owner's to judge; I'd keep it to one screen with no input beyond a key to continue.)**

### Critters: Auto-play's catch and run rules (requested 2026-11-06; not started)

Three requests from the owner, written as backlog items; nothing built. Today's rules are in
`firmware/main/monster_bot.c` (the battle choice): a catch is thrown when the wild critter is at or under half HP
(or one blow would take it), or at once while hunting; a run is chosen when the party is `worn()`.

- [ ] **1. Don't catch a critter at full health.** Auto-play should not throw at an opponent whose HP is full (it
  has not been worn down, so the throw is wasted). Hunting throws at once (`hunted`), which breaks this: the rule
  must hold in hunting too, so a full-health critter is weakened first. Gate: a battle log with a catch on a
  full-HP foe fails `make -C sim monster`.
- [ ] **2. Don't catch when the party has no open space.** Auto-play should only try to catch while the party has
  5 or fewer members, so the catch has a free slot. Today the hunting branch also allows a catch on a full party
  when the box has room (`w->n_box < MP_BOX`), which the owner does not want. A full party sends its straggler to
  the box first (see the 2026-11-04 release section), so the catch comes after that move, not instead of it.
  Gate: no catch attempt with 6 in the party.
- [ ] **3. Don't run while another party member can still fight.** Auto-play should not choose Run (`MP_ACT_RUN`)
  while any party member other than the active one is at full health, since that member could win the battle.
  `worn()` allows a run on total HP alone, which can fire with a full-health member still in the party. Run only
  when no other party member is at full HP. Gate: a run chosen with a full-health bench member fails
  `make -C sim monster`. **(mine: "full health" means HP at its maximum, and a fainted member does not count as
  able to fight.)**

### Critters: battle screen overlap (reported 2026-11-06; not started)

- [ ] **1. The opponent's sprite must not overlap the player's status box.** In battle, the opponent critter's
  sprite currently draws over the player's critter's name, HP and status box. The sprites and the boxes should be
  laid out so neither covers the other, at the device's screen size. Gate: a battle smoke shot showing both
  cards with no overlap, looked at (`[s]`); the bench check is the owner's (`[d]`).

---

## Bench checks `[d]`, by area

**Re-flash before reasoning about what is on the device.** The bench is only as
current as the last flash.

### Critters and the Dex (built in the simulator 2026-10-16 to 11-04)
- [ ] Walking in Critters: smooth, no visible drawing down the screen (40 ms a
  frame at 160 MHz, measured). The panel at 40 MHz: no speckle, wrong colours
  or shifted lines anywhere, in any app; if there are, `clock_speed_hz` in
  `display.c`.
- [ ] Critters and the Dex open within about half a second of the tap
  (measured 419 and 438 ms to first paint), and Wi-Fi and a HotSync still work
  (the SD card changed SPI host).
- [ ] At 160 MHz and with the loop sleeping at least a tick, walking still feels
  smooth (33 → 40 ms a frame). The battery gain isn't measured: `power.log`
  can A/B it.
- [ ] The D-pad and A/B under a finger (a tap turns, a hold walks), and after
  moving 2 px in the slightly smaller panel; how long entering Brideswood takes.
- [ ] On USB power Critters stays lit; unplugged it dims and locks, and waking
  comes back to the same battle (4.15 V and up reads as USB).
- [ ] The story on glass: choosing a look and a first critter (`<` from Agnite
  wraps to Nut, then Vamporm); talking; a keeper spotting you and their second
  critter; losing brings you back to the last healer; Larkhollow to Kestrel;
  the frame rate in Kestrel (60 tiles, six people).
- [ ] The rooms: in at Home's door, out over the mat, Wren's Lab, the rest
  house (Nurse Hale rests the party).
- [ ] Auto-play: on from the menu, it walks, talks and battles at a watchable
  pace, the title says so, a tap takes over, the lock pauses it; how long it
  runs on battery, and whether it ever stands still or walks in circles. The
  party panel is readable at arm's length and keeps up.
- [ ] The longer game: an Auto-play run from a new game to Dr Wren (hours); a
  person's game into Act 2; whether its pace feels right. Auto-play in the
  Reach (the map costs 10.4 KB; a full Dex takes 22,000–40,000 more actions).
- [ ] The store: Sam in the doorway before Ivy, behind the counter after;
  buying with A; the Bag; a potion in battle; Auto-play buying.
- [ ] After flashing with Critters: the 2.0 game replaces the card's old one at
  boot. The web installer with "Also install Critters" ticked puts
  `games/monster/game.bin` on the card; without the box the card's game is left.
- [ ] A party of six: the icons on Party and on Auto-play's panel, and tapping a
  line of Party (24 px each).
- [ ] The Dex: opens from the Games folder; a caught critter shows in full after
  saving in Critters; an old save keeps its party; leaving gives the heap back
  and the title bar goes back to black; ◀ ▶ are fast enough; the portrait and
  the green clock read well; Anoleaf and Fluttaflap hold up on the blue.
- [ ] The Dex's fade is very slight on the Talk page at every strength, and its
  bezel has no dark square corners. Menu ▸ TV effect (Light, Standard, Strong,
  Off): is small text readable, and which do you want as the default?

### Study
- [ ] The tile and portrait, a lesson and its quiz, reviews two hours later
  (Undo, the Card view), the kana answers, and whether 1-bit kanji read well at
  64 and 24 px.
- [ ] The 2x answer reads well, and a long one falls back to 1x.
- [ ] Full screen: Home and Menu in the title bar are easy to hit (the wide Home
  never touches the clock), the strip is back on the dashboard, a lesson card
  mostly without scrolling; the 20 px kana read well.
- [ ] The Week screen after a few days: the bars match what was done, the streak
  holds overnight, Undo takes a review back off the count.
- [ ] The lock's STUDY row and the weather box above it read well, the badge on
  the icon, the count going up on its own as reviews come due.
- [ ] S6 — a full course on the real card: open time, next-card time, heap,
  battery pull, kanji readability; loading a second course.

### Guru and Coach
- [ ] Guru full screen: the list scrolls and ticks well, the greeting
  dismisses, Home and Menu work there.
- [ ] The week charts: legible at 18 px bars? Does the dashed target line read
  as a line? Coach's day goal (6) dwarfs a normal day — honest, or
  discouraging?
- [ ] The speakers in the strip (the Assistant, Coach, Guru; portraits right):
  tolerable over the silkscreen until tapped away? Do they read as standing in
  front of the writing area?
- [ ] The portraits at 1:1 (thin ink outlines are what a resistive panel's
  diffusion softens); tap-anywhere with a thumb.
- [ ] Coach: does the 15 % session dim read as intentional (`CO_DIM_PCT`)?
  Hollow (gave-up) marks are faint; the five-second give-up hold; the ritual's
  three questions; is 1.4 s × 10 phases the right end flash (`CO_FLASH_MS`,
  `CO_FLASH_N`); does the indefinite reflect hold read as insistent or annoying?

### Memos, Graffiti and the Palm behaviours
- [ ] P1: write half a memo, let the screen sleep, wake and swipe up: the memo
  is there, saved; Home from an edit saves it.
- [ ] P3: the title is an easy target and doesn't fight the category pop-up.
- [ ] P4: the delete dialog (title, question and buttons don't touch).
- [ ] P7: the command stroke recognises reliably and never fires by accident
  while writing; the stroke then N opens New.
- [ ] P8: the "i" is easy to hit and the tips read well.
- [ ] The swipes on real glass: the harness says 99.5 % of hurried swipes
  register with no letter lost; does a sideways flick ever eat a letter?
- [ ] The PUNC chip is a small target at the top of the strip; is 4 s too short
  or too long?
- [ ] The stroke sheet at 40 px cells: legible, and a drag never lands as a tap.
- [ ] Quick-add with real Graffiti: is the bar worth using over the full form?
- [ ] Kana Tier 2: tune `KW_THRESH` on the real panel.
- [ ] The 13 px drawn checkbox as a finger target.

### System, Settings and HotSync
- [ ] Password entry: the peek is long enough to read, Show/Hide, a long
  password scrolls with `<`.
- [ ] Fast keypad entry on the Calculator: nothing missed; a deliberate slow
  slide across keys never types twice.
- [ ] The trainer and Kana prompts on their own line — legible, not crowded?
- [ ] HotSync with no Wi-Fi lands on Settings ▸ Wi-Fi with the Assistant's
  instructions: once with no network saved, once with a wrong password.
- [ ] Wi-Fi: a real join, the scan against real APs, and whether 8 s is enough
  for networks 2–4.
- [ ] Setting the clock by hand writes it (`settimeofday` is refused in a
  container).
- [ ] A HotSync reports the clock, news and weather honestly; an RSS feed
  fetches and the heap holds; `weather.dat` is written and real numbers show.
- [ ] Location: wipe `latitude`/`longitude`, sync, the `geoip:` line names the
  right town; a carrier NAT or captive portal leaves it unchanged; a pinned
  location survives two syncs.
- [ ] The pool line: `lvgl: pool low-water: ... on "<screen>"` on the UART —
  which screen is the tightest on hardware?
- [ ] The greys: zone bars and list hairlines are `COL_RULE` `0xC8C8C8`, dimmed
  text `COL_DIM` `0x8C8C8C`, the calculator's grey keys. Grey-on-grey behind a
  resistive panel is what an emulator judges worst.
- [ ] 12/24-hour consistency across the title bar, weather strip, Rise/Set.
- [ ] The unlock band catches the eye without shouting.
- [ ] Install from the web page with "Erase" clear: the update keeps the saved
  Wi-Fi password.
- [ ] Games: Zip's drag feel under a fingertip (`ZP_CELL`); `New` latency and
  WDT quiet; play clocks across a real power cycle. **Open decision:** should a
  game's clock pause when the backlight times out (a hook from `idle_step()`
  into `games_pause_clocks()`)?

### Power and hardware
- [ ] **Test the attached speaker** — plug it in and confirm it makes sound,
  note which connector it used (the 2-pin beside the SD slot on most
  revisions, GPIO26 through an amplifier) and whether it is loud enough without
  a volume control. Gates §Next 4 and 5.
- [ ] Battery: does the gauge track a real discharge through the flat middle?
  Is the divider on tolerance (`BAT_TRIM_PERMILLE` at unity; one multimeter
  reading at `JP2`)?
- [ ] Screen-on power (idle is solved: >24 h, <46 mA). Ranked, unmeasured:
  (1) backlight level; (2) DFS *without* light sleep (`min_freq_mhz = 80`);
  (3) CPU 160 → 80 MHz fixed, A/B with `power.log`; (4) panel `SLPIN` when the
  backlight blanks.
- [ ] A hardware button → sleep: BOOT/GPIO0 is RTC-capable and can be both the
  trigger and the `ext0` wake; RESET/EN cannot be intercepted; deep sleep kills
  tap-to-wake (touch is SPI). Measure this board's real sleep current first.
- **The case** — out of software scope; gating for launch.

### For the owner to review (no bench needed)
- The Dex speaker's lines in `dex.c` (`STORY`, `MEMORY`, `NEARLY`, `FULL`) and
  his biography are drafts; so are the shortened descriptions
  (`game_region.json`, `tools/game_creatures/`) and our critters' categories
  and sizes.
- Critters choices made while building (2026-11-04): sprite facing by battle
  heading (front left, back right); Auto-play's party rule (the last 8 foes,
  10 levels under the average; the last of a line may go, the Dex keeps it);
  the story's threat and its single-scene ending.

---

## §Next — features, not yet scheduled

1. **"About this screen" on every screen `[s]`.** Someone finished a Zip board
   and could not learn from the device why it still said "4 left". Extend
   Menu ▸ About to show a couple of paragraphs *for the current screen*,
   keeping the global About. One scrollable label in the overlay pattern, text
   in one static table keyed by screen (flash, not RAM). Cover the games,
   Memos, Graffiti, HotSync, News, Study, Guru and the lock screen.
2. **Graffiti writing *feel*.** Accuracy is done and gated (99.7 % letters at
   3 px jitter; `make -C sim graf`). Left: ink-trail / char-echo polish and
   threshold tuning from real on-device telemetry.
3. **The Assistant's other jobs** (parked until asked): a HotSync-failure
   explainer, a first-boot welcome, an SD missing/corrupt explainer.
4. [ ] **Short, charming beeps across the whole firmware — plan first `[s]`
   for the plan, `[d]` to hear it.** One sound vocabulary: a few short tones (a
   rising two-note "ready", a falling "done", a low "no", a soft tick for taps),
   each under about 150 ms, with a single place that maps events to sounds.
   Cover HotSync done/failed, a Study answer right or wrong, a Critters
   encounter and the lock/unlock. Silent when the speaker is absent. Write the
   plan (the event list, the tones, the RAM and flash cost, how it is tested
   without hardware) before any code; the sound itself waits on the speaker
   test above.
5. [ ] **A volume setting in Settings `[s]` for the UI, `[d]` to hear.** A
   Volume row (off, low, medium, high) saved with the other config, read by
   the beep layer (§Next 4). The simulator checks the row and its saved value;
   only the bench can say whether the levels are right.
6. **Study courses from GitHub** — `docs/COURSE_CATALOG_PLAN.md`. Not approved.
7. **KnowledgeQuest** — `docs/KNOWLEDGEQUEST_PLAN.md`. Not approved.
8. **HotSync's memory, re-measured without the sync engine.** The task stack
   is still the 32 KB the engine needed (`HOTSYNC_STACK`); the "stack:" line
   at the end of a completed run says what it can be cut to. The
   `heap[wifi-up]` creep (~13 KB lower after a session of app use than after a
   fresh boot) is the next thread if feed fetches fail intermittently `[d]`.

---

## §Proposals — NOT approved

From the R15 review (2026-09-23) and since. Ranked within each heading by value
for effort.

### User experience
1. **A first-run path.** A fresh device boots to the lock screen and then the
   launcher; nothing points at Settings ▸ Wi-Fi. The Assistant could greet the
   first boot and say where to start (§Next 3).
2. **Undo a delete.** A "Deleted — Undo" toast for a few seconds.
3. **Menus that fit.** Coach's Options menu runs near the bottom edge. Move
   the rarely used rows (Reset counters) under one "More..." or into Settings.
4. **Position 6 on the launcher is empty** since 2026-11-07. Candidates: the
   Graffiti trainer out of the Games folder, or the KnowledgeQuest app if that
   plan is approved.

### Open-source readiness
1. **A photo of the real device** at the top of the README.
2. **Split `ui.c`** (about 12k lines) into modules behind a private header —
   lock/dashboard, Memos, Settings, Coach, Guru, Study, games, Graffiti,
   overlays. It is the largest barrier to a contributor, and it makes every
   simulator rebuild recompile everything.
3. **The smoke tour's shot names** still carry internal codes (`r5_locked`,
   `p4_*`); renaming means renaming the shots.
4. **One `make check`.** The rule "every gate, not just the smoke" lives in
   `CLAUDE.md` and a maintainer's head. One target that runs the host gates,
   the simulator gates and `smoke32`.
5. **The archived account keys in `config.ini`.** `bridge/config.c` and the
   secret store still read `dav_*`, the collection paths and the sync policy
   (so an old card's file still loads); nothing uses them. Drop them, with the
   store's account-password slot, in one change with `config_test`.
6. **Contributor scaffolding.** `CONTRIBUTING.md` (the gates, the design rules,
   the pool budget), issue templates, and tagged releases with a changelog.
   Ship the simulator's toolchain as a Dockerfile or devcontainer.
7. **Static analysis in CI.** `-Wextra`, `cppcheck`, and ASan/UBSan over more
   of the host gates.

---

## Parked — offered, NOT approved (do not build without a yes)

- **The app platform** (`APP_PLATFORM_PLAN.md`): native C apps from the SD
  card, with an SDK, versions and a local inspection. **On hold since
  2026-09-29**, planned but not built. Bring it back if someone else wants to
  write apps, if `ui.c` or flash becomes a bottleneck, or if apps need
  sharing between devices.
- **Opt-in CORS-proxy RSS fetch in the web emulator.** Feed servers send no
  `Access-Control-Allow-Origin`; the routes are a public proxy (fragile) or a
  self-hosted one (infra). Emulator parity only.
- **Coach's optional hardware** (`COACH_DESIGN.md` §9): the 8:00 knock (an RTC
  alarm pin to an RTC-capable GPIO), a piezo tick (one GPIO, a second LEDC
  channel), co-working over ESP-NOW (needs Wi-Fi resident during a session —
  measure heap and battery first).

## Decided — kept so the reasoning is not relitigated

- **Date Book, Address, To Do and the iCloud sync are archived** (the owner,
  2026-11-07: "pointless to compete with smartphones"). The code is at the git
  tag `archive/pim-caldav`; nothing on the device or in `bridge/` syncs with a
  calendar or contacts server any more. BLE and a companion phone app were
  dropped earlier for the same reason (`PRODUCT_PLAN.md` §2).
- **The SRS app is built in, not an SD app** (2026-09-29). The app framework
  would have cost several times the app, saved only the full-firmware flash,
  and limited the app to a small memory block, 1-bit bitmaps and no kana
  font. Courses are data on the card, which covers most of the benefit of
  adding things without reflashing. `SRS_PLAN.md` §1.
- **No RTC part for timekeeping.** Drift on battery is under a minute a day and
  idle clears 24 h (measured 2026-08-27). An RTC survives only as the enabler for
  the parked 8:00 knock. If one is ever fitted it shares SPI2 with the SD card,
  and a breakout whose MISO does not go high-Z will corrupt every SD read.
- **The LVGL pool is 32 KB** (`31100 bytes total` on hardware), and `make -C sim
  poolparity` fails if the sim and the device drift. `smoke32` is 32-*bit*, not
  32 KB.
- **The Japanese trainer is frozen at Tier 2** (product decision 2026-07-19).

## Someday

Dark mode.
