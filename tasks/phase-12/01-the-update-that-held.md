# 01 — The update that held

**Depends on:** nothing.
**Spec:** [evidence/README.md](evidence/README.md) (what meshmaster.dk says of the update; the unit's page is not yours to read, and that file is all of it); `README.md` (*The state of things* ~L40–66, *Still unverified on a board*, the phase table ~L84–90), `docs/flashing.md` (*From 0.1.2 to 0.1.4, over LTE* ~L122–245), `docs/bench.md` §18 (*The Walter's own release over LTE, 0.1.2 to 0.1.3* ~L1896, *The budget's message* and *The download says who it is* after it), `docs/console.md` (the first lines a port prints; `self status`), `docs/release.md`; `components/agent/firmware_jobs.cpp` (the journal lines of an apply's result ~L618–640, `at_epoch`), the fetch behind an offer (the range loop, and what it does with a range that fails or comes back short); `tasks/phase-11/FOLLOWUPS.md` (*The review of 04*).
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

On 10 November a unit took v0.1.4 over LTE from v0.1.2 and kept it: stored at 14:52 UTC, committed
at 15:20, with nobody at it. The day before, the same unit rolled v0.1.3 back by itself. Those two
are the first update over LTE that held and the first rollback, both in the field.

The repository still says it is *not known* whether a unit has taken v0.1.4, and the bench's item
for this walk is unticked and written for v0.1.3.

## Deliverables

1. **README** says what happened: where it says *not known when this was written*, and wherever
   *Everything else is still simulators* and *Still unverified on a board* now claim more than is
   true or less. Say exactly what was seen and from where. The server's pages were read; no console
   was. A thing nobody watched is not ticked as watched.
2. **`docs/flashing.md`**, *From 0.1.2 to 0.1.4, over LTE*: the walk as it went, with its times,
   and the three offers before it that expired because the unit did not check in within the
   server's 30 minutes. A person who offers a release to a sleeping unit should read here that the
   offer has to be made again if the unit was away.
3. **`docs/bench.md`** §18: the item is for 0.1.2 to 0.1.4 and says which of its readings the field
   gave (the ranges, the result, the commit, the version on the heartbeat) and which only a cable
   gives (the console's lines, `self status`, the seconds of the download). Tick nothing the
   evidence does not carry. The download's length goes in as the evidence has it: at most
   10 min 15 s.
4. **Two things to read from the code and say**, in the docs where they belong and in the report:
   - **14 requests for 12 ranges.** When does one offer ask for a range twice, and does anything
     say so? The result says `resumed: false` and the agent's log has no line for it.
   - **Three journal lines with one second.** *switching*, *on-trial* and *confirmed* are all
     stamped 15:20:13Z, and a trial is not over in a second. Say what `at` is.

   If either turns out to be a defect, do not fix it here: a line under this pile's FOLLOWUPS with
   what you read and where.
5. **`docs/console.md`:** one sentence where the banner is described: what is printed before a USB
   host is attached is not seen by a person who plugs the cable in afterwards
   (`tasks/phase-11/FOLLOWUPS.md`).
6. **`tasks/phase-11/FOLLOWUPS.md`:** under *`v0.1.4` is on a unit* and under the `printf` line, add
   what was done. The reviewer ticks the boxes.

No code changes, no release, no tag. Nothing opens a port.

## Acceptance criteria

- [ ] `grep -rn "not known" README.md docs/` finds nothing about v0.1.4.
- [ ] Every time and figure in the three documents is in `evidence/README.md`, and nothing is said to have been seen on a console.
- [ ] The report answers both questions of deliverable 4 with the source lines it rests on.
- [ ] `tools\verify.ps1` green (the docs rails). CI green.
