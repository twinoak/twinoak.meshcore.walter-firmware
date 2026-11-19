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
