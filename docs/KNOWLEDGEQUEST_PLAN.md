# KnowledgeQuest plan — exploring a graph of knowledge on the CYD

_Written 2026-11-07 at the owner's request: evaluate the opportunity brief
(`KNOWLEDGEQUEST_BRIEF.md`) and write a plan._

**Status, 2026-11-07 (later the same day): K0 is built in the simulator, not
yet on the device.** The owner chose the launcher tile (question 1 of §10):
position 6 is **Knowledge**, a smiling tree (`tools/gen_knowledge_icon.py`),
and the first test is 40 steps in one line, each building on the one before
(`courses/kq-numbers/`, from counting to percent change). What was built:

- `tools/mkquest.py` (§5): a graph of concept cards (`quest.json` +
  `nodes.jsonl`) to a Study course source; it refuses unreviewed cards unless
  `--drafts` is given, and then marks the course `-draft`.
- The pilot's 40 cards, **written by Claude and not yet reviewed by a person**
  (question 3 of §10 is still open). Every answer was checked by hand.
- Knowledge opens Study's engine on that one course, built into the firmware
  like the demo and written to the card on first open. Because a tile needs
  screens of its own, three small pieces of K1 came forward: a step's check
  asks its own question and shows its own answer (not "what does the term
  mean"); one step a lesson; and the end of a step says what was explored,
  what it opened, and how many lie beyond (the fog, as a count).
- Reviews stay on, lightly (question 4 answered by default: 1, 3, 7, 21 and
  60 days, then retired). Say if Explore should be pure discovery instead.

Not built: the reverse-link section (format 1.1), the map, the region list,
anything past one course. The verdict below said "inside Study"; the owner
chose the tile, which §9 had kept open as the fallback, and it shares
`course.c`, `srs.c` and Study's screens as §9 described.

## 1) The brief, in one paragraph

Learning as navigation of a prerequisite graph of concepts. A session teaches
one tiny node — one idea, one explanation, one diagram, one question — in 30 s
to 3 min, then shows what it unlocked. No courses, no feed, no notifications;
offline, on dedicated hardware. A game layer: a fog over what lies further on,
a skill-tree feel, a personal map of what you know. At scale 20,000–50,000
concepts across the sciences and humanities, built with LLM help and curated by
people. The brief points at `withmarbleapp/os-taxonomy` for content and
concepts.

## 2) Verdict

**It fits this device and this project unusually well, and most of the engine
already exists. Build it as an "Explore" mode of Study, not as a new app, and
start with a pilot that needs no firmware change at all.**

Why it fits:
- **The direction the owner just set.** Date Book, Address and To Do went
  because competing with a phone is pointless (2026-11-07). A device for
  curiosity, offline, one idea a day, is the thing a phone is bad at. It is
  the same argument the brief makes, and the one Coach already makes for focus.
- **The hardware matches the brief's session.** One idea, one 1-bit diagram,
  one self-graded question on a 240×320 screen is what Study's lesson card
  already is. No typing (`SRS_PLAN.md` §11 decision 9) matches "one question".
- **Study's course format is already a prerequisite graph.** An item's
  `built_from` and `unlock_after` links gate its lesson until the linked items
  are known (`unlock.by_links`); levels group items; the reader verifies
  every byte; courses are data on the card, built on a computer, and Phase 2
  (`COURSE_CATALOG_PLAN.md`) would deliver them over HotSync.
- **Spaced repetition is the missing half of the brief.** The brief optimises
  discovery and dismisses flashcards; but a node "mastered" once and never seen
  again is forgotten. Study's schedulers can keep what was explored, quietly,
  while Explore chooses what is new.

What doesn't fit, and has to be designed around:
- **Scale.** 50,000 nodes in one course is inside the format's limit (65,535
  items) but far outside Study's measured budget (open a 9,000-item course in
  under a second) and its progress file (16 B an item: 800 KB). And working out
  "what does this node unlock" means scanning every item's links today: the
  format stores the edges one way only.
