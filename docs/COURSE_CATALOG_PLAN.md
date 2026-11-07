# Course catalog plan — Study courses hosted on GitHub, fetched by HotSync

_Written 2026-11-07 at the owner's request ("evaluate if this makes sense and
write a plan only"). **Not approved. Nothing here is built.**_

## 1) The request

1. Host Study's SRS courses on GitHub, with a course catalog that links to
   them, also on GitHub. Suggest how the repos are split and laid out.
2. HotSync refreshes the catalog.
3. Study ▸ Menu ▸ Courses lists every course in the catalog with a status
   mark: on the card or not. Tapping one that isn't on the card toggles a
   pending mark; only one course can be pending. Tapping one that is on the
   card asks to delete it from the card.
4. The next HotSync downloads the pending course to the card, and Study can
   open it. Is a course of 2,000+ cards too much to stream to the card?

## 2) Verdict: it makes sense, and it fits what is already there

- **Courses are already data on the card** (`SRS_PLAN.md` §11 decision 2):
  one self-contained `course.srs` per course in `/sdcard/study/<id>/`, read a
  piece at a time, with progress kept apart and keyed by stable item ids, so
  a course can be replaced by a newer build without losing progress.
- **HotSync already streams files to the card in constant RAM.** The news
  feeds, the weather and the location reply all go through `dav_fetch_url()`
  (`firmware/components/bridge/dav_esp.c`): the body is written to a file as
  it arrives, in the HTTP client's 2 KB buffer, with no size cap in RAM. A
  course is the same request with a bigger file.
- **The reader can already check a file end to end.** `course_verify()`
  checks every section's and every item's CRC-32 with a ~4.8 KB scratch
  buffer, so a damaged or truncated download is caught before Study opens it.
- **The builder is already reproducible.** `tools/mkcourse.py` builds a
  course byte for byte from its source (pinned Pillow, pinned font), which is
  what makes building courses in CI and publishing them trustworthy.

What is new: a catalog file and its parser, one more HotSync stage, the
pending mark, and a Courses screen that merges the catalog with the card.

## 3) Is a 2,000-card course too much? No.

**Size.** Measured from the demo (64 items, 79,264 B): text averages about
150 B an item; a rendered kanji or kanji word about 300 B; a rendered example
sentence about 1,170 B. So:

| A 2,000-item course | Estimate |
|---|---|
| Latin-script vocabulary (text only, like the Albanian deck) | 0.3–0.5 MB |
| Japanese, every term drawn, no sentences | about 0.9 MB |
| Japanese, every term drawn, a sentence on 1,300 of them | about 2.4 MB |

**Time.** The download rate of this board over TLS onto its SPI card has not
been measured; 30–150 KB/s is the plausible range for an ESP32 with the SD
card on SPI. At the slow end 2.4 MB takes about 80 s, and a text deck a few
seconds. It is the first thing the bench must measure (§9, C3).

**RAM.** None that scales: the fetch writes as it receives, and verification
reads one item at a time. Study's own limits are far away: 65,535 items per
course, and `SRS_PLAN.md` §7 budgets opening a 9,000-item course in under a
second. Progress costs 16 B an item on the card (32 KB for 2,000).

**What does need handling:**
- **Never a half course on the card.** Download to `course.srs.tmp`, verify,
  then swap it in (`safefile.h`). A drop mid-download leaves the old course
  (or none) and keeps the course pending for the next HotSync.
- **Free space first.** Check the card's free space (`f_getfree`) against the
  catalog's size plus a margin before starting.
