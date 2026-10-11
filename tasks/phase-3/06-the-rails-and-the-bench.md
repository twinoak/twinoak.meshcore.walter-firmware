# 06 — The rails and the bench

**Depends on:** 05.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5 … S-8, S-12, §7, §10; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) (all of it — this task holds the code against it); [`CLAUDE.md`](../../CLAUDE.md) (its last paragraph is the list of rails); every report under `tasks/phase-*/reports/`; the hardware repository's `hardware/pcb/BENCH-BREADBOARD.md` §7 at `b798aab043281fa4c9b17802a164fa94be12e206` (the order the rig is brought up in).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Four piles were written by many sessions, none of which saw a board. Before a
person sits down with the first one, this task does three things: it turns
every promise this firmware makes — never chip-erase, never drive reset high,
never trust an unsigned image, never skip TLS verification, never print a
secret — into **a test or a grep that fails the build**, in one place; it
turns the bench steps that fifteen tasks each appended into **one ordered
walk** a person can follow from an empty desk; and it makes the README say
what this firmware is and is not.

It writes little new code. If it finds a rail that does not hold, **that is a
finding to report and fix**, not to paper over.

## Deliverables

- **`host_test/rails/`** and **`tools/check_rails.py`**, run by
  `tools/verify.ps1`; one entry per rail, each naming the decision it guards:

  | Rail | Held by |
  |---|---|
  | nRESET is never output-high | the fake pins, across every test in the suite (a global assertion, not one test) — and a grep of `platform/` for any push-pull configuration of that pin |
  | everything radio-facing is an input before KILL goes high; two resets ≥ 1 s apart | the fake pins, globally |
  | no `ERASEALL`, no `ERASEUICR`, no write to UICR | the simulated chip fails any test that touches them; a grep that the constants are referenced nowhere but their definition |
  | the flasher writes only inside the board's application region and the settings page | the simulated chip's write log, asserted at the end of **every** test that flashes |
  | nothing reaches the store or a radio without the six checks | the token type; a test that the only constructor of the token is the verdict |
  | a normal build trusts no test key and contains no signing code | a search of the built image for the test key's bytes; the release tooling's refusal of a `+TESTKEY` build |
  | TLS is always verified; no `http://` | task 02's checks, moved here |
  | no secret, token, PIN, IMEI or ICCID in the log, the journal, `dump`, `status`, or the wake record | one test that provisions secrets, runs a simulated day and every console command, and searches everything captured |
  | nothing the mesh or a scanner said is logged as text | the same capture, searched for the scripted radio's payloads |
  | never both updates in one wake; never an update on an unconfirmed Walter image | the simulated month of task 05, asserted on its journal |
  | the supervisor's daily caps | the property test of phase 2, task 05, run here with a longer horizon |
  | `core/` includes nothing of ESP-IDF or the modem library | a grep |
  | `spec/` is the copy it claims to be | `specsync.py verify` |
  | no mesh request is sent by this firmware | task 05's assertion |

  For each rail the report says: holds; or held only after a fix made in this
  task (which fix); or **cannot be held by a test** and why.
- **A failure-injection sweep**: the suites' power-loss loops (image store,
  journal, flasher, radio update) run together end to end — a Walter that
  loses power at every flash operation **of a whole update, download to
  commit** — and the outcome is always one of: the old image running, the new
  image running, or a journalled state the next wake resumes. Report the
  number of injection points.
- **`docs/bench.md`, rewritten as one walk.** Collect every *Not verified
  here* from every task and report; order it as the hardware's
  `BENCH-BREADBOARD.md` §7 brings the rig up — power and console, UART,
  reset, SWD identification, the first flash, the kill line, then images,
  updates, the supervisor, the modem, HTTPS, provisioning, sleep, a day. For
  each step: what to connect, what to type, what should be seen, **what it
  means if it is not**, and a box to tick. Mark the steps that can damage
  something or lose the radio's identity, and the three that matter most
  (the first flash; the deliberate bad image; the scope on nRESET across
  sleep). Nothing may be dropped: the report lists each source item and the
  step it became.
- **`docs/bench.md` ends with "What to bring back"**: the measurements that
  turn placeholders into numbers — every named constant that any report
  listed as *to be set on the bench*, gathered into one table with its
  current value, the file it is in, and the bench step that measures it.
- **`docs/field.md`**: what a unit on a mast does that a person should know —
  what each wake does, what it does when the radio is silent, when the modem
  has no coverage, when the battery is low; how to read `journal` and `status`
  from a cable; what "the supervisor has stopped" means and what to do; how a
  unit is provisioned and how its radio gets its first golden image; what
  **cannot** be recovered remotely (a protected chip, a dead bootloader, a
  locked SIM).
- **`README.md`**, as it should read to someone arriving cold: what the unit
  is (one paragraph, a link to `spec/SYSTEM.md`), what this firmware does and
  what it deliberately does not (the agent), how to build and test
  (`tools/verify.ps1`, Docker), how to flash a Walter, where the bench walk
  is, how a release is made and signed, the state of things — *no board has
  run this yet* until one has.
- **`docs/ARCHITECTURE.md` held against the code**: every *(verify)* mark
  either removed with the citation that settled it or left with a sentence
  saying what is still unknown; every place the code ended up different from
  the document, corrected in the document. The report lists the differences.
- **`tasks/phase-3/FOLLOWUPS.md`** gains, for the piles that come next: what
  a bench pile should automate once the rig is on USB (which console
  sessions, which expected outputs); what the agent work will need from this
  firmware that `Backend` does not yet offer; every specification gap any
  report raised, with the task that raised it.

## Acceptance criteria

- [ ] `tools/verify.ps1` green with `check_rails.py` and the rails tests in it.
- [ ] Every rail in the table has a line in the report: holds / fixed here / cannot be tested, with the reason.
- [ ] `docs/bench.md` is one ordered walk; the report maps every earlier bench item to a step, and no task's *Not verified here* is missing.
- [ ] The table of bench-set constants is complete: a grep for the phrase the tasks used finds nothing that is not in it.
- [ ] `README.md`, `docs/field.md` exist and say plainly that no hardware has run this firmware.
- [ ] The report ends with a short honest list: what in this firmware is most likely to be wrong when the first board arrives, in order, and why.

## Not verified here

Everything in `docs/bench.md`. That is the point of the file.

## Out of scope

New features. Fixing a design decision (raise it; `spec/` is changed where it
lives). The bench pile itself.