- **A map of thousands of nodes on a 240-pixel mono screen.** A whole-graph view
  isn't legible; the map has to be local (a node and its neighbours) or
  summarised (how much of each region you've explored).
- **Content.** The brief's node needs an explanation, a diagram, a question and
  an answer. The taxonomy has none of the four in that form (§3). This is the
  real cost of the project, not the firmware.
- **"Sync knowledge externally (GitHub?)"**: HotSync only *reads* now (the
  transport allows GET only). Writing to GitHub means a token on the device and
  a write path, which is a security decision for the owner (§8, K4).

## 3) The taxonomy (`withmarbleapp/os-taxonomy`), measured

Read 2026-11-07 through the GitHub API: **v1, 1,861 micro-topics, 4,003
prerequisite edges, 9 subjects** (Mathematics 581, Science 576, English 339,
Personal & Social 118, History 109, Computing 52, Life Skills 46, Geography 22,
Learning to Learn 18), ages about 4 to 12. `topics.json` is 2.3 MB,
`dependencies.json` 0.9 MB.

- **A topic** has an id, a type (conceptual, procedural, ...), subject and
  domain, a name, a plain-language description, mastery "evidence" bullet
  points, an age range, a centrality score, an `assessmentPrompt` written for a
  parent ("If {{name}} says ..."), and curriculum codes.
- **An edge** is `topicId` depends on `prerequisiteId`, `hard` or `soft`, with
  a one-line reason. The graph is acyclic.
- **Licences:** the database (structure, ids, relationships) is **ODbL 1.0**;
  the text Marble wrote (names, descriptions, evidence, prompts, reasons) is
  **CC BY-SA 4.0**; the curriculum standards keep their own upstream licences.
  Commercial use is allowed with attribution; a course built from it is a
  derivative database and must stay ODbL, its text CC BY-SA, both credited.
  That is compatible with publishing such a course in the Phase 2 catalog; the
  firmware (GPLv3) is a separate work that reads data from the card.

**What it is good for here:** a well-made, open graph to build and test the
engine on, and a real content source for a **children's** edition (the brief
names childhood learning as a fit for offline hardware).

**What it isn't:** the brief's adult "curious person" graph. It stops at about
age 12, it has no lesson text, no diagrams and no learner-facing questions
(`evidence` is a checklist for an adult; `assessmentPrompt` addresses a parent).
Every node would need its card written.

## 4) How it maps onto Study

| The brief | In Study today | What Explore adds |
|---|---|---|
| A node: id, title, summary, diagram, question, answer, prerequisites | An item: id, term, text fields, pictures, `built_from`/`unlock_after` links | A `concept` kind whose card is explanation → diagram → question |
| Prerequisites gate a node | `unlock.by_links` | Nothing |
| "Available paths" | The lesson queue (everything unlocked) | A frontier list chosen by the learner, not served in order |
| "Newly unlocked" after a session | Not shown | Reverse links: what this node opens |
| One concept per session | Lessons come in batches (`lessons.batch`) | `batch: 1` and an end screen |
| Fog of knowledge | Not shown | Count, but don't name, what lies two steps out |
| Personal knowledge map | The Week screen, stage groups | A local map (a node, its prerequisites and what it opens) and a region list ("Electricity: 12 of 40 explored") |
| Spaced repetition | The schedulers | Explored nodes reviewed lightly, so the map stays true |

**The format change Explore needs:** a reverse-link section (for each item,
the items whose links point at it), written by `mkcourse.py`. It is an
additive section, so it is a minor version (format 1.1): older readers skip a
tag they don't know (`COURSE_FORMAT.md` §3). With it, "what did this unlock"
and the frontier are a few reads, not a scan of the course.

## 5) The content pipeline (on a computer, never on the device)

```
graph source ──▶ node cards ──▶ review ──▶ course source ──▶ mkcourse.py ──▶ course.srs
(taxonomy, or    (LLM draft:    (a person   (items.jsonl,      (pictures
 an LLM-built     explanation,   accepts,    built_from =       rendered,
 graph)           question,      edits or    hard edges)        checked)
                  answer,        rejects)
                  diagram spec)
```

- **`tools/mkquest.py`** (new) turns a graph plus reviewed cards into a course
  source: hard edges become `built_from`, soft edges `unlock_after` (or are
  dropped), subjects and domains become levels or regions, and the licence and
  attribution go into `META` and the README.
- **Cards drafted by a model, accepted by a person.** The brief's "why now"
  is right that an LLM can draft an explanation, a question and an answer per
  node from the topic's name, description and evidence. It is also where
  errors creep in, so every card carries a review state and the build refuses
  unreviewed cards (the way `mkcourse.py` already refuses what the device
  would have to guess at). A consistency pass checks that a card doesn't lean
  on an idea that isn't among its prerequisites.
- **Diagrams are the hard part.** A 1-bit picture of at most 240 px wide reads
  well for a number line, a circuit, a fraction bar or a labelled shape. Draw
  them from small templates in code (a number line, a bar model, a node-edge
  sketch) where possible, by hand for the rest, and allow a card with no
  diagram. Generated raster art at this size and depth isn't worth trusting.
- **Cost and time** depend on the model and the card length: measure them on
  the pilot's 50–80 nodes before quoting anything for thousands.

