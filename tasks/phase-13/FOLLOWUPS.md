# Phase 13 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] A wake by the radio's line inside a wait sends nothing.** Read in the code on
  13 November, not seen on a board: `WakeReason::timeAwayMs()` is 0 for any wake but the timer, so
  `AgentLoop::restore` still owes the whole wait a failed call left (30 s and up), and
  `wake_cycle.cpp` (~L1253) calls that expected. A wake a person asked for then does nothing
  whenever the wake before it ended in a failed call. It may be why job 28399's wake was not
  heard. To decide after the first report: the radio never sleeps, so its uptime before the sleep
  and after it is the time away, for every kind of wake.
- [ ] **[later] An attention has never been seen to wake a sleeping Walter on a board.**
  `docs/bench.md` §3's item is unticked and was written before deep sleep existed.
- [ ] **[later] A wake claims once, two jobs at most** (`AgentLoop::wakeJobs`, `kClaimMax`). With
  meshmaster.dk's diagnostics switch on, the report takes one of the two. A second claim while
  jobs wait and time is left is the library's to grow, in the exit-node firmware's repository.
- [ ] **[later] agent-core is behind**, carried from phase 12's follow-ups.
