# 06 — A wake a person asked for

**Depends on:** 05
**Spec:** `spec/LINK-PROTOCOL.md` §8 (`EV_ATTENTION`, reason 4: *a person asked for the Walter*; one new attention per reason per hour; `SLEEP_NOTICE` is the last request before a sleep), §5.1 (`HELLO`: `boot_id`), §5.2 and §5.3 (`PING` and `STATUS`: `uptime_s`), `components/core/include/core/cycle/wake_reason.h` (`timeAwayMs`: 0 for every wake but the timer's; the retained record; `WakeEntry`), `components/agent/agent_backend.cpp` (`restoreState` ~L190–230, `saveState` ~L175, the one call of it ~L334, `agent state` ~L640–670), `components/core/cycle/wake_cycle.cpp` (`stepState` reads `STATUS` and a pending attention, `stepBackend`, `stepSleep`, `journalWake`), task 05 of this pile (where a trial boot's wait is set aside), `tasks/phase-13/FOLLOWUPS.md` (its first item), `host_test/core/cycle/test_time_away.cpp`, `host_test/agent/test_agent_backend.cpp`
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 13 November Thomas pressed *Wake it up* on meshmaster.dk (job 28399). The unit's radio
answered `ok` at 18:09:02 UTC. Nothing came from the unit until its timer woke it.

What the code says, which nobody has confirmed at the unit's console:

- The agent's wake sends nothing while a wait is owed. A wait is what one failed call leaves:
  30 seconds, then more, or what the server's `Retry-After` named.
- The wait is saved and put back at the next boot less the time the board was away, and the
  time away is 0 for every boot that is not a timer wake. A wake on the radio's line is not a
  timer wake, so the wait is owed whole.
- This unit has had a failed call at nearly every wake. So nearly every wake a person asks for
  finds a wait, and sends nothing.

Task 05 took the wait away from a boot on trial. This task takes the root: the board learns how
long it was away whatever woke it, and a wake a person asked for does not wait at all.

The radio never sleeps and it counts its own seconds. Its `uptime_s` before the sleep and after
it, with the same `boot_id`, is the time the board was away, for every kind of wake.

## Deliverables

1. **A reading before the sleep.** After the agent's state has been saved and before
   `SLEEP_NOTICE`, the board reads the radio's `uptime_s` and writes it down with the radio's
   `boot_id` in the duty cycle's retained record. The reading belongs to the state it was taken
   after: saving the state again makes the reading void until a new one is written. A wake that
   saved its state and never reached the reading (a crash, a watchdog, a restart for an update,
   a radio that did not answer) leaves none.
2. **The time away, for every wake.** At a boot whose `HELLO` names the same `boot_id` and
   that has a reading, the `uptime_s` step 4 reads less the one written down is the time away:
   less one second for the whole seconds at either end, less the radio clock's own error (one
   constant, said once, with where its figure comes from), and never below 0. `timeAwayMs()`
   answers that for a wake that is not the timer's, and it is known before `restoreState` asks.
   A timer wake keeps the figure it has today. Another `boot_id`, no reading, a radio that
   answered nothing, an `uptime_s` below the one written down: 0, as today. The rule the
   library is handed the figure under stands: never more than really passed.
3. **A wake a person asked for owes no wait.** A wake whose attention is reason 4, whether the
   event woke the board or step 4 found it pending in `STATUS`, does not put the saved wait
   back: the line task 05 drew for a trial boot, drawn here too. Everything else of the state
   is restored as it is. The bound on how often is the radio's (§8: one attention per reason an
   hour); this board adds none, and the report says so.
4. **What a person reads.** `agent state` says where this boot's time away came from (the
   timer, the radio, nothing) and that a wait was set aside when it was, and with it the
   report's `agent` section. A wake in the list of ten that sent nothing for a wait says which
   of the three it had, if the entry has room; if it has not, the journal's line for that wake
   carries it and the report says where it went. The comment in `wake_cycle.cpp` that calls a
   line wake inside a wait expected, the block over `timeAwayMs` in `wake_reason.h` and the
   `else` branch of `agent state` say what is true now.
5. **Room.** `tools/heap_at_start.py` over this task's build: the margin is not below main's.
   The retained record stays inside `kRetainedRecordMax`.
6. **The words.** `docs/` where waits and wakes are described. `docs/bench.md` §3, under *An
   attention wakes the Walter*: the lines to run on a board, a wake asked for inside a wait and
   what `agent state` says after it. One line in `release-notes/unreleased/`. One line under
   the first item of `tasks/phase-13/FOLLOWUPS.md` saying what this task did; the box stays
   unticked.

## Acceptance criteria

- [ ] A saved wait of 300 seconds; the board sleeps; the radio's line wakes it for reason 2 with the radio's `uptime_s` 400 seconds on and the same `boot_id`: the wake posts its heartbeat (test).
- [ ] The same, 100 seconds on: nothing is sent, and the wait still owed is no less than the 200 seconds really left and no more than 300 (test).
- [ ] The same, 10 seconds on, for reason 4: the heartbeat is posted (test).
- [ ] A timer wake inside a wait that finds reason 4 pending in `STATUS`: the heartbeat is posted (test).
- [ ] Another `boot_id` at the next boot, for reason 2: the wait is owed whole (test).
- [ ] A state saved with no reading written after it: the wait is owed whole at the next boot that is not a timer wake (test).
- [ ] An `uptime_s` below the one written down, same `boot_id`: 0 (test).
- [ ] A timer wake with no attention: the figure and the tests it has today, unchanged.
- [ ] `agent state` names the source of the time away in each of the three cases, and a wait set aside (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Whether an attention wakes this board out of deep sleep at all: `docs/bench.md` §3's box is
unticked and only a board can tick it. A line disarmed by spurious wakes: the board does not wake
then, and the pending attention is seen at the next timer wake, which deliverable 3 covers. The
radio answering `ok` to `walter.wake 1` whether or not it raised an attention: the repeater
firmware's. A power cut: the radio restarted with the board, nothing was measured, 0 as today.
The chip's own clock as a second measure. The library: copied byte for byte, not changed here.
Which call fails at every wake of the unit. The board: no session opens a port. A release:
Thomas presses the button.
