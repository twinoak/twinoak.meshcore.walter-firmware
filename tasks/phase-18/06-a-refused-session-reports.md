# 06 — A refused session reports

**Depends on:** 05 (merged).
**Spec:** `tasks/phase-18/FOLLOWUPS.md` (*Sessions 4 and 5*) and `tasks/phase-18/reports/05.md` (deviations 5 to 7 and 9); `components/agent/firmware_jobs.cpp` (the words of a refusal ~L136–147, `refusalFor` ~L166–178, an apply's result ~L850–870, what an apply says of a flash that did not begin ~L990–1000 and ~L1085–1095, a session's result ~L1900–1932, a session's result posted from the journal ~L2150–2200 with its outcome at ~L2158, the word a session's result carries ~L2214) and `components/agent/include/walter_agent/firmware_jobs.h` (`kJournalNoteBytes` ~L149); `components/core/update/scanner_session.cpp` (`word` ~L120–130, the endings ~L182–196, ~L596, the flash that did not begin ~L849–851) and `components/core/include/core/update/scanner_session.h` (`Refusal` ~L175–190); `components/core/update/radio_update.cpp` (the reason's sentence ~L125–127, `flashFailureNote` ~L283–308); `host_test/agent/` (`test_a_flash_that_wrote_nothing.cpp`, `test_a_session_is_a_job.cpp`, `meshmaster_script.h`, `session_job.h`) and `host_test/core/update/test_a_flash_that_wrote_nothing.cpp`; `host_test/fixtures/contracts-ahead/` and its `README.md`; meshmaster.dk, read only: `src/MeshMaster.Core/NoiseScope/NoiseScopeSessionContract.cs` (~L40–70) and `src/MeshMaster.Core/Transport/AgentProtocol.cs` (the door for a result ~L1210–1230; what it answers a result it turns away, and one for a job that has ended); `docs/update.md` (*A session*, *The journal*), `docs/cycle.md`
**Size:** one session.
**Model:** claude-opus-5

## Goal

Task 05 gave a session a new ending: two flashes that wrote nothing, and the session is
`refused` with the radio as it was. On 24 November the unit at Bellevue took it (meshmaster.dk
job 43526, NoiseScope session 5): *flashing-scanner* at 13:13:34Z, the first try's line at
13:14:07Z, *refused* at 13:15:01Z. Then the job stayed leased for an hour and ended as *the
exit node took this job and never reported a result*.

What went wrong, read in both repositories:

1. **The result has no word.** 05 leaves the session's `Refusal` at none for this ending, the
   result's writer leaves `refusal` out when there is none, and meshmaster.dk turns a
   `refused` session without one away whole (*A refused session names which precondition
   stopped it*). 05's tests ran against this repository's own script of the server, which did
   not hold that. The same door meets `noMemory`, a word it has never had, and the session
   the journal would not open, which sends none on purpose (~L2214).
2. **The line that should say why is cut.** *the flash did not begin, twice: nothing was
   written and the radio is a* is seventy characters of a longer sentence, and the second
   try's step, status and address are in none of what reached the server.
3. **A session whose result waits in the journal would be reported as `restored`.** The post
   from the journal takes the outcome from one bit, open or closed (~L2158), and a session
   that ended refused is closed.

After this task every refusal a unit can make reaches the server in a word the contract has,
every line of a journal is whole, a session is reported as what it was from wherever it is
reported, and a result the server will never take is not owed for ever.

## The contract

**The words.** A refusal gains one word for an apply and a session alike, and a session's
gains two more that the Walter has had since its pile 16 and the list never did:

| word | whose | what it says |
|---|---|---|
| `flash` | an apply's and a session's | the flash did not begin: the debug port gave up twice before anything was written, and the radio is as it was |
| `noMemory` | a session's | no room for a step's lines: no external memory answered and the heap has no block of that size |
| `journal` | a session's | the journal would not take the entry that opens a session, so none was begun |

**The fixture**, `NoiseScopeSessionResultRefused.json`, one line: the session of
24 November as it should have reported.

```json
{"outcome":"refused","steps":[{"ended":"notRun","lines":0,"bytes":0}],"lines":0,"linesUnanswered":0,"journal":[{"at":"2026-11-24T13:13:33Z","step":"check","note":"battery 4112 mV, link up, clock set"},{"at":"2026-11-24T13:13:34Z","step":"flashing-scanner","note":"the scanner image is going on"},{"at":"2026-11-24T13:14:07Z","step":"flashing-scanner","note":"scan: swd wait-timeout at 0x036000; nothing written"},{"at":"2026-11-24T13:15:01Z","step":"flashing-scanner","note":"2nd try scan: swd wait-timeout at 0x036000; nothing written"},{"at":"2026-11-24T13:15:01Z","step":"refused","note":"the flash did not begin, twice; the radio is as it was"}],"refusal":"flash"}
```

meshmaster.dk's pile 4.48 task 260 carries the same words and the same line, and from it on a
refused result is taken whatever its word. Until it is deployed the server turns `flash`
away as it turns away none: no worse than today. The fixture waits in
`host_test/fixtures/contracts-ahead/` as the others do.

## Deliverables

1. **`flash`**, for an apply and for a session that ends `refused` because the flash did not
   begin, twice. **`journal`** for the session the journal would not open, where nothing is
   sent today. `noMemory` as it is.
2. **Every refusal has a word, and a test says so by counting**: for every way an apply and a
   session can end `refused` in this firmware, the result carries a word of the contract's
   list. A refusal added later without one fails that test, not a session on a unit.
3. **The fixture is what the firmware writes.** The run of 24 November on the bench (a
   scanner's flash that fails before its halt, twice, at the scan, with those times) ends in
   a result that is the fixture's members and values, and a journal of its five lines.
4. **Each failure has its line, and the ending has its own.** The second try's failure is
   journalled as the first's is, with *2nd try* before it, and the ending's line is short
   enough to be whole. **No line this firmware writes into a journal is ever cut**: the
   longest text every line can take is worked out and held under `kJournalNoteBytes` by a
   test or a `static_assert`, for the lines of 05 and for every line there was before it.
   Where a sentence is too long it is written shorter, not cut.
5. **Reported as what it was.** A session's result posted from the journal says `refused`,
   with its word, when the journal's last stage is refused; `restored` only for a session the
   golden image came back from; `stuck` as today. The same care for an apply's.
6. **A result the server will not take is not owed for ever.** Find and say in the report
   what the firmware does today when the server answers a result with a 4xx that is its
   verdict on the body, and with the answer it gives for a job that has ended. After this
   task: such an answer settles the entry, the journal says once *the server did not take the
   result* with the status, and the next wake does not post it again. A 401, a 429, a 5xx and
   no answer stay owed as today. Name the statuses from meshmaster.dk's door, read only.
7. **The script of the server does the server's rule.** `meshmaster_script.h` turns away what
   the door turns away today: a `refused` session with no word or one outside the list it is
   given. The tests of 05 that pass only because it did not are found and named.
8. **Tests:**
   - 24 November: the result and the journal of deliverable 3; against the script holding
     today's rule with `flash` in its list, the result is taken; with the list as it is on
     the server today, it is turned away, the entry is settled, and nothing is posted at the
     next wake;
   - the same session with the power pulled before the result is posted: the next wake
     reports `refused` with `flash`, not `restored`;
   - an apply the same two ways: `refused` with `flash`;
   - a session refused for memory and one the journal would not open: `noMemory`, `journal`;
   - the longest line of every kind, whole;
   - a session that restores is what it was: 03's, 04's and 05's tests pass unedited, or each
     edit is named.
9. **The words.** `docs/update.md` (*A session*: its three endings and the words a refusal
   carries; *The journal*: no line is cut), `docs/cycle.md` where a result is owed. A line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The test of 24 November: a result with the fixture's members and values and a journal of five whole lines (the test's name, the result printed in the report).
- [ ] Every `refused` ending of an apply and of a session carries a word of the list, held by one test that counts them (its name).
- [ ] No journal line can be cut: the bound and what holds it (the name), and the longest line of each kind quoted.
- [ ] A session posted from the journal after a refusal says `refused` and its word (the test's name).
- [ ] A result the server turns away is settled and not posted again; the statuses are named from meshmaster.dk's source (the test's name, the lines read).
- [ ] The script holds the server's rule, and the tests it used to let through are named.
- [ ] `tools\verify.ps1` green. CI green.

## What only a board can show

Nothing new: the next session on the unit that is refused this way reads so on its page within
the minute. Thomas's, from meshmaster.dk; I read it.

## Out of scope

**Why the first try fails**: three sessions of three on 0.1.12 stopped at the same address,
`0x036000`, thirty-two seconds into the flash. Not chance, not found, and a task of its own.
meshmaster.dk's half. A session started again by the firmware after a refusal: every session
is started by a person. The board. A release: Thomas presses the button.
