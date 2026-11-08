# Phase 10 — a unit that is heard (v0.1.3)

Written on 8 November, after pile 9 merged and v0.1.2 was published. The unit was still at 0.1.1 on
the bench.

**What the morning showed.** The unit woke every 15 minutes all night, 56 wakes since the power-on
at 20:37, and meshmaster.dk heard it on about every sixth wake. It showed *quiet*, and two jobs waited
from 09:25 UTC. `agent state` had the cause: a failed call sets a hold of 30 s or more, the library
saved what was left of it across the sleep, and the sleep counted for nothing. So the hold was spent
five seconds at a time across short wakes that each sent nothing. At the usual 60-minute interval
that is six hours of silence after one failed call, which may well be what Thomas saw on 6 November.
The exit-node firmware's 42 fixes it in agent-core. 01 brings it in.

The rest is what the night's reports left for a Walter pile:

- the journal's *the backend was reached* on wakes that sent nothing (02);
- the repeater's v0.1.2 refusing when it has lost its identity, which this firmware cannot yet tell
  from a radio awaiting its first install (03);
- the download message that promises a resume (04);
- **the download itself** (05). At the first update over LTE, job 22841 failed in two seconds. The
  download has never sent the `X-Agent-Version` header meshmaster.dk requires on `/agent/v1`, so no
  release of either box could ever be fetched. meshmaster.dk's pile 4.28 (task 177) lets the units
  already out there download without it, and this task sends it;
- **the stall that silences the unit** (06). With adverts waiting, the answer to an observations POST
  is lost almost every time, and meshmaster.dk's log shows it was sent. Its trace needed a special
  build. 06 puts it on the console, so the bench can capture it with the release build.

**Prerequisite:** **the exit-node firmware's pile 1.10, tasks 42 and 43**, merged into its `master`
(they change `lib/agent-core`). Task 01 syncs at that commit and stops (`logs/STOPPED-01.md`) if they
have not landed. **Start that pile first.**

**Not on the board.** The first unit is on Thomas's bench. No session opens its ports; each task adds
its bench lines to `docs/bench.md`. Findings from the bench of 8 November (the update to 0.1.2, the
connection test) may be added here as tasks before 07.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-wait-sleeps-too](01-the-wait-sleeps-too.md) | the exit-node firmware's 42 and 43 | agent-core synced; a timer sleep takes its own length off a hold, never more than was slept; a local job priced as local; `agent state` shows the time away |
| 02 | [what-a-wake-did](02-what-a-wake-did.md) | 01 | the seam tells *answered*, *sent nothing (why)* and *failed* apart; the journal says *reached* only for an answered heartbeat; `wake` says per wake what it did |
| 03 | [a-radio-without-its-identity](03-a-radio-without-its-identity.md) | — | a radio's identity known, not made yet, or lost; `agent id` and the heartbeat's counters say *lost*; an update's health check spends no rollback on it; the pages that say the key was kept corrected |
| 04 | [the-download-that-does-not-resume](04-the-download-that-does-not-resume.md) | — | the two messages stop promising a resume for the Walter's own image; the bench's figure set against the budget |
| 05 | [the-fetch-says-who-it-is](05-the-fetch-says-who-it-is.md) | — | every request to `server.url` carries `X-Agent-Version`; a refusal names its status and file; the host tests' fake server refuses what meshmaster.dk refuses |
| 06 | [a-trace-from-the-console](06-a-trace-from-the-console.md) | 05 | the modem library's TX/RX trace in the release build, switched on from the console until the next boot; no token in any trace line; `docs/bench.md` §13 needs no special build |
| 07 | [the-release-0-1-3](07-the-release-0-1-3.md) | 01–06 | `v0.1.3` published as *Latest* and verified |

```powershell
.\tasks\run-all.ps1 -Phase '10'
```

## Models

- **Opus**: all seven.

## Afterwards (Thomas)

- **0.1.2 → 0.1.3 over LTE**, once meshmaster.dk's 4.28 is deployed: *Offer a release*, a wake,
  *Apply the candidate*. Before that, the cable.
- **Then the bench:**
  - unplug, and watch meshmaster.dk hear the unit at every wake, a failed call or not;
  - `agent state` after a failed call shows the hold the sleep took off;
  - `wake` and `journal` say what each wake did;
  - `trace modem on`, `lte up`, `agent wake` with adverts waiting: the stall's trace, for its fix.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
