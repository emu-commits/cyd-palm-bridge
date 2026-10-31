# A longer Critters: plan (proposal, 2026-10-30)

> **Status: built 2026-10-31, in the simulator; the bench is next.** The
> owner said yes to all four decisions in §6, with one addition: Auto-play
> must find the store, buy, and use items. Built, one commit each: the
> engine (format 1.3), the looks stored once (2.0), three new areas, items
> and the store (2.1), and the acts. What was built differs from §3 in
> places: Act 2 also runs through the new areas, and the gauntlet is
> Dragon's Cave rather than Kestrel's Ring. `BUILD_PROGRESS.md` (2026-10-31)
> has the measurements: Auto-play finishes the game with every starter,
> its first critter at level 40–64.
> The ask (2026-10-30): make the game much longer and deeper without
> making the game file much bigger, so that the player's main critter is
> **above level 36** when the game ends.

## 1) Why the game is short now

Measured with `make -C sim monster`: Auto-play finishes the region with each
of the five starters, seed 1.

| Starter | Battles | Lost | Main critter at the end |
|---|---|---|---|
| Agnite | 244 | 7 | Agnite 15 |
| Anoleaf | 291 | 11 | Anoleaf 16 |
| Cardiling | 236 | 8 | Cardiling 15 |
| Vamporm | 65 | 0 | Fluttaflap 13 |
| Nut | 294 | 7 | Arthrobolt 18 |

The rules are Tuxemon's (`monster_play.c`):

- A level costs the cube: going from level *L* to *L*+1 needs about
  3*L*² experience (default group).
- A win against a level-*L* creature gives *L*², shared between the critters
  that fought it.

So a critter needs about three or four wins a level against its own level.
The region's wild critters stop at level 9, which gives at most 81 a win. At
level 15, a level costs about 700. The bot's last hundred battles hardly move
it, and the last keeper (Warden Ivy, 12/12/13) is beaten before it matters.
Agnite, Cardiling and Anoleaf evolve at 18 or 24, and again at 36. **None of
them evolves even once.**

The length problem is the level ceiling, not the number of maps.

## 2) Where the bytes are

`game.bin` is 663,544 B. The game partition is 960 KB, which leaves 319 KB.

| Part | Size | Notes |
|---|---|---|
| Character sheets, per area | 43–68 KB an area, 418 KB in all | Every area keeps its own copy of the player's six looks: 36 KB × 8 areas |
| Tiles and chunks | 116 KB | |
| Creature sprites | 112 KB | About 4 KB a creature |
| Creatures, moves, learnsets, encounter tables, text | 7.5 KB | |

People, encounter tables and words cost almost nothing. A **new area** costs
about 55–80 KB, mostly the sheets copied into it. That leads to the plan:
**go deeper in the areas we have, and add new areas only once the sheets
are shared** (§5, optional).

## 3) The plan: three acts on the same maps

The story already has hooks:

- the Cotton Tunnel's fallen rocks;
- "the store opens next season";
- the rest house's plates, with "a gap where Larkhollow's should be";
- Dr Wren, who is talked about but never met.

| Act | Opens with | Wild levels | Keepers | Main critter leaves at |
|---|---|---|---|---|
| **1** (as now) | A new game | 2–9 | Ada to Ivy, levels 4–13 | about 15 |
| **2: next season** | Beating Ivy (`beat_ivy`) | 12–22. First evolutions in the grass (Bolt, Dracune, Eyesore, Noctalo, Cardiwing, Gectile, Selmatek); Propellercat wild for the first time | All six road keepers come back with **three critters each, levels 16–24**, each built around a type. Pell and Juno at 23–25, then **Ivy's rematch at 26–28** | about 28 |
| **3: the Larkhollow plate** | Ivy's rematch (`beat_ivy2`) | 22–32, second stages rare | **The Ring gauntlet:** four keepers in a row on the Ring's stairs with no rest between them, 30–34. Then **Dr Wren** in the Lab, for the plate: Arthrobolt 38, Velocitile 39, Agnigon 40. Credits follow | 38–40 |

