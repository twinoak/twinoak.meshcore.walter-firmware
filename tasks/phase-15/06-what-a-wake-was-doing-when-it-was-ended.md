# 06 — What a wake was doing when it was ended

**Depends on:** —
**Spec:** `components/core/include/core/cycle/wake_guard.h` (`kWakeBoundMs` ~L52, and why a wake that overruns is ended by a reset), `components/core/cycle/wake_guard.cpp` (~L142: `Restart::kWakeOverrun`), `components/core/include/core/health/health.h` (~L47), `components/core/include/core/cycle/wake_reason.h` (`WakeEntry` ~L213 and *0 means it never reached sleep* ~L217; the retained record and its size), `components/core/cycle/wake_reason.cpp` (~L162: an overrun reads as a watchdog; ~L370), `components/core/cycle/wake_cycle.cpp` (the ten steps; `journalWake`), `components/core/include/core/cycle/wake_counters.h` (the four totals, `agent_wakes`), `components/agent/identity.cpp` (~L161: the totals in the heartbeat), `components/agent/modem_uplink.cpp` (`call` ~L196: which call is out), `components/agent/include/walter_agent/link_mesh.h` (the local door and the mesh a job is run through), `components/core/console/console.cpp` (the list of ten ~L3975–4025), `components/core/include/core/power/interval_policy.h` (`kMaxFaultDoublings`), `components/interfaces/include/interfaces/retained.h` (`kRetainedRecordMax`), `docs/cycle.md`
**Size:** one session.
**Model:** claude-opus-5

## Goal

In the night of 17 November the unit took the hourly `status` job of its own repeater
(meshmaster.dk job 32763) at 01:29 UTC and again at 02:05. Both times it sent its heartbeat,
claimed the job, and was never heard again in that wake: five minutes later the bound on a wake
ended it by reset, the next boot read that as a watchdog, and the unit slept thirty minutes
instead of fifteen. The job expired unanswered and nothing was measured for over two hours.

What those two wakes were doing for five minutes, nobody can say. The journal has *woke by
watchdog*. The list of ten says *it never reached sleep (a reset, a crash or the bound)*.
The log, the list of jobs and the letters of the wake were in RAM, and the reset took them.

A wake that is ended must leave word of what it was in the middle of.

## Deliverables

1. **Where a wake is, written down as it goes**, in memory a reset leaves and a power cut
   takes. No flash. It holds:
   - the step of the ten;
   - inside the exchange with the server: which call is out (config, heartbeat, claim, a job's
     result, an extend, progress, observations), or that a claimed job is being run, of which
     type, and how many requests it has made of the radio or the mesh;
   - that the modem or the link is being waited on, when this layer knows it;
   - the second of the wake at which that began.

   Written when it changes. A few bytes. Everything this board knows without changing the
   copied library; say in the report what it could not know.
2. **The boot after says it.** When the wake before did not reach sleep, whatever ended it (the
   bound, a crash, the chip's own watchdog): one line in the journal, and that wake's entry in
   the list of ten, saying the step, what was out or being run, and for how long.
3. **Counted.** A fifth total beside the four of `agent_wakes`: wakes that did not reach sleep.
   Kept as they are kept, and sent in the heartbeat as they are sent, so that meshmaster.dk's
   counters show the next one when it happens.
4. **Read from afar.** The report's `wakes` section carries the line.
5. **Room.** The retained record stays inside `kRetainedRecordMax`.
   `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The words.** `docs/cycle.md`, `docs/console.md`. `docs/bench.md`: a line to run on a
   board (a wake held past its bound with `stay` off, and what the next boot prints). One line
   in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] A scripted wake that hangs in the second request of a claimed `status` job until the bound: the next boot's journal line and the list of ten say step 8, a job being run, `status`, one request answered, and the seconds (test).
- [ ] A scripted wake that hangs in the observations call: they name the call (test).
- [ ] A scripted wake that hangs before the uplink is up: they name the step (test).
- [ ] A wake ended by a crash in the same place: the same line, with the reason the boot already gives (test).
- [ ] A wake that reaches sleep: no line, and its entry as it is today (test).
- [ ] The fifth total moves by one for each of the three, survives a sleep and a power cut, and is among the heartbeat's counters (test).
- [ ] A retained record written before this task reads back with no such word, and nothing is refused (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Why the two wakes of 17 November hung: this task is how the next one is read. The bound's five
minutes, and the longer sleep after it. The library: copied, not changed here. What
meshmaster.dk says of a job taken and never answered (its pile 4.40). The board: no session
opens a port. A release: Thomas presses the button.
