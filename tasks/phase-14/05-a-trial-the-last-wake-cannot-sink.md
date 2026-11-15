# 05 — A trial the last wake cannot sink

**Depends on:** —
**Spec:** `components/agent/include/walter_agent/agent_uplink_check.h` (the trial's third condition: one heartbeat answered `ok`, *the wake's, once*), `components/agent/agent_backend.cpp` (`restoreState`, ~L205–227: the hold put back with `timeAwayMs()`; the wake and the trial's steps after it, ~L300–333), `components/core/cycle/wake_cycle.cpp` (`stepResume` ~L560, `stepUpdate` ~L900, `runTrial` ~L1010), `components/core/include/core/update/self_update.h` (`kTrialWindowMs`, `kNoUplink`), `components/core/include/core/cycle/wake_reason.h` (`timeAwayMs`: 0 for every wake but the timer's), `components/agent-core/src/AgentLoop.cpp` (`restore`, ~L864: `holdForMs` less the time away)
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 15 November Thomas applied v0.1.6 to the unit (meshmaster.dk job 30474). It booted, went on
trial, and five minutes later rolled itself back: *the backend was not reached inside the trial
window*. The unit is on v0.1.4 again and v0.1.6 is on its bad list, never to be tried again. The
diagnostics v0.1.6 carries, which were built to find out why the unit misbehaves, cannot be
installed on the unit that misbehaves.

What the code and the server's record say, which nobody has confirmed at the unit's console:

- The trial's third condition is one heartbeat answered `ok` in this boot, and the wake posts it
  **once**.
- The agent's wake sends nothing while a hold is owed. A hold is what one failed call leaves:
  30 seconds, then more.
- The hold is saved and put back at the next boot less the time the board was away, and the time
  away is 0 for every boot that is not a timer wake. A restart into a trial is not a timer wake.
- This unit has had a failed call at nearly every wake: `httpFailures` rose by one about every
  five minutes on the morning of the 15th, with a heartbeat answered each time. Which call it is,
  v0.1.4 cannot say.
- So the wake that took the apply at 08:41:07 UTC left a hold; v0.1.6 booted owing all of it,
  sent nothing, and its trial ran out. The rolled-back v0.1.4 owed it too and sent nothing; the
  timer wake after that posted the heartbeat and the apply's result, at 08:52:00.
- The update to v0.1.4 on 10 November confirmed. The hold was already in that image; the wake
  before it had, it seems, no failed call.

A trial exists to answer one question: can this image reach the server. A wait that an older
image's failed call left behind must not answer it *no* before the question has been asked.

## Deliverables

1. **A trial boot owes no hold.** In a boot that is on trial, the wait restored from the saved
   state is not put back: the heartbeat goes out in the trial's first wake, whatever the wake
   before it ended with. Everything else of the state is restored as it is. Say in the report
   where the line is drawn, and that the library is told nothing untrue about the time away.
2. **A heartbeat that fails is tried again while the window lasts.** Today it is posted once,
   and one slow attach is a rollback and a bad-listed image. While on trial, the agent's wake
   runs again each time the library's own wait has run out, until a heartbeat is answered or the
   window ends. Nothing else decides when the modem speaks, and nothing but the heartbeat is
   retried for the trial's sake.
3. **The owed result of a rollback is not held up either.** After a trial is rolled back, the
   boot of the old image owes the server the apply's result. Show what holds that back today —
   on the 15th it took the timer wake after — and send it in the first wake that can.
4. **What a person reads.** `self status` and the journal's line for a rollback by `kNoUplink`
   say how many heartbeats were tried in the window and what the last one came to, so that *not
   reached* is told apart from *not asked*.
5. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The words.** Where the trial is described for a reader (`docs/`), and the comment in
   `agent_uplink_check.h` that says *once*. A line in `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] A scripted unit whose saved state holds a wait of 300 seconds, switched to a new image: the trial's first wake posts a heartbeat, and the image confirms (test). The same without this task's change rolls back by `kNoUplink` (the test shows it, by a switch or by its history).
- [ ] The same state in a boot that is **not** on trial and is not a timer wake: the wait is owed whole, as today (test).
- [ ] A trial whose first heartbeat fails and whose second, after the library's own wait, is answered: confirmed, with two heartbeats on the wire and no other call repeated (test).
- [ ] A trial in which every heartbeat fails: rolled back by `kNoUplink` at the window's end, and the journal's line says how many were tried (test).
- [ ] A rollback followed by the old image's boot with a wait in its state: the apply's result is posted in that boot's first wake (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Which call fails at every wake of the unit: the report of pile 13 is how that is found, once an
image that carries it is on the unit. A wake by the radio's line inside a wait (phase 13's first
follow-up): the same root, its own task. The bad list: v0.1.6 stays on it, and the next release
is another image. The board: no session opens a port. A release: Thomas presses the button.
