# 01 — What the unit says about itself

**Depends on:** —
**Spec:** `components/agent/include/walter_agent/firmware_jobs.h` and `firmware_jobs.cpp` (the four job types as an `mm::IJobExtension`; `kDataBytes`; how `twinoak.fw.state` writes its result), `components/agent-core/include/agent/IJobExtension.h` (`kMaxExtensionTypes` = 8, four used), `JobRunner.h` (`kDataCap` 8192, `kBodyCap`), `JobEnvelopes.h` (`kClaimMax` = 2, `kMaxJobTypeLen`), `components/agent-core/src/AgentLoop.cpp` (`wakeJobs`: one claim a wake), `components/agent/include/walter_agent/agent_backend.h` (`kWakePostBytes`, 32 kB a wake), `components/core/console/console.cpp` and `commands.cpp` (the printers of `wake`, `agent state`, `agent obs`, `agent jobs`, `journal`, `lte`, `rak status`, `rak watch`, `rak events`, `power`, `reboots`, `heap`, `dump`, and of the settings), `docs/console.md` (what each prints, and which only read), `docs/cycle.md`, `spec/SYSTEM.md` S-12 (a stranger's name is never logged), `host_test/` (`agent`, `agent-backend`, `fixtures`, and the contract fixtures copied from meshmaster.dk); **meshmaster.dk, read only:** `docs/dev/unit-protocol.md` §5 and §8, `tests/fixtures/contracts/FirmwareState*.json`.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

On 13 November a wake request reached the unit's radio, the radio answered *ok*, and the unit did
not check in. Why not is written on the unit: `wake` lists the last ten wakes and what each did,
`agent state` says what wait is owed, `lte` says how far the uplink got. All of it is behind a USB
cable, on a unit that is meant to sit on a mast.

This task lets meshmaster.dk ask for it. One new job type returns the console's own lines, by
section, and nothing else changes. Its reader is whoever has to find a fault without the cable, so
the test of every section is: **does it answer the question in the table below?**

## The contract

meshmaster.dk gets the same text in its task 216. Until its `docs/dev/unit-protocol.md` has it,
this is the page.

```
Type      twinoak.report
          credential class none; the unit's own uplink only; nothing on the air;
          two attempts, as twinoak.fw.state (it changes nothing on the unit).

Payload   UnitReportPayload.json
{ "sections": ["wakes", "agent", "calls", "uplink", "observations", "jobs"] }

  sections   1 to 12 names, each 1 to 16 characters of a-z. The order is the order they are
             written in.

Result    UnitReportResult.json
{
  "at": 1794595200,
  "sections": [
    { "name": "wakes", "text": "first line\nsecond line", "cut": false },
    { "name": "agent", "text": "first line", "cut": true }
  ],
  "left": ["jobs"],
  "unknown": ["calls"]
}

  at         the unit's clock when the report was written, seconds since 1970; 0 when not set.
  sections   the sections written, in the order asked. text is the section's lines joined with
             \n, with no \n at the end and no other control character. cut is true when lines
             were left off its end to make the result fit.
  left       names asked for that this firmware has and did not start: the result was full.
  unknown    names asked for that this firmware has no section for.

  The whole data object is at most 8000 bytes. The outcome is ok whenever the job ran.
```

## The sections

| name | the console lines it is | the question it has to answer |
|---|---|---|
| `wakes` | `wake` | Did it wake at that time, by what (the timer, the radio's line and for which reason, a spurious wake), how long was it awake, did the uplink come up, did the server answer, did a job run? How many spurious wakes, and is the line armed? |
| `agent` | `agent state` | Is a wait owed, how long and why? How many failures in a row, failing for how long? Every counter the heartbeat carries. Is the unit's clock set, and from where? |
| `uplink` | `lte` | What did the last attempt do and how far did it get? Is a backoff in force? What signal does the modem report? |
| `observations` | `agent obs`, without a pull | How many adverts wait, were heard, were dropped, and where: the radio's ring, this ring, or refused by the server? |
| `jobs` | `agent jobs` | The last ten jobs: type, outcome, how long, and whether a result still waits. |
| `journal` | `journal` | The last entries of the flash journal, in words. |
| `radio` | `rak status`, `rak watch`, and the counts of `rak events` | Is the radio healthy, why did it last boot, is an attention pending and for which reason, how many events were seen, dropped or lost, and how many link retries and timeouts? |
| `power` | `power` | The band, the readings behind it, and the interval it chose. |
| `restarts` | `reboots`, `heap` | Why this board last restarted and the four before, and the heap's low-water mark. |
| `settings` | every setting, as the console prints them | What is set, and to what. A secret prints as *set* or *unset*. |
| `log` | `dump` | The last lines this port printed in this wake, oldest first. |

`calls` is task 02's. Until then it is answered in `unknown`, which is what `unknown` is for.

## Deliverables

1. **The fifth job type**, in the same extension as the four. The heartbeat advertises it.
2. **One printer for both readers.** A section's lines are the lines its console command prints:
   the same code writes both, and no wording exists twice. Say in the report how the lines are
   taken. No heap; every buffer a member with a named bound.
3. **Read only.** No section changes anything: `agent obs` without its pull, and the counts of
   `rak events` without taking the events out of the queue. `radio` may ask the radio over the
   link; nothing goes on the air.
4. **It fits.** Sections are written in the order asked. One that does not fit whole is cut at a
   line end and marked; ones not started are named in `left`. Where a list is newest-first on the
   console, the cut loses the oldest.
5. **Nothing it must not say.** No secret's value (the token, a PIN, a password, a key), whoever
   typed it and whenever. No stranger's advert name (S-12). The unit's own name is the server's
   already and may stay out too.
6. **Where a section does not answer its question today**, add the line to the console command, so
   the cable and the report gain it together. Say which lines are new. If the answer needs the
   radio to say something its `STATUS` does not carry (how many attentions it raised, and how many
   the hourly limit refused), change nothing and leave a line under FOLLOWUPS: the link protocol
   is the design repository's.
7. **What it costs**, in the report, from the code: the bytes of two report results against one
   wake's 32 kB, what happens to the second when the budget is spent, and how long the job holds a
   wake open.
8. **The fixtures**, as the two blocks above, where the copied ones are, marked as *not yet in
   meshmaster.dk* (its task 216). A FOLLOWUPS line to compare them byte for byte once it is.
9. **The words.** `docs/console.md` (a command that gained a line), `docs/cycle.md` or wherever the
   four job types are listed for a reader, and the README's one line. A line in
   `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] A claimed `twinoak.report` for `wakes, agent, uplink, observations, jobs` answers the contract's shape, and each section's text is what the console command prints in the same state (tests, one per section).
- [ ] All eleven names are answered; `calls` and a name nobody has are in `unknown` (test).
- [ ] A scripted unit: woken by the radio's line with a wait still owed, nothing sent, asleep again. The next report's `wakes` shows that wake and its reason, and `agent` shows the wait (test).
- [ ] A scripted unit: three spurious wakes. `wakes` shows them, the count, and the line disarmed (test).
- [ ] A result that would pass 8000 bytes: the last section written is cut at a line end with `cut` true, the rest are in `left`, and the data is valid JSON (test).
- [ ] With a token, a PIN and a password set, a token typed at the console in this boot, and an advert heard from a stranger with a name: none of the five values is anywhere in a report of all eleven sections (test).
- [ ] The heartbeat's job types include `twinoak.report`, and the four others are unchanged (test).
- [ ] No section changes a counter, a queue or a record: two reports in a row differ only in `at` and in what the first one's own run added to `log` and `jobs` (test).
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Anything that changes what the unit does on a wake. meshmaster.dk's side. The board: no session
opens a port. A release: Thomas presses the button.
