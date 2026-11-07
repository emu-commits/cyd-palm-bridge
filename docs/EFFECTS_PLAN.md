# Critters: Tuxemon's move effects, one at a time

> **Status: a plan, 2026-11-05. Nothing built.** Asked for by the owner on
> 2026-11-05: "a plan to add all the Tuxemon effects and introduce them one at
> a time to make sure the game stays balanced with them", "including a way to
> visualize in battle which status effects are active on each critter". The
> checklist is in `BACKLOG.md` ("Critters: Tuxemon's move effects"). Choices
> marked **(mine)** are for the owner to change.

## 1. Where the game is today

The game plays three kinds of effect (`tools/mkgame.py`, `Rules._unsupported`;
`GAME_FORMAT.md` §2):

- **damage**, plain;
- **healing**, of itself or the target;
- **give a status**, and only three statuses: **poison** and **burn** (lose
  max HP / 8 a turn) and **recover** (gain max HP / 16 a turn).

A move with any other effect, or with a range the game doesn't know, is left
out when the game file is built, and its critter simply never learns it.
Measured on 2026-11-05 (the build of 60 critters, Tuxemon at the pinned
`9e6258f`):

| | Count |
|---|---|
| Tuxemon techniques | 274 |
| …that the game plays | 82 |
| …left out | 192, blocked by about 60 different effect kinds |
| Moves our 60 critters would learn but can't | **64 moves, 173 move slots, in 27 critters** |

What blocks the 64 moves the game's critters actually want, most first (a move
can need two effects; `mkgame.py --report` lists them all):

| Effect | Moves here | What it does in Tuxemon |
|---|---|---|
| give **exhausted** | 6 | melee and ranged × 0.5 |
| **splash** | 6 | damage that also hits the target's side (in one-on-one: the target only, see §4.5) |
| give **blinded** | 5 | speed and dodge × 0.5 |
| give **enraged** | 4 | melee × 2, ranged × 0.5 |
| give **noddingoff** (asleep) | 4 | skips its move; 50 % a turn to wake after the first; at most 5 turns |
| give **stuck** | 4 | its melee and touch moves' potency ÷ 2 |
| give **elementalshield** | 3 | a foe that hits it loses max HP / 16 |
| give **grabbed** | 3 | its ranged and reach moves' potency ÷ 2 |
| give **hardshell** | 3 | armour × 1.5 |
| give **sniping** | 3 | melee × 0.5, ranged × 2 |
| **remove** | 3 | takes the target's (positive or negative) status away |
| give **festering** | 3 | it can't be healed |
| give **diehard** | 2 | the next blow that would faint it leaves it at 1 HP |
| **appear** / **disappear** | 3 | two-turn moves: out of reach for a turn, then strike |
| give **lifeleech** | 2 | the giver drains max HP / 16 a turn from it |
| one each | 9 | switch (change a type), statchange, multiattack (hits twice), focused (dodge × 1.5), revenge, prop_healing, prop_damage, chargedup (all stats × 2), charging |

**Two Tuxemon rules shape everything below.** A critter holds **one status at
a time**: a new one replaces the old (`on_positive_status: replaced`, the rule
we already follow). And **most statuses are only stat multipliers**: exhausted,
slow, blinded, enraged, focused, hardshell, softened, sniping and chargedup are
one `stat_modifiers` table each in `mods/tuxemon/db/status/`. One mechanism
plays all nine.

## 2. The rule for introducing them: one at a time, measured, reversible

1. **An effect is switched on by data.** A new list in
   `tools/game_region.json`, `"effects": ["exhausted", ...]`, names the effects
   the build may use. `mkgame.py` leaves out a move whose effect isn't both
   supported by the firmware and on the list. Turning an effect off again is
   one line and a rebuild of `game.bin`, no reflash. The firmware can play more
   than the list allows; never the other way round (the build refuses it).
2. **One effect (or one status) per step, one commit per step.** Each step is
   the same six parts, in this order:
   1. the rule in `monster_play.c`, from Tuxemon's code (`tuxemon/core/effects/<name>.py`,
      cited in a comment), in the integer arithmetic of the rest;
   2. `mkgame.py` accepts it and writes it (`GAME_FORMAT.md` updated);
   3. its battle icon and words (§5);
   4. Auto-play knows what it's for (§6);
   5. the balance run before and after (§3), numbers into `BUILD_PROGRESS.md`;
   6. a sim screenshot of a battle with it, looked at.
3. **A step that fails the balance gates doesn't ship as is.** First choice:
   tune it with `move_edits` on the moves that give it (potency, recharge,
   accuracy), as we did with `refresh`. Second: leave it on the firmware but
   off the list, and say why in the backlog. Tuxemon's own numbers are the
   starting point every time.

