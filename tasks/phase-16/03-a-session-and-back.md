# 03 — A session and back

**Depends on:** 01
**Spec:** `spec/SYSTEM.md` S-5, S-7 and S-12, and §6; `spec/LINK-PROTOCOL.md` §9; `components/core/include/core/update/radio_update.h` (`kScannerWindowMs` ~L92, the battery's two figures ~L106–111, `kFlashWindowMs` ~L146, `kOnePassMs` ~L150, `State` ~L160, `Kind` ~L185: `kRoleScanner` and `kRoleRepeater`, `Reason` ~L195, `Request` ~L254, `start` and `resume` ~L345–352), `components/core/update/radio_update.cpp` (what a switch back checks and proves, ~L434–751); `components/core/include/core/update/journal.h` (the entry ~L14–24, the kinds ~L72–105, `protect()` ~L37); `components/core/cycle/wake_cycle.cpp` (`stepResume` ~L620; the state step's *the scanner is running* ~L674; the steps' table ~L138–166), `components/core/include/core/cycle/wake_guard.h` (the bound on a wake, and what may raise it); `components/core/include/core/supervisor/supervisor.h` (`Health::kScanner`) and `docs/supervisor.md` (~L32–54); `components/core/include/core/console/heap_report.h` (the internal heap's figures, and why the largest block is the one that decides) and `components/platform/heap.cpp`; `components/core/console/console.cpp` (`rakUpdate` and `rak role`, ~L588, ~L2376, ~L2987), `parser.cpp` (~L318); `docs/update.md` (~L60–72, ~L129), `docs/cycle.md` (*The budget, and the things that may hold a wake open*, ~L399; *Nothing runs twice*, ~L431); task 01's report. In the repeater firmware's repository, read only: `docs/noisescope.md` §4 and §5.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

`rak role scanner` makes the radio a scanner. It stays one until a person types
`rak role repeater`. On the bench that is a person with a cable. On a mast it is nobody: a
board that restarts between the two, or a server that stops answering, leaves a site with no
repeater.

A session is the two switches as one thing with a program between them, and rails that end it
whatever happens in the middle. The repeater coming back must depend on nothing but this board,
its store and its journal.

## Deliverables

1. **The session**, as logic under `components/core/`, with no ESP-IDF include: the check,
   the scanner image flashed and a scanner found (`kRoleScanner`, as it is), a greeting, the
   steps, the golden image flashed back and proved (`kRoleRepeater`, as it is), the end.
   Neither kind of the radio's update machine is changed.
2. **The greeting**, said once a scanner is found: `time <epoch>`, `auto 0`, `info`. What the
   scanner answers is kept and handed over with the first step's lines, and counts as that
   step's: the session's lines begin with its `ack`s and its `info` line.
3. **A step** is a command and its seconds. The rules, held here whoever gives the program:
   - 1 to 6 steps;
   - a command is 1 to 40 characters of `a`–`z`, `0`–`9`, space and the full stop, and its
     first word is one of `stat`, `sweep`, `scope`, `hist`, `dwell`, `cad`;
   - seconds are 5 to 300, and come to 600 at most over the program.

   Run so: what the scanner said between steps is thrown away; the command is said (task 01);
   lines are kept whole in the session's room until one of these, which is how the step
   *ended*:
   - `done`: a `done` line for the command's first word;
   - `clock`: its seconds are up, and `stop` is said;
   - `full`: the room is used up, and `stop` is said;
   - `refused`: the `ack` said not ok;
   - `silent`: no `ack` came.

   Then the step's lines are handed to whoever runs the session, and the next step begins
   when that returns.
4. **Its room**: one buffer of 8 192 bytes for a step's lines (`kStepBytes`), taken from the
   heap when the session begins and given back when it ends. No static storage of that size.
   A session that cannot have it is refused before anything is touched. The room is asked of
   one seam, so that task 05 can answer it from somewhere else.

   Why 8 192 and no more: the unit in service has said what it has. Its report's `restarts`
   section, in November: *heap: 27472 bytes free, largest free block 12288, least free since
   this boot 25300 (internal, 8-bit)*. A room of 16 kB is not there to be had, and one of
   12 kB would leave nothing whole for anybody else.

   **And measured, at every session**, so that the figure can be raised later from what units
   say. Through the seam the console's `heap` already has (`core/console/heap_report.h`): the
   free bytes and the largest free block just before the room is taken; the largest free
   block just after; and the least free since boot, read when the session ends. One line of
   the session's journal, `room`, written before the switch back, says them inside the note's
   72 bytes, as `8192 of 27472, block 12288; left block 4096; least free 17108`. `rak session`
   prints the same line. A build that was handed no such seam says so in the line. The
   figures decide nothing here.
