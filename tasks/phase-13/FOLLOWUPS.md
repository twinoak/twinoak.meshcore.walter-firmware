# Phase 13 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] A wake by the radio's line inside a wait sends nothing.** Read in the code on
  13 November, not seen on a board: `WakeReason::timeAwayMs()` is 0 for any wake but the timer, so
  `AgentLoop::restore` still owes the whole wait a failed call left (30 s and up), and
  `wake_cycle.cpp` (~L1253) calls that expected. A wake a person asked for then does nothing
  whenever the wake before it ended in a failed call. It may be why job 28399's wake was not
  heard. To decide after the first report: the radio never sleeps, so its uptime before the sleep
  and after it is the time away, for every kind of wake.
  **Task p14-06 did exactly that** (15 November 2026): the board reads the radio's `uptime_s`
  after the agent's state is saved and before `SLEEP_NOTICE`, writes it down with the radio's
  `boot_id` in the duty-cycle record, and reads it again at a boot that is not a timer wake —
  before the state is restored — so the span is the time away, less a second for the whole
  seconds at either end and less the radio clock's own 500 ppm. A wake carrying §8's reason 4
  owes no wait at all, whether the event woke the board or step 4 found it pending in `STATUS`.
  `agent state` and `wake`'s list say which of the three measured a boot. **The box stays
  unticked**: none of it has been on a board, and the item above it is the one that has to be
  walked first.
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

- [ ] **[later] The heap at start is down to 116 bytes of margin.** `tools/heap_at_start.py` over
  this task's build: the 20 480-byte kept scratch is carved out of a free block of 20 596, and
  **117 more bytes of `.data` or `.bss` move it into another heap** - which is what v0.1.3 died of
  (rail R22, phase 11's task 01). main was at 300 bytes before this task and this task spent 184
  of them on what it had to keep: two bytes an entry in the wake record (why a wake sent nothing,
  and the seconds), the eight failed calls packed to 72 bytes, and the four wake counters. The
  rest of it was given back by holding the ring in the form it is stored in and the counters to
  16 bits. **The next task that adds static storage has to make room first**, and the place to
  make it is `agent::kKeptScratchBytes` (20 480) or the library's runner beside it, not another
  round of byte-shaving: those two are the allocations the margin is measured against.

- [ ] **[later] A failed call that never left the board is in no list.** `agent calls` holds the
  calls the server answered badly or did not answer; a call the uplink refused before anything
  went out - not configured, a URL that did not fit, the 120-a-minute ceiling, too little of the
  wake's deadline left - is counted by `ModemUplink::uplinkCounters()` and `agent probe` and is
  nowhere in the list. That is deliberate (*what came back* is the list's question), but a unit
  whose wakes all end in *budget* would show an empty list and a person would read it as a unit
  with no trouble. Either the list or the `calls` section should say the four refusal counts
  beside it.

- [ ] **[later] A download range's failure has no `Retry-After` and no byte count in the list.**
  `core::update::FetchResult` carries both and the entry keeps neither: an entry is nine bytes and
  the eight have to fit one 160-character value of the settings store. If the list is ever wanted
  wider, the place to put the extra bytes is a second key, not a longer entry.
- [ ] **[next pile] The notes' pull request says no check runs. One does.** The body the release
  writes (`.github/workflows/release.yml` ~L439) and `docs/release.md` (~L88) say GitHub starts no
  workflow from an event raised with the job's own token. For v0.1.5's pull request (#60) GitHub
  started a `ci` run and held it for a maintainer's approval (*action required*); unapproved, such
  a run expires after about an hour and is listed as failed, as the repeater's did on 13 November
  (its run 43467131534). The run staying green and *merge by hand* are right as they are. The
  sentence should say: approve the run and merge when it is green, or merge without waiting.