## 3. Balance: what's measured, and the gates

**A new host program, `sim/tests/balance.c`** (`make -C sim balance`), built
on the rules and on Auto-play's move choice, so both sides play as Auto-play
does. Every run prints one table and writes `build/balance.tsv`.

- **Round robin:** every critter against every critter at levels 10, 25 and
  40, each at the moves it would know then, 20 seeds a pair. 60 critters is
  3,600 pairs × 3 levels × 20 seeds = 216,000 battles. My estimate, not
  measured: minutes on the host, from how fast the mirror gate's 200 run; E0
  measures it, and if it's too slow a gate run samples 2,000.
- **Per critter:** win rate, compared with a baseline the size of its stats
  (a stronger critter should win more; the measure is how far it moves).
- **Per move:** how often it's chosen, and the win rate of the side that used
  it.
- **Per battle:** turns, mean and longest, and how many never end in 400
  turns (the mirror gate's limit).

**The gates**, each new effect against the run just before it **(mine,
first numbers to tune):**

| Gate | Pass when |
|---|---|
| Nothing stalls | no battle unended in 400 turns (today's mirror gate, widened to all pairs) |
| Length | the mean battle stays within 25 % of the previous run |
| No critter runs away | no critter's win rate moves more than 10 points |
| No move takes over | no single move is chosen in more than 40 % of the turns of the critters that know it, unless it was before |
| The game still plays | `make -C sim monster`: Auto-play beats every keeper and fills the Dex, its losses within 50 % of before |

A step's numbers go in `BUILD_PROGRESS.md` beside the effect's name, with the
seed set, so a later step can be compared. Not measured by any of this: a
person choosing moves. The bench item for each phase asks the owner to play a
few battles with the new effects in the sim and on the board.

## 4. The order

The order is **(mine)**: what unlocks the most of our 64 moves for the least
new machinery first, and the rules a later effect needs before it.

### E0: the foundation (no new effect is switched on)

- **The format**, 2.3 (the save, version 5). `STAT` gains what the new
  statuses need: a kind beyond 1–3, the five stat multipliers (tenths, so
  0.5 is 5), a duration (0 none), a chance (percent, for asleep, flinching,
  confused), the ranges it affects (stuck, grabbed), and flags (positive,
  negative, ends with the battle, linked to its giver). A `MOVE` effect gains
  the kinds that aren't "give" (remove, splash, multiattack, ...). The status
  icons (§5) go in their own section of the game file.
- **The rules** keep a status's turn count and, for a linked status, which
  side gave it. Only statuses that outlast a battle are saved; today that's
  poison and burn, and nothing new does **(mine)**.
- **The battle cards show the status** (§5), first for the three we have.
- **The `"effects"` list** (§2.1) and its check in the build.
- **`balance.c`** and its first run: the baseline every later step is
  measured against.

### E1: stat multipliers (one status per step)

`exhausted` (6 moves), `blinded` (5), `enraged` (4), `hardshell` (3),
`sniping` (3), then `slow`, `softened`, `focused` and `chargedup`. One rule
for all: the stat is multiplied while the status lasts, inside
`mp_stats`-style arithmetic for the battle only. Expected risk: low; the
damage formula already divides strength by resistance, so ×2 and ×0.5 move
damage by at most 4 times. The balance gate watches enraged and chargedup most.

### E2: statuses that change what a critter can do

`noddingoff` (asleep: skips its move, its own duration of 5 turns), `stuck`
and `grabbed` (halves the potency of some ranges), then `flinching`,
`confused` (a random move instead) and `harpooned`. **Risk: high.** Losing
turns is what makes a battle feel unfair; asleep gets its own balance run with
the owner before it ships.

### E3: damage over time, drain, no healing

`festering` (no healing), `lifeleech` (the first linked status: it needs the
giver), then `wasting` and `plague`. Watched with the healing stalemate in
mind (`BACKLOG.md`): festering should shorten battles, lifeleech could lengthen
them.

### E4: shields and comebacks

`elementalshield`, `diehard`, then `revenge`, `retaliate`, `prickly`, `spiky`.
Each acts when the other side hits, so they're the first effects outside the
end-of-turn tick.

### E5: effects that aren't statuses

- `remove` (takes a status away; trivial once E1 exists);
- `splash`: in a one-on-one battle it's plain damage, so it's played as
  damage to the target **(mine; if double battles ever come, it changes)**;
- `multiattack` (hits twice, each hit drawn on its own);
- `appear`/`disappear` (two-turn moves: the first turn it's out of reach);
- `statchange` (a stat changed directly, not by a status), `switch`
  (changes a type), `prop_damage`/`prop_healing` (by a proportion of HP),
  `charging`/`chargedup` (a turn spent, then all stats × 2).

### E6: left out on purpose

Effects that mean nothing in this game or would need a game it isn't:
`swap`, `run`, `forfeit`, `money`, `scope`, `transfer`, `life_swap`,
`life_share`, `photogenesis`, `foresight`, `cooldown_modifier` (our recharge
works differently), `charmed`, `wild`, `lockdown`, `feedback`. Each gets one
line in `GAME_FORMAT.md` saying why, and stays out. **(mine; the owner may
want any of them back)**

## 5. Seeing the statuses in battle

**Today** a status shows only as words in the battle box ("Nut gets poison."),
and they scroll away. The cards show name, level and the HP bar, and nothing
says what's on a critter now.

**The design (mine):** each battle card carries **one status badge**, because
a critter holds one status at a time:

```
 ┌──────────────────────────────────┐
 │ Maelstrotl  Lv 42          [☠]▸ │   foe card: name, level, then the badge
 │ ████████████░░░░░░░░░░░░░░░░░░░ │   HP bar
 └──────────────────────────────────┘
          ┌──────────────────────────────────┐
        ◂ │ Valkit  Lv 12             [z]••• │   our card: the badge, and its turns left
          │ ██████████████████████████░░░░░ │
          │ HP 41 / 52           Asleep      │   our card has room for the status's name
          └──────────────────────────────────┘
```

- **The badge** is the status's 9 × 9 icon in a 1-px frame, 11 × 11, at the
  right end of the name line, on a red-tinted ground for a negative status
  and a green one for a positive (the frame colour, so it reads without
  colour too: a negative badge's frame is solid, a positive one's dotted).
- **Turns left**, for a status with a duration (asleep's five), as dots
  beside the badge, one a turn, so the end is visible coming.
- **Our card's third line** names the status after the HP, in words
  ("Asleep", "Exhausted"): our side has the room, and it's the one the
  player acts on. The foe card has only the badge.
- **A stat multiplier** shows as its own icon (exhausted, enraged and the
  rest each have one), and the badge's line in the Party screen gives the
  effect in words ("melee and ranged halved").
- **When it changes**: the badge blinks twice when a status is given, takes a
  turn's dot when it ticks, and goes when it ends, in step with the words in
  the box, from the events the rules already send (`MP_EV_STATUS`, `_END`).
- **Outside battle**: the Party screen, the Dex's party list and Auto-play's
  party panel show the same badge beside a critter that keeps its status
  after battle (poison, burn).

**The icons are drawn for us (mine).** Tuxemon has a 9 × 9 icon for each
status (`gfx/ui/icons/status/`), but `ATTRIBUTIONS.md` doesn't list them, so
their licence is unknown and `--strict` would refuse them. A new
`tools/gen_status_icons.py` draws ours, CC0, as `gen_interior_sheet.py` draws
the rooms: one 9 × 9 picture per status, in at most 4 colours, built into the
game file. If the owner learns their licence, Tuxemon's can replace ours with
an allow-list line each.

**Checked how:** a sim screenshot for each status on both cards, with the
longest name in the game at level 100, and a pixel test that the badge never
touches the name's text (the lesson of the scanlines on the menu: check
overlays with a pixel test, not a glance).

**Cost:** about 40 icons × 9 × 9 at 4 bits ≈ 1.7 KB of game file, read from
the card into the battle band's draw like the sprites; no new static DRAM.

## 6. Auto-play

Each step teaches Auto-play (`monster_bot.c`) its effect, or Auto-play
plays worse with every effect switched on and the "game still plays" gate
stops being a measure of the effect. Each status needs at least: don't give
it to a critter that's immune or already has it; prefer a buff before
attacking and a debuff on a strong foe; cure or swap out of a status that
costs turns. The balance run uses the same policy, so it measures the
effect, not a confused bot.

## 7. Size and memory

- Flash and game file: the rules grow by a few KB; `game.bin` by the icons
  and the longer `STAT` rows. It's 807 KB in a 983 KB partition today.
- RAM: the battle gains a turn count and a giver per side (bytes). No new
  static buffers; `CLAUDE.md`'s DRAM rule holds.
- The save: version 5 adds a turn count only to statuses that outlast a
  battle; version 4 saves still load.

## 8. Decisions for the owner

1. The order (§4), and whether asleep and the other turn-losers (E2) come at
   all, or later.
2. The gate numbers (§3): 25 % on length, 10 points on win rate, 40 % on a
   move's use.
3. `splash` as plain damage in one-on-one battles.
4. The badge design (§5), and our own icons over Tuxemon's.
5. The left-out list (E6).
6. That no new status outlasts a battle.
