# Guru — editing the habit list

_Written 2026-09-19, when the habits moved out of the C source._

Guru's habits are a text file. You can change the wording, drop the ones that do
not apply to you, and add your own, without a toolchain and without a reflash.

## Where the list lives

There are two copies, and which one you are looking at matters:

| File | What it is |
| --- | --- |
| `firmware/main/guru_pool.txt` | **The source of truth in this repo.** Edit this to change the list that ships. |
| `firmware/main/guru_pool.c` | **Generated.** `tools/gen_guru_pool.py` compiles the `.txt` into it. Never hand-edit; CI regenerates and diffs. |
| `/sdcard/guru.txt` | **The device's own copy, if it has one.** Replaces the built-in list at startup. |

After editing `guru_pool.txt`:

```
tools/gen_guru_pool.py      # regenerate guru_pool.c
make -C sim gurupool        # the gate CI runs: generated file matches, parser agrees
```

## Editing it on the device

Since 2026-11-07 the list is edited on the device itself:

- **Change a habit:** open it from the list, tap **Edit**. The Graffiti strip
  comes up under a form: the category (tap it to step through the five), the
  name and the why. **Save** (where Edit was) writes the list to `guru.txt`
  on the card and goes back to the habit, with the strip hidden again.
- **Delete a habit:** **Delete** on the same form, then confirm. Days you
  already ticked it stay counted.
- **Add a habit:** **Menu > New habit** opens the same form, empty. Save adds
  it at the end of its category, if anything was written; a habit needs both
  a name and a why.

Each change writes the **whole** list, with the one change, to `guru.txt`
(replaced whole, so a power cut can't leave half a file) and reads it back,
so what Guru shows is always what the card holds. If the card already had a
`guru.txt` that Guru could not use (a typo on some line), it is kept as
`guru.txt.bak` rather than written over. A new habit takes the lowest free
id; a deleted habit's tick for today is cleared so a later habit given its
number starts unticked (the log's past rows keep their category, which is
what the week reads). The edited list stays in RAM for the rest of the boot.

## Editing it on a computer

1. In Guru, **Menu > Export habit list**. This writes `/sdcard/guru.txt`.
2. Put the card in a computer and edit that file in any text editor.
3. Put it back. Guru reads it when it next starts.

Export **will not overwrite** an existing `guru.txt`. Once the file is on the
card it is yours, and "export" must never be the gesture that throws away an
evening of editing. To start over, delete the file first — which is something
you can only do on purpose.

To go back to the built-in list entirely, delete `guru.txt` from the card.

## The format

```
id | category | name | why
```

```
1 | gut | One brazil nut | Brazil nuts are the densest food source of selenium...
```

- **id** — 1 to 64, and **permanent**. `guru.log` on the card stores this number,
  so a record written today has to still mean the same habit next year. Give a
  new habit the next free number. **Never reuse or renumber one**, or your own
  history will quietly re-label itself.
- **category** — one of `gut`, `metabolic`, `mind`, `strength`, `recovery`.
  These drive the week analysis; the headings you see on screen are separate
  words that can be reworded without touching your file.
- **name** — up to 30 characters, or the table row clips on a 240px screen.
- **why** — one or two sentences, shown when you tap the habit. The detail
  screen scrolls, so length is up to you.

Blank lines and `#` comments are ignored. **Order does not matter** — the list
is grouped by category when it is drawn, so you can write a habit down wherever
you happen to be in the file.

A `why` line cannot itself contain a `|`.

## When something is wrong with your file

Nothing breaks. A file that is missing, truncated, or edited into nonsense is
refused as a whole and the built-in list stays installed, so Guru always works.

To find out *why* a file was refused, open **Menu > About** inside Guru. It names
the line and the problem:

```
Your guru.txt was not used:
guru.txt: line 12: unknown category cardio
```

The list is rejected **all or nothing** on purpose. A parser that skipped the
bad lines would give you a list quietly missing habits you thought you had
added, and you would not find out until you noticed something was gone.

## A note on the copy

These are widely discussed consumer wellness practices. **They are not medical
advice**, and Guru's About box says so.

A `why` line may say what a practice is understood to do and why people do it —
a habit with no stated point is one nobody keeps. It should not name a disease,
promise to prevent or cure anything, or give a dose. "Builds aerobic base" is
fine. "Prevents heart disease" is not. "Take 5 g" is not.