## 6) What it costs on the device

- **Static DRAM:** a few flags (Study's session is already one heap block).
- **Heap:** the frontier and the "newly unlocked" list are bounded (say 32
  entries, 2 B each); the local map reads a node and its links (≤ 64 each
  way) one item at a time.
- **Card:** a 2,000-node course with a diagram on most cards is about 1–2 MB
  (`COURSE_CATALOG_PLAN.md` §3's arithmetic); progress 32 KB.
- **Scale beyond ~9,000 nodes** needs measuring before it is promised: open
  time, the due scan, the progress fold. The likely shape is one course per
  region (Mathematics, Physics, ...) with the frontier spanning the courses on
  the card; cross-course prerequisites would need their own design.

## 7) The screens (Explore, inside Study)

1. **Today:** what was explored today, and three to five **paths** — frontier
   nodes, each with its region — chosen to spread across regions (the brief's
   "Available paths"). A line counting what lies beyond ("9 more beyond these").
2. **A node:** the explanation (scrolls if it must), the diagram, "I've got it"
   → the question → reveal → right / not yet (self-graded, as Study's are).
3. **Unlocked:** "Mastered: Ratios. Newly open: Slope, Percentages, Scale
   drawings." Tap one to go on, or close the device — the brief's "the session
   is complete".
4. **The map:** a node in the middle, its prerequisites above, what it opens
   below, drawn on the shared I1 canvas; and a region list with how much of
   each is explored, the fog shown as "?" counts.
5. **Review:** explored nodes come back now and then as a single question,
   through the existing scheduler, so the map stays honest.

Speakers stay out of it, or one (Study's) stands in the strip for the end of a
session; the brief's "no engagement farming" rules out streak nagging, and
Study's Week screen already does the honest version.

## 8) Phases

| Phase | What | Firmware? | Where verified |
|---|---|---|---|
| **K0 — pilot as a plain course** | `mkquest.py`; 50–80 nodes from the taxonomy's Mathematics (number → fractions → ratios → proportional reasoning), cards drafted and reviewed, a few diagrams; built as an ordinary Study course with `by_links` | **None** | On the device in Study as it is: is one node a day pleasant at this size? Is the diagram legible? |
| K1 — Explore mode | the `concept` kind's card order, `batch: 1`, the Unlocked screen, the reverse-link section (format 1.1) | Study screens, `course.c`, `mkcourse.py` | `[s]` smoke; host gates for the new section |
| K2 — the map | the local map and the region list, the fog counts | Study screens | `[s]`; `[d]` legibility |
| K3 — scale | one course per region, a frontier across courses; measure open time and the fold at 10k nodes | `study.c`, `srs.c` | `[d]` timing |
| K4 — your map off the device | export what you know to the card (a small JSON or TSV the computer can draw); writing to GitHub only if the owner wants a token on the device | `study.c`; a write path is an owner decision | `[s]` file; security review |
| K5 — content at scale | the review tooling, more regions, an adult graph beyond the taxonomy's age 12 | tools only | review |

K0 is the decision point: it costs a converter and some writing, changes no
firmware, and answers the only question that matters before the rest — whether
exploring a graph one node at a time is good on this device in a hand.

## 9) Risks

- **Card quality** — an LLM-drafted explanation can be wrong in a way a
  learner can't detect. Mitigation: human review is a build gate; a card names
  its sources.
- **The adult graph doesn't exist yet.** The taxonomy ends at about age 12;
  an adult edition is new authoring (an LLM-built graph, reviewed), and the
  biggest single cost.
- **Share-alike** — a taxonomy-based course must be published ODbL / CC BY-SA
  with attribution; private edits to it can't be shipped closed.
- **Diagrams at 1 bit** may not carry the subjects that need them most
  (chemistry, biology). Pilot first in mathematics, where they do.
- **Two modes in one app** could confuse Study. If K1 shows that, Explore can
  take launcher position 6 (free since 2026-11-07) as its own tile, sharing
  `course.c` and `srs.c`. (Done for K0, at the owner's word: the Knowledge
  tile.)

## 10) Open questions for the owner

1. ~~Explore inside Study (recommended), or its own app on position 6?~~
   Answered 2026-11-07: its own tile, Knowledge, on position 6.
2. The pilot's audience: the taxonomy as it is (children, ages 4–12), or a
   small adult graph drafted for the pilot?
3. Is a model-drafted, human-reviewed card acceptable, and who reviews?
4. Should explored nodes come back for review (the SRS half), or should
   Explore stay pure discovery as the brief has it?
5. K4: an export file on the card, or a GitHub token on the device?