- **Long runs.** A one- or two-minute download is a long time to hold Wi-Fi
  on battery; the status line shows progress ("Kanji N5: 1.1 of 2.4 MB") and
  Cancel must be able to stop *inside* the download (a check in the HTTP
  client's data callback), not only between stages.
- **Resume** (an HTTP `Range` request on a retry) is a later refinement, not
  needed for courses this size.

## 4) The repos

**Recommendation: one catalog repo with GitHub Pages, one repo per course,
and the tools staying in this repo.**

```
emu-commits/cyd-palm-bridge          (this repo)
  tools/mkcourse.py                  the builder, as now
  tools/mkcatalog.py                 NEW: builds catalog.tsv from released courses
  .github/workflows/course.yml       NEW: a reusable workflow (workflow_call):
                                     build + --check a course at a pinned commit
  courses/demo-kanji/                the demo stays here (it ships in the firmware)

emu-commits/study-<id>               one per course, e.g. study-kanji-n5
  course.json, items.jsonl (or cards.tsv), ids.tsv, README.md, LICENSE
  .github/workflows/release.yml      on a tag: calls course.yml, attaches
                                     course.srs to the GitHub Release

emu-commits/study-catalog            the catalog, served by GitHub Pages
  sources.tsv                        hand-edited: which course repos, which tag
  .github/workflows/publish.yml      for each source: fetch the release asset,
                                     run mkcourse.py --check, copy it into the
                                     site, write catalog.tsv, deploy Pages
  site (built, not committed):
    catalog.tsv
    courses/<id>/<version>/course.srs
```

**Why this split:**
- **Licences differ per course**, and each course says its own (`META`). The
  demo is CC0; a KANJIDIC2-based deck is CC BY-SA; the owner's Albanian
  course is in its own repo already. One repo per course keeps each licence
  and README with its content. **WaniKani-derived content is never
  published** (`SRS_PLAN.md` §8): such a course stays on its author's card.
- **Courses version independently.** A fix to one deck is a tag on its repo;
  the catalog picks it up on its next publish.
- **The device talks to one host.** Everything it fetches is under
  `https://emu-commits.github.io/study-catalog/`: one TLS session for the
  whole stage, stable URLs, no redirects. Release-asset URLs would redirect
  to `objects.githubusercontent.com` (a second host, a second ~40 KB
  handshake on this heap) with long signed URLs.
- **The catalog CI is the gate.** Nothing reaches a device that
  `mkcourse.py --check` hasn't passed; the catalog records each file's size
  and CRC-32, and the device checks both.

**The simpler start, if preferred:** one `study-courses` repo holding the
courses' sources *and* the catalog, built and published by one workflow.
Fewer moving parts while every course is the owner's own; split a course out
when it gets its own maintainer or licence. The device side is identical.

## 5) The catalog file

Tab-separated text, because the device has no JSON parser and every other
file it fetches is line-oriented (Open-Meteo's CSV, RSS read a window at a
time). One course a line, at most 64 courses, at most 16 KB:

```
#cyd-study-catalog	1
#id	version	bytes	crc32	items	language	licence	title	url	about
kanji-n5	1.2	2412032	9a3c51e0	2004	ja	CC-BY-SA-4.0	Kanji N5	courses/kanji-n5/1.2/course.srs	The JLPT N5 kanji and ...
```

- `url` is relative to the catalog's own URL (or absolute https).
- The parser skips a line it can't read and refuses a file without the
  header; a refused catalog leaves the previous one in place.
- Stored as `/sdcard/study/catalog.tsv`, replaced whole after it parses.

## 6) HotSync

Two stages, after the weather (so a slow course never delays the news):

1. **Catalog.** Fetch `catalog.tsv` to a temp file, parse it, swap it in.
   Small; one request.
2. **The pending course, if there is one** (`/sdcard/study/pending.txt`
   holds its id). Look it up in the catalog, check free space, fetch to
   `/sdcard/study/<id>/course.srs.tmp`, check the size and CRC-32 against the
   catalog, `course_open()` + `course_verify()`, check the file's own id
   matches, swap it in, clear the pending mark. The status line names it.
   Any failure leaves the card as it was and the course pending, and says why.

The simulator's HotSync stub (`sim/stubs/hotsync_stub.c`) gets a fixture
catalog and copies a fixture course from the tree, so the whole flow is
gated without a network.

## 7) Study ▸ Menu ▸ Courses

One list: the catalog's courses and any course on the card that isn't in the
catalog (a sideloaded one, the demo), sorted by title. A drawn mark in the
first column, as Guru's and the memo checklist's boxes are drawn:

| Mark | Meaning | Tap the mark |
|---|---|---|
| empty box | in the catalog, not on the card | mark it pending (any other pending course is unmarked) |
| box with a down arrow | pending: downloads on the next HotSync | unmark it |
| filled box | on the card | "Delete <title> from the card?" (confirm / cancel) |
| filled box with a dot | on the card, the catalog has a newer version | the same choice, plus "Update" (marks it pending) |

**One deviation from the request, for a reason:** the request has a tap on a
downloaded course ask to delete it. But this screen is also how you *open* a
course today (the picker). The proposal follows the split the other lists use:
**the mark** is the action (pending, delete), **the name** opens the course.
Otherwise there'd be no way to switch courses from the list.

A footer line says what will happen: "Kanji N5 downloads on the next HotSync"
or "Tap a box to download a course".

**Delete:** removes `course.srs` and keeps the progress files (a few KB), so
downloading the course again carries on where it left off. Deleting the
progress too is one more confirm ("Also forget your progress?"). The demo can
be reinstalled from the menu as now.

## 8) Memory and code

- **Static DRAM:** nothing new beyond a few flags; the catalog is read from the
  card when the screen opens (64 lines at most) and freed when it closes.
- **Heap during the download:** the same as a news feed's fetch.
- **Code:** `study.c` gains `st_catalog_load()`, `st_pending_get/set()` and
  the merged list; `hotsync.c` two stages; `ui.c` the screen. `course.c` is
  unchanged.
- **Gates:** a host test for the catalog parser (truncations, bad lines, the
  64-line cap, a refused file leaving the old one), `make -C sim srs` for the
  pending/delete rules, the smoke tour for the screen and the stubbed
  download, and `tests/mkcatalog_test.py` for the catalog builder.

## 9) Phases

| Phase | What | Where it's verified |
|---|---|---|
| C0 | `mkcatalog.py`, the reusable `course.yml`, the `study-catalog` repo and its Pages site with the demo as its first course | CI; the catalog URL serves |
| C1 | The catalog parser and the HotSync catalog stage | host gate; `[d]` a real fetch |
| C2 | The Courses screen: marks, pending, delete | `[s]` smoke shots |
| C3 | The download stage: free space, temp file, size/CRC, `course_verify`, swap, cancel inside the download | `[s]` with the stub; `[d]` **the download rate**, a 2 MB course, a Wi-Fi drop mid-download |
| C4 | Updates: the "newer version" mark, update keeps progress | `[s]`, `[d]` |

## 10) Open questions for the owner

1. One repo per course plus a catalog repo (recommended), or one
   `study-courses` repo to start?
2. The mark/name split on the Courses screen (§7), rather than "tap a
   downloaded course to delete it"?
3. Delete keeps progress by default (§7)?
4. Which courses first? Candidates: the demo; a JLPT N5 kanji deck from
   KANJIDIC2/JMdict (CC BY-SA); the Albanian course from its own repo, if
   the owner wants it public.