5. **The rails.**
   1. **Open before the first flash.** The journal says a session is open, under a kind of its
      own, before the scanner image is flashed. It says closed when the golden image is back
      and proved, and at no other moment.
   2. **An open session with nobody running it is ended by the next wake.** After a restart,
      a crash, the bound on a wake or a power cut, step 3 of the next wake takes it up, after
      any radio update the journal says is in progress: the golden image is flashed back and
      proved, and the journal says closed. It needs no server, no uplink and no console.
   3. **Twenty minutes as a scanner at most** (`kSessionCeilingMs`), counted from the moment
      the scanner is found. Past it the session goes to the switch back, whatever step it is
      in and whatever its lines are waiting for.
   4. **Not begun unless it can be ended.** The check holds everything the switch to the
      scanner checks and everything the switch back will: a scanner image that verifies and
      is the golden image's board and band, a golden image that verifies, the battery's two
      figures, the link up, this board's clock set, no radio update in progress, this
      firmware confirmed and not on trial. Each refusal has its word (deliverable 7).
   5. **A switch back that is refused or fails leaves the session open**, and every wake
      tries again. One journal line a try.
   6. **Begun by a person or a job, never by this firmware.** The console's verb below and
      task 04's job are the only two callers.
   7. **One a wake, and it is that wake's update** (S-7): no other update step runs in a wake
      that ran a session, or the other way round.
6. **A scanner nobody opened a session for is left alone**, as today: a person typed
   `rak role scanner` and means it. The supervisor says which of the two it sees: *in a
   session, since …* or *running the scanner, left alone*.
7. **How it ends**, for the caller: *restored*, with each step's ending, lines and bytes;
   *refused*, with one of `battery`, `link`, `clock`, `inProgress`, `noGolden`, `noScanner`,
   `program`, `noMemory`; *stuck*, when the golden image did not come back. And its own
   journal lines, in words a person reads: `check`, `flashing-scanner`, `scanner`, `step-1` …
   `step-6` with the command, `room`, `flashing-golden`, `booting`, `proving`, `restored`, `stuck`.
8. **The wake is held open** for a session as it is for an update: two passes of the update
   machine and the ceiling, and not a second more. `docs/cycle.md`'s table gains the row.
9. **`rak session <seconds> <command>`**: a session of one step. Each line is printed whole
   as the step hands it over, and not logged (S-12). `rak session` alone says whether one is
   open, since when, and what the next wake will do about it.
10. **Said where a unit is read**: the wake's record, `rak watch`, and the report's `radio`
    and `journal` sections.
11. **The words.** A section of `docs/update.md` (or a page of its own beside it) with the
    rails as a numbered list; `docs/console.md`, `docs/supervisor.md`, `docs/cycle.md`.
    `docs/bench.md` §9, three items: a session of one `sweep` on a board with a radio, with
    the times of both flashes; the cable pulled while the radio is a scanner, and what the
    next boot does; `rak role scanner` by hand, three wakes with `stay` off, and the scanner
    still there. One line in `release-notes/unreleased/`.

## Acceptance criteria

Against the scripted flasher, the scripted scanner of task 01 and the simulated flash.

- [ ] A session of two steps: the journal reads open, the scanner's flash, the two steps, the golden image's flash, proving, closed; the radio answers `HELLO` as the golden image; the steps ended as scripted, one `done` and one `clock` with a `stop` said (test).
- [ ] `time`, `auto 0` and `info` are said before the first step's command, and the `info` line is among the lines the first step hands over (test).
- [ ] The power cut at every write the session's journal makes, and at every write of both flashes: in every case the next boot's wake ends with the golden image proved and the session closed (test, over the whole list of writes).
- [ ] A restart in the middle of a step, and the bound on the wake ending it there: the same (tests).
- [ ] The scanner image flashed and no scanner found inside `kScannerWindowMs`: no step runs, the golden image is back, the session closed (test).
- [ ] A caller that sits on a step's lines past the ceiling: no further step is begun and the switch back starts (test, on the scripted clock).
- [ ] A step of `hist` scripted to send 20 kB: it ends `full`, `stop` is said, the lines handed over are whole and under 8 192 bytes (test).
- [ ] A session against a scripted heap: its journal has one `room` line with the five figures as scripted, in 72 bytes or less; a session with no heap seam has the line saying so (tests).
- [ ] An `ack` that says not ok ends the step `refused` and the next step runs; no `ack` ends it `silent` (tests).
- [ ] Each refusal with its word, and the radio untouched: no scanner image, a scanner image of another band, no golden image, the battery below its figure, the clock not set, a radio update in progress, this firmware on trial, seven steps, a command of 41 characters, a command beginning `reboot`, a command with a `;`, 4 seconds, 301 seconds, 601 seconds in all, no room (tests).
- [ ] A switch back refused for the battery: the session stays open; the next wake tries again; with the battery back it is restored and closed (test).
- [ ] A scanner found at a wake with no session open is not flashed over, at that wake or the next three (test).
- [ ] A wake that ran a session runs no update in step 9, and a wake with an update in progress begins no session (tests).
- [ ] Nothing but the console's verb and the seam task 04 will use can begin a session (a rail in `tools/check_rails.py`, or a test that says how it holds).
- [ ] No text of a scanner's line is in the log, the journal or a report (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The job, the payload and the posts: task 04. A session longer than its ceiling, or one that
spans a sleep. A faster flash. The update machine's two kinds. The scanner's firmware. The
board: no session opens a port. A release: Thomas presses the button.
