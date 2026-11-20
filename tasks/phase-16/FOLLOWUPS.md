# Phase 16 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] The fixtures of tasks 02 and 04 wait in `host_test/fixtures/contracts-ahead/`.**
  Once meshmaster.dk's pile 4.42 is merged, compare the six files byte for byte with its
  `tests/fixtures/contracts/` and move them into the copied tree with the others:
  `FirmwareOfferPayloadScanner.json`, `NoiseScopeSessionPayload.json`,
  `NoiseScopeSessionStep.json`, `NoiseScopeLinesReply.json`, `NoiseScopeSessionResult.json`,
  `NoiseScopeStepResult.json`.
- [ ] **[later] A session is two flashes of fifteen minutes around a few minutes of measuring.**
  14 min 44 s and 14 min 51 s for 414 kB on 13 and 14 November, by the unit's own journal:
  about 470 bytes a second. If sessions are to be routine, the flasher is where the time is.
  Nothing here looked at why it is that slow.
- [ ] **[Thomas, the bench] The PSRAM has never been brought up on a board by this firmware.**
  Task 05 switches it on for the room of a session and nothing else, and no test here can
  prove a PSRAM answers: QEMU boots the release with none. Its ⚠ items in `docs/bench.md` are
  the proof, on a spare board, before the release that carries it. The unit in service is
  covered by the trial and the rollback if a boot fails, and by the fall-back to the heap if
  the PSRAM does not come up; neither is a reason to skip the bench.
  **Still open, and now about merged firmware** (task p17-02, 19 November): task 05's
  implementation was reverted for want of 5 256 bytes and is back, so the items exist and are
  written — `docs/bench.md` §1, *The PSRAM, proved by a Walter with no radio at all* (`heap`,
  `heap room`, `heap`), *No kilobyte lost against the release before* and *A wake with no session
  is as long as it was*; §9, *A minute of `scope` in 64 kB of PSRAM*. Nothing has changed about
  what is unproved: no test in this repository can bring a PSRAM up, and the first proof is a
  board.
- [ ] **[later] Where no PSRAM comes up, one step's lines are 8 192 bytes**, because that is
  what the heap of a unit in service has in one piece: *27472 bytes free, largest free block
  12288* by its own report in November. A minute of `scope` at 250 points then ends `full`
  after about thirteen frames. Every session's `room` line says which room it had.
  **This is now the fall-back and not the rule** (task p17-02, 19 November): a board whose PSRAM
  answers gets 64 kB, and the 8 192 bytes are what a board with none, with the 8 MB octal
  variant, or with a chip that fails its pattern still gets. The `room` line of every session
  says which, so the question *how often is it the small room* is answerable from what units
  post rather than guessed at — and nothing has measured it yet, because no board has run this.
- [ ] **[later] 64 kB a step is a choice, not a limit.** The PSRAM has 2 MB. What bounds the
  room is what leaves the board: six steps of it are 384 kB against the 512 kB meshmaster.dk
  keeps of a session, and every 8 kB is a post over LTE-M. A `hist` of the whole band (about
  56 kB) now fits; a `scope` of 2048 points at a frame a second for a minute (160 kB) does not.
  **True of merged firmware since task p17-02** (19 November), and the one number to change is
  `core::update::kStepBytesExternal`. What it would cost is the uplink and nothing else: the room
  itself is external memory with 2 MB behind it, and no byte of static DRAM moves with it. The
  figure to decide it on is the `room` line units post, which no board has written yet.
- [ ] **[later] A session lives inside one wake.** A night of sweeps, one every ten minutes
  with the Walter asleep between, is what NoiseScope's `auto` is for and what this pile does
  not do: it would leave the site without a repeater for the night, and the scanner's lines
  would wake this board as a chattering line does (`docs/console.md` ~L1062).
- [ ] **[Thomas] `spec/SYSTEM.md`** lists *NoiseScope as a job* under *later* (§9), and its
  decisions (§3) have no line for task 03's rails: a session is opened in the journal before
  the first flash, and ended by the next wake whatever ended the last. The hardware
  repository's to add.
- [ ] **[later] Pile 15's task 04 still waits for the library's bump, and piles 12 to 15 are
  not reviewed.**
- [ ] **[later] The journal's list has no words for the agent's own entries.** The report of
  18 November, made after the update to v0.1.9, prints two entries (seq 890 and 891) as *kind
  0x0007, 48 byte(s) - nothing in this build reads it*. 0x0007 is `kAgentJob`, the agent's record
  of an apply (`core/update/journal.h` ~L86), written by
  `components/agent/firmware_jobs.cpp`; `Console::printJournalEntry` (`console.cpp` ~L4243) has
  no case for it, and `core` cannot read the agent's payload as things are.

## The review of 01–06 (19 November)

All six merged with CI green (#79 to #84); in no release yet (`VERSION` 0.1.9, ten notes
waiting). 05 merged its report alone and its two open criteria are met by phase 17's task 02;
06's open *CI green* was the pull request's, which merged.

Read: every report's deviations; the header of `core/update/scanner_session.h` and, in
`scanner_session.cpp`, the refusals at `start()`, `resume()` and the whole of `stepBack()`; how
a wake takes an open session up (`core/cycle/wake_cycle.cpp` ~L650–722) and the bounds of
`wake_guard.h`. Not read line by line: the steps of a session, `firmware_jobs`' half of 04, 01's
text mode, 02's store path, the tests. Nothing found that has to be settled before a release.
The rail that matters holds as written: the journal says open before the first flash, every
wake's step 3 takes an open session up and is allowed `kSessionBoundMs` for it (~L712), and the
switch back does not depend on a battery reading that a scanner cannot give (report 03's first
deviation, which found that hole itself).

- [ ] **[Thomas, before a session on a mast] No session has run on hardware.** Two flashes over
  SWD with a program between them, and a repeater that is off the mesh until the second has
  proved. `docs/bench.md` has the walk; the first sessions belong on a radio on a desk, with the
  power pulled once in each flash, before *Start a session* is pressed for TwinOak Bellevue.
- [ ] **[later] 03: a session that cannot be switched back is tried again at every wake, at full
  length, for ever.** `stepBack()` ends `stuck` and the next wake's step 3 takes it up again
  (`scanner_session.cpp` ~L972–1034; `wake_cycle.cpp` ~L707–722). That is the rail, and right
  for a flash that failed once. A radio that cannot be flashed at all costs a quarter of an hour
  awake at every wake, and while the radio is a scanner nothing can read the battery it is
  spending. After some tries in a row the tries could be further apart; the journal has an
  entry for each.
- [ ] **[none] 04: the lines are posted as `application/json` though they are NDJSON** (report
  04's fifth deviation, left for this comparison). meshmaster.dk's route reads the body as text
  whatever the header says (`AgentV1.cs` ~L142–147 at 28daabd). Nothing to do.
- [ ] **[later] The six fixtures of 02 and 04 still wait, and are the same bytes as
  meshmaster.dk's** (compared on 19 November, with the two `UnitReport*`).
  `FirmwareApplyProgress.json` of phase 14 is not. *Corrected on 20 November:* this was
  written as if a sync turned `main` red by a tripwire, as in the exit-node firmware's
  repository. There is no such test here. The four tests read the waiting directory by its
  path, and nothing fails when the copied tree holds the same files too. They move when a
  task moves them (the exit-node's pile 1.16, task 62, is the pattern for the tests).
