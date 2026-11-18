# 04 — A session is a job

**Depends on:** 02, 03
**Spec:** meshmaster.dk's `docs/dev/unit-protocol.md` §3 and §5 and its pile 4.42, task 236 (the same contract as below); `spec/SYSTEM.md` S-7 and S-12; `components/agent/include/walter_agent/firmware_jobs.h` (the five types ~L88–96; the lease's three figures ~L106–120; `kJournalMaxEntries` ~L128; `kDataBytes` ~L134), `components/agent/firmware_jobs.cpp` (`extendLease` ~L285; `runApply` ~L726: the bound raised ~L805, what the wake is told it carried ~L808, the journal posted as it is written ~L810–837; `postApplyProgress` ~L614; a result owed across a restart: ~L943–957, `resultOwed` ~L990, `postOwedResult` ~L1039); `components/agent/include/walter_agent/modem_uplink.h` (`kExtensionPayloadBytes` ~L125, `postProgress` ~L255, `callJob` ~L274), `call_log.h` (`CallKind` ~L76); `components/core/include/core/cycle/wake_progress.h` (pile 15, task 06); `host_test/fixtures/contracts-ahead/README.md`; `docs/cycle.md` (the page for the implementer of `Backend`); task 03's report.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

Task 03 made a session something a person starts at the console, with its lines printed on the
port. A unit on a mast has no port. This task is the session as the server's job: the program
comes in the payload, the scanner's lines go to the server step by step, and the result says
how it ended.

## The contract

```
Type      twinoak.noisescope.session
          credential class none; the unit's own uplink only; nothing on the air;
          control and dangerous; one attempt; leased 60 minutes from the claim.

Payload   NoiseScopeSessionPayload.json, at most 384 bytes as sent
{"slot":0,"steps":[{"command":"stat","seconds":30},{"command":"sweep","seconds":30},{"command":"hist 869.4 869.8 25","seconds":40},{"command":"dwell 869.618 60","seconds":70},{"command":"scope 863 870 250 0 1000 max","seconds":20},{"command":"cad","seconds":20}]}

  slot      the radio position, as an apply names it.
  steps     1 to 6, run in order. NoiseScopeSessionStep.json:
{"command":"hist 869.4 869.8 25","seconds":40}
  command   a line for the scanner, 1 to 40 characters of a-z, 0-9, space and the full stop.
            Its first word is one of stat, sweep, scope, hist, dwell, cad.
  seconds   5 to 300: the most the step lasts. A step ends when the scanner says it is done,
            or when its seconds are up, whichever is first.
            The seconds of all steps come to 600 at most.

Lines     POST /agent/v1/jobs/{id}/lines/{from}
          The body is what the scanner said: its lines that begin with {, each as it was
          sent and ended with \n, at most 8192 bytes a post. {from} is how many lines of this
          job the unit has handed to a post before this one; 0 for the first.
          Answered with NoiseScopeLinesReply.json:
{"held":25,"full":false}
  held      how many lines of this job the server holds now.
  full      true when the job's room is used up: the unit posts no more lines and goes on.

Progress  POST /agent/v1/jobs/{id}/progress, FirmwareApplyProgress.json as an apply's is:
          the journal written so far, and the newest step as the note.

Result    NoiseScopeSessionResult.json
{"outcome":"restored","scanner":"0.1.6+fcf52d5","steps":[{"ended":"clock","lines":7,"bytes":1810},{"ended":"done","lines":3,"bytes":4312},{"ended":"notRun","lines":0,"bytes":0}],"lines":10,"linesUnanswered":0,"journal":[{"at":"2026-11-18T10:00:00Z","step":"check","note":"battery 4012 mV, link up, clock set"},{"at":"2026-11-18T10:00:01Z","step":"flashing-scanner","note":"0.1.6+fcf52d5, 414208 bytes"},{"at":"2026-11-18T10:15:02Z","step":"scanner","note":"a scanner answered"},{"at":"2026-11-18T10:15:04Z","step":"step-1","note":"stat"},{"at":"2026-11-18T10:15:36Z","step":"step-2","note":"sweep"},{"at":"2026-11-18T10:16:10Z","step":"flashing-golden","note":"0.1.6+fcf52d5"},{"at":"2026-11-18T10:31:14Z","step":"proving"},{"at":"2026-11-18T10:34:20Z","step":"restored","note":"0.1.6+fcf52d5 proved"}],"refusal":"battery"}

  outcome          restored   the radio was a scanner, for all of the program or part of it or
                              none, and the golden image is back and proved. Envelope ok.
                   refused    nothing was touched; `refusal` says why. Envelope failed.
                   stuck      the golden image did not come back. Envelope failed.
  scanner          the version of the scanner image that ran; absent when none did.
  steps            one for each step of the payload, in order. NoiseScopeStepResult.json:
{"ended":"clock","lines":21,"bytes":9120}
  ended            done     the scanner said the command was done
                   clock    its seconds ran out and the unit said stop
                   full     the unit's room for one step was used up and it said stop
                   refused  the scanner answered the command with ok false
                   silent   the scanner did not answer the command
                   notRun   the session ended before this step
  lines, bytes     what the step handed to its posts.
  lines            how many lines the unit handed to posts in all.
  linesUnanswered  how many of them were in posts that got no answer. They may have arrived.
  journal          FirmwareJournalEntry.json, bounded as an apply's: 40 entries and 2 000
                   characters. The step words are the unit's.
  refusal          battery, link, clock, inProgress, noGolden, noScanner, program, noMemory;
                   absent unless the outcome is refused.
```

