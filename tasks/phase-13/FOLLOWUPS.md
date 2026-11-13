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
- [ ] **[later] `twinoak.report`'s two fixtures are this end's words, not meshmaster.dk's.**
  `host_test/fixtures/contracts-ahead/UnitReportPayload.json` and `UnitReportResult.json` were
  written from task 01's *The contract* block, because the copied tree
  (`host_test/agent-core/tests/fixtures/contracts/`) is held byte for byte to its manifest and a
  shape the server does not have yet cannot be put there. Once meshmaster.dk's task 216 is
  deployed: compare the two byte for byte with the server's own, and move them into the copied
  tree where every other fixture is. That directory's README says the same.
- [ ] **[later] The radio cannot say how many attentions it raised, or how many its hourly limit
  refused.** The report's `radio` section answers the rest of its question from `STATUS`, `rak
  watch` and the link's counters, but §5.7's `STATUS` carries neither figure and nothing else
  does, so the firmware says nothing rather than guessing. The link protocol is the design
  repository's: `LINK-PROTOCOL.md` §5.7 would have to carry them.
- [ ] **[later] `agent jobs` cannot say how long a job took.** The report's `jobs` section answers
  the type, the outcome, when it ran and whether a result still waits, but not the duration: the
  copied library's `JobRunner::Record` keeps `atEpoch` and the transaction count and no start or
  finish (`components/agent-core/src/JobRunner.cpp`, `remember`). It is the exit-node firmware's
  to add, and arrives here with the next copy.
