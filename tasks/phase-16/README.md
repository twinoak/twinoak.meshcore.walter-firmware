# Phase 16 — the radio as a scanner, for a while

Written on 18 November.

**What there is.** The store keeps a scanner image for each radio. `rak role scanner` flashes
it and proves a scanner is there; `rak role repeater` flashes the golden image back and proves
it as an update is proved. The link's client tells a scanner from a repeater, keeps its last
three lines cut to 320 bytes, and says one word to it, `info`. A scanner that is running is
left alone.

**What there is not.** Nothing but a person at the console starts any of it. Nothing tells the
scanner what to measure. Nothing of what it says leaves the board. And a radio turned into a
scanner stays one until a person turns it back, which on a mast is nobody.

**What this pile builds.** The scanner spoken to, and its lines kept whole (01). An offer that
fills the scanner slot (02). A session: the scanner image, a program of steps, and the golden
image back, with rails that bring the repeater back whatever happens between (03). The session
as meshmaster.dk's job `twinoak.noisescope.session`, its lines posted as they come (04). And
room for what a step says: the internal heap has 8 192 bytes in one piece to give, so the
module's PSRAM, off since the first task, is switched on for that one job (05).

**And one that is not about the scanner** (Thomas, 18 November). In the night of 17 November
two wakes took a `status`, ran into the five-minute bound and were reset, with thirty minutes
of sleep after each: over two hours with nothing measured. What they were in is not known. What
is known is that an exchange is promised thirty-nine minutes and the bound on its wake is five. 06 makes the bound cover what
a wake took on, and a job that is not answered end as a `timeout` the server hears of. It
depends on nothing here.

**One launch for all six.** A run stops at a task that does not land, and 06 is after 05 in
the table. So if the PSRAM costs memory the start does not have, task 05 leaves the firmware
as it was and merges its report alone, and the run goes on to 06.

**What a session costs.** Two flashes of the radio, about fifteen minutes each, with the
program and three minutes of proving between them: about forty minutes off the mesh, with this
board awake for all of it.

**The server's half is meshmaster.dk's pile 4.42.** Both piles are written against the same
contract and the same fixture lines, given in tasks 02 and 04 here and in its tasks 235 and
236. They can run at the same time. Pile 15's task 04 still waits for the library and has
nothing to do with this one.

```powershell
.\tasks\run-all.ps1 -Phase '16'
```

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-scanner-is-spoken-to](01-the-scanner-is-spoken-to.md) | — | a line said to a scanner and its `ack` read; a scanner's lines kept whole in a buffer the caller lends; `rak say` |
| 02 | [an-offer-into-the-scanner-slot](02-an-offer-into-the-scanner-slot.md) | — | `twinoak.fw.offer` with `into: scanner` stores the release's scanner image in the scanner slot, taken up again across wakes as a candidate is |
| 03 | [a-session-and-back](03-a-session-and-back.md) | 01 | scanner image, steps, golden image back, as one machine; an open session is ended by the next wake whatever ended the last; `rak session` |
| 04 | [a-session-is-a-job](04-a-session-is-a-job.md) | 02, 03 | the sixth job type: the program from the payload, the lines posted step by step, the journal as progress, a result owed across a restart |
| 05 | [the-room-is-in-the-psram](05-the-room-is-in-the-psram.md) | 04 | a step's room is 64 kB of PSRAM where there is PSRAM and 8 192 bytes of the heap where there is not; an ordinary wake never touches it; `heap` and the session's `room` line say which |
| 06 | [a-wake-outlasts-what-it-took-on](06-a-wake-outlasts-what-it-took-on.md) | — | the bound on a wake covers the leases of the jobs it claimed; a job the radio never answers is reported `timeout` and the wake sleeps; a wake that hangs is still reset |

## Models

- **Opus**: 01, 02, 03, 04, 05, 06.

## Afterwards (Thomas)

- **The bench, with a board that has a radio on its link.** `docs/bench.md`, the items these
  tasks add:
  - `fetch scanner`, `rak role scanner`, `rak say sweep`: the `ack`, and a sweep line whole;
  - `rak role repeater`, then `rak session 30 sweep`: scanner, one step, the repeater back and
    proved, about forty minutes;
  - the same with the USB cable pulled while the radio is a scanner: the next boot flashes the
    golden image back by itself;
  - **the PSRAM, before any release** (task 05's ⚠ items; no test can bring a PSRAM up, so a
    board is the first proof). On a Walter alone: `heap room` says `psram of 2097152`, the
    boot's `heap:` line has lost no kilobyte against the release before, and a wake with no
    session is as long as it was. On a board with a radio:
    `rak session 60 scope 863 870 250 0 1000` gives sixty lines and a `room` line saying
    `psram`.
  A Walter alone has no radio to turn into a scanner. The scanner by itself needs no Walter:
  the release's `twinoak-scanner-rak4630-single-868-*.uf2` on a RAK over USB, and the repeater
  firmware's `tools/noisescope/waterfall_plot.py`.
- **Release**, merge its notes pull request, import it on meshmaster.dk, mark it canary, offer
  and apply. Then, with meshmaster.dk's pile 4.42 deployed: *Fetch a scanner image* on the
  exit node's Firmware tab, and *Start a session* on its NoiseScope tab.
- **After the first sessions: read the `room` line** in each session's journal on
  meshmaster.dk. `psram` in it means the step had 64 kB. A line without it means the unit
  fell back to 8 192 bytes of the heap, and the same line says what the heap had.
- **`spec/SYSTEM.md` §9** still lists *NoiseScope as a job* under *later*, and no decision of
  its §3 says what task 03's rails say. Both are the hardware repository's to change.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
