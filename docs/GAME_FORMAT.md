# Game format — `game.bin`, version 1

> **Status: G1 (2026-10-16).** `tools/mkgame.py` writes and reads it, and
> `firmware/main/monster.c` reads it in C (`monster_play.c` plays its rules);
> the two readers' dumps are compared byte for byte (`make -C sim monster`).
> G1 added `META` key `0x22` (§3.2). It may still change without a version
> bump until something ships.

`game.bin` holds everything the monster game reads from the card: the
rules' numbers, the creatures, their moves and battle sprites, the areas
(tiles, cells, warps, encounters, signs) and the credits. It's built on a
computer from [Tuxemon](https://github.com/Tuxemon/Tuxemon) by
`tools/mkgame.py` (`GAME_PLAN.md` is the plan). It follows `course.srs`
(`COURSE_FORMAT.md`): one file, a section table with CRCs, read a piece at
a time, never loaded whole.

## Contents

1. [Building the file](#1-building-the-file)
2. [The rules, as Tuxemon has them](#2-the-rules-as-tuxemon-has-them)
3. [The binary format](#3-the-binary-format)
4. [Colour: 8 bits for maps, 4 for creatures](#4-colour-8-bits-for-maps-4-for-creatures)
5. [Limits](#5-limits)
6. [What the reader checks](#6-what-the-reader-checks)
7. [Changing the format](#7-changing-the-format)

---

## 1) Building the file

```
pip install -r tools/requirements-game.txt    # Pillow and PyYAML, pinned
python3 tools/mkgame.py --fetch               # Tuxemon at the pinned commit, into ~/.cache/cyd-palm/tuxemon
python3 tools/mkgame.py -o game.bin --report  # build, and say what was left out
python3 tools/mkgame.py --check game.bin      # validate, print sizes
python3 tools/mkgame.py --dump game.bin       # the canonical text dump (G1's C reader must match it)
python3 tools/mkgame.py --preview DIR game.bin  # the areas and sprites as the device draws them
```

Four files in `tools/` decide what's built:

| File | What it holds |
|---|---|
| `mkgame.py` | The pinned Tuxemon commit (`TUXEMON_COMMIT`, the full SHA). The build refuses a checkout at any other commit, or with local changes under `mods/`. |
| `game_allow.tsv` | The licence allow-list (`GAME_PLAN.md` §10). **Only what it lists is built**: tilesets, creatures and character sheets, each with licence, authors, source and a status (`credited` from Tuxemon's `ATTRIBUTIONS.md`, `unverified`, or `confirmed` by hand). A map cell from an unlisted tileset loses that layer's tile, and the build says how many. |
| `game_region.json` | The region: which maps, their names, where the player starts, the player's sheet, and the encounter tables. The tables are ours, not Tuxemon's, because Tuxemon's name creatures the allow-list doesn't have. |
| `game_ids.tsv` | Stable numbers for creatures, moves, statuses and areas, as `ids.tsv` is for courses. A save refers to these, so a rebuild that adds content keeps old saves working. `mkgame.py` appends new numbers and never renumbers. **Commit it.** |

**Reproducible:** two builds from the same inputs are identical to the byte
(checked: both depths, built twice, compared with `cmp`). Nothing depends on
the time, the machine or dict order; Pillow (12.3.0) and PyYAML (6.0.2) are
pinned; the palette reduction is Pillow's median cut with fixed k-means
passes and no dithering.

**The test fixture** (`tests/data/game/`) is a build of our own content, so
the gates never need Tuxemon: `source.py` writes a small tree in Tuxemon's
layout (five invented creatures, ten moves, two maps, tiles and sprites drawn
from numbers, all CC0), and `mkgame.py --unpinned` builds it, naming the
source in `META` instead of a Tuxemon commit. `tests/mkgame_test.py` rebuilds
it byte for byte; `--unpinned` is for that and nothing else.

**What the build takes from Tuxemon:**

| From | What |
|---|---|
| `mods/tuxemon/db/element/*.yaml` | The 13 types and their multipliers |
| `mods/tuxemon/db/shape/shapes.yaml` | The 14 body shapes' six base stats |
| `mods/tuxemon/db/status/` | `poison`, `burn`, `recover` (the first version's statuses) |
| `mods/tuxemon/db/monster/*.yaml` | Allow-listed creatures: types, shape, stage, catch rate and resistances, experience group, evolution, learnset |
| `mods/tuxemon/db/technique/*.yaml` | The moves those creatures learn, if their effects are ones we play (§2) |
| `mods/*.yaml` | `config_combat`, `config_capture`, `config_monster`, `range_map`: the rules' constants |
| `mods/tuxemon/maps/*.tmx` | The region's maps: tile layers, tile properties, collision objects, events |
| `mods/tuxemon/gfx/sprites/battle/<slug>-sheet.png` | Battle sprites: the 64×64 front at (0,0), the 64×64 back at (64,0) |
| `mods/tuxemon/sprites/<name>.png` | Character sheets: 48×128, twelve 16×32 frames, three per direction |
| `mods/tuxemon/l18n/en_US/LC_MESSAGES/base.po` | English names and dialogue |

**What it doesn't take:** Tuxemon's event scripts beyond the four verbs in
§3.11 (warps, encounters, signs), its NPCs, items, music and sounds. The
build lists every event verb it skipped, per area.

## 2) The rules, as Tuxemon has them

Read from Tuxemon's source at the pinned commit (`tuxemon/formula.py`,
`monster/stats.py`, `monster/experience.py`, `combat/`, `core/effects/`).
G1 implements them in **integer arithmetic** with a seeded generator, so a
battle replays exactly; where Tuxemon uses floats, G1 computes the same
value exactly and the result can differ from Tuxemon's by rounding only.
The constants are in `META` (§3.2), so they're the file's, not the code's.

**Stats.** Each of hp, armour, dodge, melee, ranged and speed is

    stat = shape_value × (level + 7) + iv

`shape_value` is from `SHAP`; `iv` is a number from 0 to 15 drawn for each
stat when a creature is made (Tuxemon's `iv_range`). Tuxemon also adds
training points and taste modifiers; the first version has neither. Max HP is
the hp stat.

**Damage.** A move's range picks the stats (Tuxemon's `range_map.yaml`):

| Range | Attacker's strength | Defender's resistance |
|---|---|---|
| melee | melee × (7 + level) | armour |
| touch | melee × (7 + level) | dodge |
| ranged | ranged × (7 + level) | dodge |
| reach | ranged × (7 + level) | armour |
| reliable | 7 + level | 1 |
| special | no damage | |

    multiplier = the product of TYPE[move type][defender type] over the
                 move's types and the defender's types, clamped to 0.25-4
    damage     = floor(strength × power × multiplier / max(1, resistance))

The 7 is Tuxemon's `COEFF_DAMAGE`. `power` is a decimal (1.0 is a basic
move), stored in hundredths. **Dodge doesn't decide a miss**: it's only the
resistance to touch and ranged moves.

**Hitting.** One random number *r* in [0, 1) is drawn per creature per turn;
a move hits when `accuracy ≥ r`. A move that gives a status draws a second
number *p* and gives it when it also hits and `potency ≥ p`. G1 draws *r* and
*p* in ten-thousandths and hits when `r < accuracy` (the same but for the
one chance in 2^53 that Tuxemon's float lands exactly on it).

**Healing.** A healing move restores `floor(7 + level × healing_power)`.

**Statuses** (the first version's three):

| Status | Each turn | Immune | After the battle |
|---|---|---|---|
| poison | loses floor(max HP / 8) | Venom types | stays until healed |
| burn | loses floor(max HP / 8) | Fire types | stays until healed |
| recover | gains min(max HP / 16, what's missing); ends at full HP | | ends |

A creature has one status at a time; a new one replaces it (Tuxemon's
`on_negative_status: replaced`). Statuses act at the end of each turn, after
both moves.

The divisors, immunities and persistence come from Tuxemon's status files
(`STAT`). Tuxemon also lowers HP on the overworld every 10 steps while
poisoned (`step_interval`); the first version doesn't.

**Order of a turn.** The faster move goes first:

    speed_score = floor(max(1, speed × (1 + move_speed × 0.25)) + dodge × 0.01)

`move_speed` runs from −3 (extremely slow) to +3 (extremely fast). **A tie
is a coin toss** (Tuxemon sorts on a random `sub_priority` last).
`config_combat.yaml`'s `speed_offset` (50) is used only by Tuxemon's tests,
not its battles, so G1 leaves it out. Catching, running and swapping come
before the other side's move.

**Recharge.** A move used can't be used again for `recharge` turns.

**Met in the wild.** The level is drawn evenly from the table row's range,
each IV evenly from 0 to the maximum (`META` `0x22`), the experience is the
level's floor, and the moves are **the last four its learnset reaches by its
level** (Tuxemon's `set_moves`), not only the level-1 ones.

**Catching.** With *HP* the target's max HP and *cur* its current HP:

    catch_check = (3 × HP − 2 × cur) × catch_rate × status_mod × device_mod / (3 × HP)
    shake_check = 524325 / (8 × (100 / catch_check)^(1/4)) × u,
                  u drawn evenly from [resist_low, resist_high]

Then up to 4 shakes: each draws an integer from 0 to 65536 and the
creature breaks free if it's above `floor(shake_check)`. At a catch rate of
100 and both modifiers 1, `shake_check` is 65,540 before *u*. A catch rate
of 0 would divide by zero in Tuxemon; G1 treats it as uncatchable. The
first version has one capture device and no status modifiers, so both
modifiers are 1. G1 computes the fourth root exactly in integers (`catch_check
/ 100` scaled by 10^16, two integer square roots): for a full-HP creature
with a rate of 90 that's 48,500 against Tuxemon's 48,505.

**Running away** (wild battles):
`chance = 0.4 + 0.15 × (attempts + own level − wild level)`.

**Experience.** The total to reach level *L* is `floor(m × L³)`, with *m*
from the creature's experience group: default 1.0, fast 0.8, slow 1.25.
Beating a creature at level *L* gives `floor(its total / L)` (about *L*²
for one met at its level's floor), shared evenly among the creatures that
fought it and are still standing. Levels run from 1 to 100. The
multipliers are the file's (`META` `0x22`), in hundredths, so the totals are
exact.

**Evolving and learning.** At the level in `MONS`, a creature becomes the
one named there (keeping its experience). At each level in its learnset, it
learns that move; with four already, the battle says so (`MP_EV_LEARN_FULL`)
and the screen asks which to forget, or none (`mp_learn()`). HP rises by what
the maximum rose by. Evolving happens at or after the level, so a creature
caught above it evolves at its next level-up.

**Losing.** When every creature in the party has fainted, the party is
healed and the player is put back at the start. (Tuxemon sends the player to
the last healing centre; the first region has none yet.)

**Moves left out.** A move is built only if every effect it has is
`damage`, `healing` (on itself or the target) or `give` of one of the three
statuses to one target. For the spike's 20 creatures, that's **27 moves
kept and 49 left out** (splash damage, multi-attacks, enraged, noddingoff,
stat changes and so on). Every creature still has a move at level 1, which
the build checks. More effects are a later decision (`GAME_PLAN.md` §12).

## 3) The binary format

**All integers are little-endian.** Offsets are counted from the start of
the file (sections) or the start of the section or block they're in.
Every section starts on a multiple of 4; the bytes between sections, and
after a record that's padded, are zero. CRC-32 is zlib's, as in
`COURSE_FORMAT.md` §3.

A **string reference** (8 B) is u32 offset into `TEXT`, u16 length, u16 0.
Text is UTF-8 with every character in U+0020–U+00FF (the Palm fonts),
folded with the course builder's rules (curly quotes to straight, dashes to
`-`, and so on).

### 3.1 The file

```
header         32 B
section table  16 B x 13
META TYPE SHAP STAT MOVE MONS LRNS ENCT AREA ADAT SPRT TEXT CRED   (in this order)
```

**Header (32 B, at 0):**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `CYDG` |
| 4 | 2 | Format major version: `1`. A reader refuses any other. |
| 6 | 2 | Format minor version: `0` |
| 8 | 4 | File size in bytes |
| 12 | 2 | Section count |
| 14 | 2 | Reserved, 0 |
| 16 | 4 | Creature count |
| 20 | 4 | Area count |
| 24 | 4 | CRC-32 of the section table: the file's **fingerprint** |
| 28 | 4 | CRC-32 of bytes 0–27 |

**Section table (16 B per section):** tag, u32 offset, u32 length, u32
CRC-32 of the section's bytes. All 13 are required. A reader skips a tag
it doesn't know.

### 3.2 `META`

Entries one after another: u16 key, u16 length *n*, *n* bytes of value.

| Key | Value |
|---|---|
| `0x01` | Title, text |
| `0x02` | Source: `Tuxemon <full commit SHA>` |
| `0x03` | Builder: `mkgame <major>.<minor>` |
| `0x10` | Start: u16 area (its position in `AREA`), u8 x, u8 y, u8 facing (0 N, 1 E, 2 S, 3 W), 3 B zero |
| `0x20` | Rules, u32 × 8: COEFF_DAMAGE (7), stat coefficient (7), multiplier minimum and maximum in hundredths (25, 400), experience exponent (3), maximum level (100), moves known (4), party size (6) |
| `0x21` | Catching, u32 × 7: shakes (4), shake constant (524325), shake denominator (8), shake divisor (65536), HP multiplier (3), current-HP multiplier (2), HP divisor (3) |
| `0x22` | Growth, u32 × 4: the IVs' maximum (15), then the experience groups' multipliers in hundredths: default (100), fast (80), slow (125). Added in G1. |

`0x10`, `0x20`, `0x21` and `0x22` are required, and each may appear once. The
reader refuses values that would overflow its arithmetic (`monster.c`,
`parse_meta()`).

### 3.3 `TYPE`: types

u16 count *n* (13), u16 0, then *n* string references (the names), then an
*n* × *n* table of u8, **attacker's type by row, defender's by column**, in
quarters: 2 = ×0.5, 4 = ×1, 8 = ×2. Types are numbered by position, in the
order of Tuxemon's slugs (cosmic, earth, fire, frost, heroic, lightning,
metal, normal, shadow, sky, venom, water, wood). 255 means "no type" in a
record's second type.

### 3.4 `SHAP`: body shapes (8 B each)

u8 hp, armour, dodge, melee, ranged, speed; 2 B zero. Numbered by position,
in the order of Tuxemon's slugs (blob, brute, dragon, flier, grub, humanoid,
hunter, landrace, leviathan, piscine, polliwog, serpent, sprite, varmint).

### 3.5 `STAT`: statuses (24 B each)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Id (`game_ids.tsv`) |
| 4 | 1 | Kind: 1 poison (loses HP), 2 burn (loses HP), 3 recover (gains HP) |
| 5 | 1 | Divisor: the HP change each turn is max HP / divisor |
| 6 | 1 | Flags: bit 0 negative, bit 1 persists after a battle |
| 7 | 1 | 0 |
| 8 | 2 | Immune types: bit *t* set means type *t* is immune |
| 10 | 2 | 0 |
| 12 | 8 | Name |
| 20 | 4 | 0 |

### 3.6 `MOVE`: moves (40 B each)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Id |
| 4 | 1 | Type (255 none) |
| 5 | 1 | Second type (255 none) |
| 6 | 1 | Range: 0 melee, 1 touch, 2 ranged, 3 reach, 4 reliable, 5 special |
| 7 | 1 | Speed: 0 (extremely slow) to 6 (extremely fast); 3 is normal |
| 8 | 2 | Power, hundredths |
| 10 | 1 | Accuracy, percent |
| 11 | 1 | Potency, percent |
| 12 | 1 | Recharge, turns |
| 13 | 1 | Targets: bit 0 the enemy, bit 1 itself |
| 14 | 2 | Healing power, hundredths |
| 16 | 12 | Three effects of 4 B, in order (kind 0 = none): u8 kind (1 damage, 2 healing, 3 give status), u8 status (its position in `STAT`, for give), u8 target (0 the enemy, 1 itself), u8 0 |
| 28 | 4 | 0 |
| 32 | 8 | Name |

### 3.7 `MONS`: creatures (48 B each)

Sorted by Tuxemon's `txmn_id`, then slug. A creature is referred to by its
position here everywhere in the file; a save uses its id.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Id |
| 4 | 2 | Tuxemon's `txmn_id` (its number in Tuxemon's Tuxepedia) |
| 6 | 1 | Type |
| 7 | 1 | Second type (255 none) |
| 8 | 1 | Shape (position in `SHAP`) |
| 9 | 1 | Stage: 0 basic, 1 stage 1, 2 stage 2, 3 standalone |
| 10 | 1 | Catch rate, 0–100 |
| 11 | 1 | Experience group: 0 default, 1 fast, 2 slow |
| 12 | 1 | Catch resistance, low, hundredths |
| 13 | 1 | Catch resistance, high, hundredths |
| 14 | 1 | Evolves at this level (0 never) |
| 15 | 1 | Learnset length |
| 16 | 2 | Evolves into (position in `MONS`; 65535 none) |
| 18 | 2 | 0 |
| 20 | 4 | Learnset's first entry in `LRNS` |
| 24 | 4 | Sprite's offset in `SPRT` |
| 28 | 4 | Sprite's length |
| 32 | 8 | Name |
| 40 | 8 | 0 |

### 3.8 `LRNS`: learnsets (4 B each)

u8 level, u8 0, u16 move (position in `MOVE`). A creature's entries are
consecutive, sorted by level. Level 1 entries are what it knows when met.

### 3.9 `ENCT`: encounter tables

u16 table count *n*, u16 0; then *n* × (u16 first row, u16 rows); padded to
4; then the rows, 8 B each: u16 creature (position in `MONS`), u8 lowest
level, u8 highest level, u16 rate in hundredths, 2 B zero. A table's rates
are weights: a wild creature is chosen in proportion to them.

### 3.10 `AREA`: the area directory (32 B each)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Id |
| 4 | 2 | Width in cells |
| 6 | 2 | Height in cells |
| 8 | 4 | Offset of its block in `ADAT` |
| 12 | 4 | Length of its block |
| 16 | 4 | CRC-32 of the block's head |
| 20 | 8 | Name |
| 28 | 4 | Length of the head |

Warps and the start refer to an area by its **position** here, not its id.

### 3.11 `ADAT`: area blocks

Each block starts on a multiple of 4 and has four parts:

```
head        read whole when the area is entered (0.6-2.4 KB in the spike)
tiles       16 x 16 pixels each, read one at a time into a cache
characters  the area's character sheets, already in its palette
chunks      16 x 16 cells each, read as the player nears them
```

**The head** starts with 64 B:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Width in cells (1–255) |
| 2 | 2 | Height in cells (1–255) |
| 4 | 1 | Bits per pixel: 8 or 4 (§4) |
| 5 | 1 | Colours *c* in the palette (1 to 2^bpp − 1) |
| 6 | 2 | Tile count *t* |
| 8 | 2 | Chunk columns |
| 10 | 2 | Chunk rows |
| 12 | 2 | Warps |
| 14 | 2 | Encounter zones |
| 16 | 2 | Signs |
| 18 | 2 | Overlays |
| 20 | 2 | Character sheets |
| 22 | 2 | 0 |
| 24 | 4 | Offset of the tiles, from the block's start |
| 28 | 4 | Length of the tiles |
| 32 | 4 | CRC-32 of the tiles |
| 36 | 4 | Offset of the character sheets |
| 40 | 4 | Length of the character sheets |
| 44 | 4 | CRC-32 of the character sheets |
| 48 | 4 | Offset of the first chunk |
| 52 | 12 | 0 |

Then, in order, each list right after the one before:

1. **The palette:** *c* + 1 entries of u16 RGB565, padded to 4. Entry 0 is
   transparent (its value is 0 and isn't drawn). A pixel index above *c*
   draws black.
2. **Tile flags:** *t* + 1 bytes (tile 0 first), padded to 4. Bit 0: hop
   (a ledge one jumps down); the rest are 0.
3. **Warps, 12 B each:** u8 x, u8 y, u8 width, u8 height (a rectangle of
   cells), u16 destination area (position in `AREA`), u8 x, u8 y, u8 facing
   needed to take it (0–3, or 255 any), u8 0, u16 0. Stepping into the
   rectangle while facing that way moves the player to the destination.
4. **Encounter zones, 8 B each:** x, y, width, height (u8 each), u16 table
   (position in `ENCT`), u16 chance in hundredths of a percent. Each step
   inside starts a wild battle with that chance (Tuxemon's `total_prob`: 1 %
   unless the map says otherwise; Route 1's grass says 11 %), and the
   table's rates then choose the creature.
5. **Signs, 16 B each:** x, y, width, height, u8 facing needed (255 any),
   u8 0, u16 0, then the text as a string reference. Pressing A while
   facing into the rectangle shows the text.
6. **Overlays, 4 B each,** sorted by cell: u16 cell (y × width + x), u16
   tile. These are the tiles drawn **over** the player (tree tops, roof
   edges): Tuxemon's tile layers from the fourth on.
7. **The chunk directory, 8 B per chunk,** row by row: u32 offset from the
   block's start, u32 CRC-32 of the chunk.

The head's length (in `AREA`) is up to here, and its CRC covers all of it.

**Tiles:** *t* tiles of 16 × 16 pixels, rows top to bottom, numbered from 1
(tile 0 is "nothing"). At 8 bits a pixel is one byte; at 4 bits two pixels
share one, **the left one in the high nibble** (LVGL's `I4` layout). Each
is 256 B or 128 B. Index 0 is transparent: below the player a tile is
opaque (Tuxemon's first three layers composited over black), and an overlay
tile is transparent wherever nothing is drawn.

**Character sheets:** for each, 12 frames of 16 × 32 pixels in the area's
palette and depth, in Tuxemon's order (rows of three: walking frames for
each direction). The first sheet is the player's.

**Chunks** (768 B each): 16 × 16 cells, row by row, as 256 u16 tiles
(below the player) followed by 256 movement bytes. A chunk past the
area's right or bottom edge is padded with tile 0 and movement `0x0F`. The
cell at (x, y) is in chunk (x / 16, y / 16).

**The movement byte** says which sides of a cell can be crossed:

| Bits | Meaning |
|---|---|
| 0–3 | Can't **enter** across the N, E, S, W side |
| 4–7 | Can't **leave** across the N, E, S, W side |

A step from cell A to cell B in direction *d* is allowed when B is inside
the area, A's leave bit for *d* is clear, and B's enter bit for the side it
faces A is clear. A solid cell (a wall, a tree trunk, water) is `0x0F`.
`mkgame.py` builds it the way Tuxemon's collision map does:

- a tile with `surfable`, `walkable` or `climbable` at 0 (water, to someone
  who can't swim) is solid; there's no swimming in the first version;
- a tile's `enter_from`, `exit_from` and `endure` properties limit the
  sides (a tile with `enter_from` of `down` only can be entered from below);
  a later layer's tile wins;
- a collision rectangle from the map's objects makes its cells solid, or
  applies its own `enter_from`/`exit_from`;
- a collision line along a cell edge blocks crossing that edge both ways.

### 3.12 `SPRT`: battle sprites

One record per creature, each starting on a multiple of 4:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Width (64) |
| 1 | 1 | Height (64) |
| 2 | 1 | Bits per pixel (4) |
| 3 | 1 | Frames (2: front, back) |
| 4 | 4 | CRC-32 of the rest of the record |
| 8 | 32 | Palette: 16 × u16 RGB565, entry 0 transparent |
| 40 | 2,048 × frames | The frames, 4-bit pixels as in tiles |

4,136 B per creature: 20 creatures are 82,720 B, all 411 of Tuxemon's
would be about 1.7 MB.

### 3.13 `TEXT`

The strings, back to back, shared where they're equal. Checked by its
section CRC on open: it's small (567 B in the spike; signs and the story
will make it a few KB).

### 3.14 `CRED`

The credits screen's lines, separated by `\n`, built from the allow-list
lines actually used: the Tuxemon commit, then each creature, tileset and
character sheet with its authors and licence.

## 4) Colour: 8 bits for maps, 4 for creatures

**This settles `GAME_PLAN.md` §13 Q4, subject to the owner's review.** The
spike built two real areas, Route 1 and Brideswood from Tuxemon's Spyder
maps, both ways, and drew them from the built file, pixel for pixel as the
device would, through RGB565:

- **At 4 bits (one palette of 15 colours per area), the map loses
  information the game needs.** The tall grass where encounters happen
  loses its yellow-green and looks like the trees' foliage; the red and
  yellow flowers turn brown and white. Weighting the palette by distinct
  colours instead of by pixels kept a little more of the flowers but banded
  Brideswood's ground into two greens. Both were rejected.
- **At 8 bits the maps are exact:** after RGB565, Route 1 uses 65 colours
  and Brideswood 33, so nothing is reduced. The cost on the card is small
  (Route 1's tiles are 14 KB instead of 7 KB), and in RAM it depends on how
  G2 draws (below).
- **Battle sprites stay at 4 bits with their own palette.** 9 of the 20
  spike creatures have more than 15 colours (Pigabyte 38, Fluttaflap 32,
  Agnite 21); reduced, they look the same as the originals at 3× zoom.

**What 8-bit maps mean for the heap (for G2 to measure).** `GAME_PLAN.md`
§9 budgeted a 4-bit canvas (21 KB). An 8-bit canvas of the 240 × 176 view
is 42 KB, and with the rest (chunks 3 KB, a 64-tile cache 16 KB, the player
6 KB, the head up to 3 KB, state 4 KB) that's about 74 KB, too close to the
largest free block (about 90 KB). Drawing tiles straight into LVGL's own
draw buffer from a cache of RGB565 tiles (512 B each, 64 of them = 32 KB)
needs no canvas, and comes to about 48 KB. G2 measures both; the file
doesn't change either way, since the depth is per area.

**The tile cache.** The most different tiles on any one screen, with the row
and column scrolling in, was 34 (Route 1) and 13 (Brideswood), so a cache of
64 is ample for these maps. `mkgame.py --report` prints it for every area.

## 5) Limits

| | Limit | Why |
|---|---|---|
| Area | 255 × 255 cells | Warps, zones and signs use u8 coordinates; the overlay list a u16 cell number |
| Tiles in an area | 4,095 | u16 in a cell; the top 4 bits are kept for animation later |
| Colours in an area | 255 (8 bits) or 15 (4 bits) | Index 0 is transparent |
| Creatures, moves, areas | 65,535 each | u16 positions |
| Learnset | 255 entries | u8 length |
| Effects per move | 3 | |
| Text field | 65,535 B | u16 length |
| The file | 4 GB | u32 offsets; FAT32's limit |

## 6) What the reader checks

The reader treats the file as untrusted, as `course.c` does. G1's C reader
does all of the following, and `monster_fuzz` proves it under ASan and
UBSan (with each file that opens then played: steps, battles, a save and a
load). `monster.c` does all of it; `mkgame.py`'s reader does the file, area
and picture checks.

- **On open:** the magic, major version, header CRC and file size; the
  section table's CRC, every section inside the file, 4-aligned, in order,
  not overlapping, with zero padding between; the CRCs of every section
  except `ADAT` and `SPRT` (all small: 4.5 KB in the spike); record sizes
  whole; every position (types, shapes, moves, creatures, statuses, tables,
  areas) in range; every string reference inside `TEXT` with only
  U+0020–U+00FF in it.
- **On entering an area:** the head's CRC, and that its lists exactly fill
  it; each chunk's CRC when it's read; each cell's tile at most *t*; each
  warp's destination in range.
- **On each picture:** the tiles' and the character sheets' CRCs are checked
  by the whole-file check, not per tile (a damaged tile draws wrong colours,
  never reads outside its buffer); a sprite's record CRC before it's drawn.
- **The whole file** (`--check`, and a "Check game" menu item later):
  everything above, for every area and sprite.

`mkgame.py --check` on the spike file refused all of 1,000 truncations and
3,000 single-bit flips, without a crash.

## 7) Changing the format

As for courses (`COURSE_FORMAT.md` §7): new `META` keys, new sections and
meanings for reserved bytes are minor versions an older reader can ignore;
anything an older reader would misread is a major version; a tag, key or
bit is never reused.