The five lines above are the fixtures, byte for byte, each under the name given beside it.
meshmaster.dk's task 236 is written against the same lines.

**How the server treats a post**, so that this end need not care: a line it already holds is
left alone, so the same post sent twice changes nothing; a `from` past what it holds is
accepted and the gap counted as lost.

## Deliverables

1. **The sixth type**, in the heartbeat's list after the five. The five fixtures wait in
   `host_test/fixtures/contracts-ahead/` as given, with their rows in that directory's README.
2. **The payload is read** and held to task 03's rules by task 03's own code. A program that
   breaks one, a payload that did not fit what a claim keeps, or a slot this board has not:
   `refused` with `program`, and the radio is not touched.
3. **The session of task 03 is run as an apply is run**: in slices inside the wake, the bound
   on the wake raised to the session's, the wake told that its one update went (S-7).
4. **The lease.** Extended before the first byte; for the server's whole ceiling before each
   of the two flashes, as an apply's flash is; and every `kExtendEveryMs` otherwise.
5. **The journal as progress**, one post an entry, as an apply's.
6. **The lines.** When a step hands its lines over they are posted before the next step
   begins: bodies of at most 8192 bytes, cut at a line's end, `{from}` counting the lines
   handed to posts so far. A post that gets no answer is sent once more with the same
   `{from}`; after that this end goes on to the next lines and counts those as unanswered.
   When a reply says `full`, no more lines are posted and the session goes on to its end. On
   `ModemUplink`, with a call kind of its own in the call log and in what a wake was doing;
   the copied library is not changed.
7. **The uplink never holds the radio.** No post is waited for beyond its own call. With no
   uplink at all the steps run, the lines are counted as unanswered, and the repeater comes
   back when the program is over. Task 03's ceiling stands whatever a post is doing.
8. **The result**, inside `kDataBytes`: the outcome, the scanner's version, a row for every
   step of the payload, the two counts, the session's journal bounded as an apply's (task
   03's `room` line among its entries, so that the heap's figures reach the server with every
   session), the refusal. The envelope's outcome as the contract says.
9. **A result that outlives a restart.** Before the scanner is flashed the job is journalled
   as owed, as this firmware's own apply is. A boot that finds a session open and its job
   owed ends the session (task 03) and then posts `restored` or `stuck` with the journal it
   has, `notRun` for every step it cannot vouch for, and counts of nought.
10. **S-12.** A scanner's line goes into a post and nowhere else: not the log, not the
    journal, not a report, not the job's `log`.
11. **The words.** `docs/cycle.md`, `docs/console.md` (`agent jobs`, `agent calls`),
    `docs/update.md`. `docs/bench.md` §17, one item: a session from a bench server, with the
    posts and their `{from}` read off `agent calls`. One line in `release-notes/unreleased/`.

## Acceptance criteria

Against the scripted server, flasher and scanner.

- [ ] The five fixtures in `contracts-ahead` are the lines above byte for byte; the payload fixture is read into the six steps, and a result written from the fixture's values is the result fixture byte for byte (tests).
- [ ] The heartbeat advertises `twinoak.noisescope.session` as its sixth extension type (test).
- [ ] A session of two steps whose lines come to 3 and 21: posts at `{from}` 0 and 3, each body the lines whole and in order; then the result with `lines` 24 and `linesUnanswered` 0 (test).
- [ ] A step whose lines fill the room of 8 192 bytes: one post of at most 8192 bytes, every line in it whole (test).
- [ ] A post whose answer never comes: sent once more with the same `{from}`; then the next lines go with the next `{from}` and `linesUnanswered` is the count of that post (test).
- [ ] A reply with `full` true: no further post, the remaining steps still run, the result is `restored` (test).
- [ ] No uplink from the second step on: the steps run, the golden image is back, the result is owed and posted at the next wake that reaches the server (test).
- [ ] The lease is extended for 1 800 seconds before each flash and for 900 every five minutes otherwise (test).
- [ ] A progress post follows every journal entry, and the last one's note is `restored` (test).
- [ ] Each refusal: a payload with seven steps, one with a command beginning `dfu`, one the claim could not keep, a slot of 1 on a single adapter: `refused` with `program`, no flash (tests). The refusals of task 03's check arrive with their own words (test for `noScanner` and `battery`).
- [ ] A restart while the radio is a scanner: the next boot restores, then posts `restored` with the steps `notRun` (test).
- [ ] A session job claimed while a radio update is in progress, or while this firmware is on trial: `refused` with `inProgress` (tests).
- [ ] No text of a scanner's line is in the log, the journal, a report or the job's `log` (test).
- [ ] The result of a session of six steps with a full journal fits `kDataBytes` (test).
- [ ] The result's journal carries task 03's `room` line, and so does the last progress post before the switch back (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The server's half. A session on a schedule: there is none, and this firmware begins none. A
live picture while the session runs. A session over several wakes. The scanner's firmware. The
board: no session opens a port. A release: Thomas presses the button.
