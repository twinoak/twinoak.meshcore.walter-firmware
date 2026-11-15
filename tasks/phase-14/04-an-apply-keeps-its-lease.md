# 04 — An apply keeps its lease

**Depends on:** —
**Spec:** `components/agent/firmware_jobs.cpp` (the radio's apply, ~L745–800; `extendLease`, ~L285) and `include/walter_agent/firmware_jobs.h` (`kLeaseExtendSeconds`, `kExtendEveryMs`, `kApplyBoundMs`); `components/core/include/core/update/radio_update.h` (the windows, *to be set on the bench*) and `components/core/update/radio_update.cpp` (`step`, `stepFlashing`); `components/core/include/core/cycle/wake_guard.h` (`kRaisedBoundMax`); meshmaster.dk's task 227, which has the same contract block
**Size:** one session.
**Model:** claude-opus-5

## Goal

The unit's radio has been updated from meshmaster.dk twice. Its own journal says what that takes:

| | 13 November, 0.1.2 → 0.1.4 (job 28560) | 14 November, 0.1.4 → 0.1.5 (job 29475) |
|---|---|---|
| flashing the candidate | 14 min 44 s | 14 min 51 s |
| booting | 3 s | 3 s |
| proving | 3 min 03 s | 3 min 03 s |
| `check` to `done` | 17 min 51 s | 17 min 58 s |

Three things in this firmware were written before anybody knew a flash takes fifteen minutes.

**The lease.** The apply asks for 900 seconds at `check` and next when a step returns with five
minutes gone. The flash is one step. meshmaster.dk sets a lease to *now plus what was asked*,
even when it held more, so the thirty minutes the claim gave became fifteen, and the flash ended
15 seconds before the lease on the 13th and 8 seconds before it on the 14th. The image grew by
seven seconds of flashing between the two. One more like it and the server fails the job while
the unit is still flashing: the update goes through, the result arrives late, and a person has
been told it failed. meshmaster.dk stops shortening leases in its task 227. A unit must not
depend on that.

**The silence.** For eighteen minutes the job's page says *Nothing has come back yet*. Thomas
watched it on the 14th and asked for a longer timeout. What he was missing is a line that says
*flashing since 15:03*.

**The bound.** An apply raises the wake's bound to `kRaisedBoundMax`, thirty minutes. A rollback
is the flash and the proof twice: about thirty-seven minutes by the table. `wake_guard.h` says a
wake that overruns the raised bound is reset and the journal carries the work; nobody has looked
at what that means in the middle of the second flash.

## The contract

meshmaster.dk gets the same text in its task 227. Until its `docs/dev/unit-protocol.md` has it,
this is the page.

```
Progress  twinoak.fw.apply, POST /agent/v1/jobs/{id}/progress      FirmwareApplyProgress.json
{
  "data": { "journal": [
    { "at": "2026-11-14T14:03:57Z", "step": "check", "note": "the machine's preconditions hold" },
    { "at": "2026-11-14T14:03:58Z", "step": "flashing-candidate", "note": "the preconditions hold" }
  ] },
  "note": "flashing-candidate"
}

  journal   the entries of the result's journal written so far, oldest first, in the result's
            own shape and bounds.
  note      the newest entry's step.
  Posted when an entry is written, and replaced by the next one. It has no outcome: only the
  result has one.
```

## Deliverables

1. **The lease before every step that cannot stop.** Before the candidate's flash, and before
   the golden image's flash on a rollback, the unit extends its lease whatever the five-minute
   timer says, and asks for 1800 seconds: the server's ceiling (`MaxExtendSeconds`; more is
   refused as invalid). The extensions between the other steps stay. Read the Walter's own apply
   and the offer for the same fault — a step longer than what was last asked for — and say what
   was found.
2. **The journal as it is written.** At each journal line of a radio's apply, the progress report
   of the contract: one call a line, between two steps and never inside one. A call that fails is
   counted and changes nothing about the apply.
3. **A rollback inside one wake.** Read what the guard's reset at `kRaisedBoundMax` does to a
   rollback that is in its second flash at thirty minutes, and what the next boot's resume then
   does with the radio. Show both in a test, with the scripted radio and a flash of fifteen
   minutes on the test's clock. Then either make the bound an apply raises what a whole rollback
   takes — from the windows' own constants and a named, measured flash time — or say in the
   report why thirty minutes has to stay. A FOLLOWUPS line for Thomas either way. Nothing about
   what the machine does at any step changes.
4. **The numbers that were guesses.** `radio_update.h` reasons the battery floor from *ten
   minutes of flashing and proving*, and `wake_guard.h` the raised bound from *a flash of a
   megabyte over a 100 kHz debug port*. Put the two measured runs where a reader of those lines
   finds them. Whether 3.7 V is still the right floor for a unit awake for thirty-seven minutes
   is Thomas's: a FOLLOWUPS line tagged `[Thomas to decide]`, and no change.
5. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The fixture**, `FirmwareApplyProgress.json`, where `twinoak.report`'s two are and marked as
   they are: *not yet in meshmaster.dk* (its task 227).
7. **The words.** Wherever the apply's steps are described for a reader, and a line in
   `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] A scripted apply whose flash takes 15 min 30 s, against a scripted server that holds a lease at *now plus what was asked*, as meshmaster.dk does today: the lease never lapses before the result is posted (test, by the order and the times of the calls).
- [ ] A scripted rollback: an extension of 1800 seconds immediately before the second flash (test).
- [ ] One progress report for each journal line, in the contract's shape; the last one's journal is the result's (test, against the fixture).
- [ ] Every progress call refused or unanswered: the apply's journal and result are what they are without them (test).
- [ ] The rollback that passes thirty minutes: a test shows what a unit does today, and what it does after this task.
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

A faster flash. meshmaster.dk's side: its task 227. A second attempt at an apply. The board: no
session opens a port. A release: Thomas presses the button.
