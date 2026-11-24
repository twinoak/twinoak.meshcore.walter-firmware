# Contract fixtures this end has and meshmaster.dk has not — yet

Every other shape this firmware sends or reads is held to **meshmaster.dk's own bytes**: the
copied contract fixtures under `host_test/agent-core/tests/fixtures/contracts/`, read at run time
and never transcribed (`tasks/README.md`, `docs/upstream.md`). That tree is a copy, checked byte
for byte against its `MANIFEST.txt` before anything is built, so a shape that does not exist on
the server yet cannot be put there.

This directory is where such a shape waits. A file here is **not meshmaster.dk's**: it is this
firmware's reading of a contract agreed in words, written as the two blocks of the task that
agreed it, and it must be replaced by the server's own file — not merely compared with it — as
soon as that file exists.

| file | the contract | whose task | until then |
|---|---|---|---|
| `UnitReportPayload.json` | `twinoak.report`'s payload | meshmaster.dk's 216 | written here from `tasks/phase-13/01-what-the-unit-says.md`'s *The contract* |
| `UnitReportResult.json` | `twinoak.report`'s result | meshmaster.dk's 216 | the same |
| `FirmwareApplyProgress.json` | `twinoak.fw.apply`'s progress report — the journal as it is written | meshmaster.dk's 227 | written here from `tasks/phase-14/04-an-apply-keeps-its-lease.md`'s *The contract* |
| `FirmwareOfferPayloadScanner.json` | `twinoak.fw.offer`'s payload with `into` — the member that sends the release's scanner image into the scanner slot | meshmaster.dk's 235 | written here from `tasks/phase-16/02-an-offer-into-the-scanner-slot.md`'s *The contract* |
| `NoiseScopeSessionPayload.json` | `twinoak.noisescope.session`'s payload — the slot and one to six steps | meshmaster.dk's 236 | written here from `tasks/phase-16/04-a-session-is-a-job.md`'s *The contract* |
| `NoiseScopeSessionStep.json` | one step of that payload — a command and its seconds | meshmaster.dk's 236 | the same |
| `NoiseScopeLinesReply.json` | what a post of a scanner's lines is answered with — how many the server holds, and whether its room is used up | meshmaster.dk's 236 | the same |
| `NoiseScopeSessionResult.json` | the session's result — the outcome, the scanner's version, a row per step, the two counts, the journal, the refusal | meshmaster.dk's 236 | the same |
| `NoiseScopeStepResult.json` | one step's row of that result — how it ended, and what it handed to its posts | meshmaster.dk's 236 | the same |
| `NoiseScopeSessionResultRefused.json` | the session of 24 November as it should have reported — `refused` with `flash`, and a journal of five whole lines | meshmaster.dk's 260 | written here from `tasks/phase-18/06-a-refused-session-reports.md`'s *The contract*, which is task 260's own block |

`tasks/phase-13/FOLLOWUPS.md` carries the item that compares the first two byte for byte once task
216 is deployed, `tasks/phase-14/FOLLOWUPS.md` the same for the third once task 227 is, and
`tasks/phase-16/FOLLOWUPS.md` the same for the fourth once task 235 is and for the five of
`twinoak.noisescope.session` once task 236 is; each moves the files into the copied tree where
every other fixture is.

The five session files are **the task's own five lines**, read at run time:
`host_test/agent/test_a_session_is_a_job.cpp` collects every line of
`tasks/phase-16/04-a-session-is-a-job.md` that begins a JSON object and holds each file against
it, so a byte that drifts in either is a failing test. The sixth,
`NoiseScopeSessionResultRefused.json`, is held the same way against
`tasks/phase-18/06-a-refused-session-reports.md` by
`host_test/agent/test_a_refused_session_reports.cpp`, and that test also holds **what the firmware
writes** against it: the fixture is not a transcription of a shape, it is the result of the session
of 24 November.

The sixth is replaced by the server's own file - not merely compared with it - once
meshmaster.dk's task 260 is deployed, as the first line of this file asks of every file here.