The experience works out:

- Going from 15 to 38 takes about 51,500 experience.
- In Act 3, a level-30 wild critter gives 900 a win, and each of the
  gauntlet's twelve critters gives 900–1,150.
- That's about 80–110 wins for the main critter, less with sharing. The game
  becomes **about 2½ times as long**, judged by the bot's battle count
  (roughly 600–700 battles instead of 240–290).

The 36 target matches Tuxemon's data: Agnidon, Cardiwing and Gectile evolve
at 36. Every starter's line will have reached its final form before Dr Wren.

**Depth as well as length:**

- **Teams built around a type.** Act 2's keepers each favour one type, so
  one strong starter can't carry the game, and catching and swapping matter.
- **The gauntlet** tests how well the player manages HP across battles,
  which nothing in Act 1 does.
- **Evolved critters in the wild** make catching worth it again late in the
  game.
- **Fast and slow lines even out.** Vamporm and Nut evolve at 9 and 12 and
  win Act 1 easily (Vamporm lost no battles). In a longer game, the slow
  lines catch up and overtake them at 36.

### What it costs in the file

| Addition | Size |
|---|---|
| About 15 new keepers (48 B each) and their words | about 3 KB |
| 6–8 new encounter tables and their flag steps | about 0.5 KB |
| Dr Wren's sheet, in the Lab only | 6 KB |
| **Total** | **about 10 KB (1.5 % of the file)** |

## 4) What has to be built

1. **Encounter tables that change with a flag** (format 1.3, `META` key
   `0x14`): a list of *(flag, table, replacement)*. Once the flag is set, a
   zone that names the table uses the replacement. A 1.2 reader skips the
   key and keeps Act 1's tables, so the game still works.
   - `monster_play.c` looks the table up through the list (about 30 lines);
   - `mkgame.py` writes it from a `"steps"` list in `game_region.json`;
   - both dumps print it.
2. **Rematches in the same cell.** Two people may share a cell when one is
   hidden by the flag that shows the other. `mkgame.py`'s "shares a cell"
   check and the reader's check learn this.
3. **The bot and its gate.** `make -C sim monster` already plays every
   starter to the end. It gains a check that **the main critter ends at 36
   or more**, and prints the battle count. That one gate tells us whether
   the balance holds. The bot must not chase keepers who aren't shown yet;
   `mb_done()` counts every trainer, and the search needs checking against
   `show_if`.
4. **The finale:** Dr Wren's battle, then the credits (the existing Credits
   screen) and a line from Mum.

The phases, one commit each, every gate run:

- **L1:** items 1–3 and the fixture's tests;
- **L2:** Act 2's content, with the bot gate at 26 or more after Ivy's
  rematch;
- **L3:** Act 3 and the finale, with the bot gate at 36 or more for all five
  starters;
- **L4:** the owner's bench.

## 5) Optional, after the acts

- **Items and the store** ("next season"): a tonic and a better capture
  ball, bought with money from keepers.
  - Data under 1 KB (no sprites: text only).
  - A bag in the save and an Item line in the battle menu: moderate code,
    and the bot must learn to use items.
  - Makes the gauntlet a real resource fight.
- **Store the player's looks once** instead of in every area. This is
  format **2.0**: an older firmware refuses the file. It saves about
  **258 KB**, which pays for **3–5 new areas** from Tuxemon's Spyder maps on
  tilesets already allowed (Cotton Town, Flower City, Candy Town and their
  routes), about 20–30 KB each once the sheets are shared. That is the way to
  a second region if one is wanted.
- **A bonus for beating keepers** (×1.5 experience, a `META` growth number)
  if the gates show the grind is too long. Off by default.
- **More creature lines**, about 4 KB a creature, from the allow-list's
  verified licences.

## 6) Decisions (the owner, 2026-10-31)

1. Build the three acts (§3–4): **yes**.
2. Items and the store: **yes**, and Auto-play must know how to go to the
   store, find its way round it, and use the items.
3. Format 2.0 (shared looks) and new areas: **yes**.
4. A bonus for beating keepers: **yes**.
