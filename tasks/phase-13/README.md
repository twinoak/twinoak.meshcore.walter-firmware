# Phase 13 — what the unit says about itself

Written on 13 November.

**What happened.** A wake request reached the unit's radio (*ok*, job 28399 on meshmaster.dk) and
the unit did not check in. The same day its heartbeat showed *httpFailures 512*. Neither could be
explained from the server: what the unit knows is behind a USB cable.

**What Thomas asked for.** The journal, the wake list and the rest, remotely, switched on from
meshmaster.dk; and that it holds what is needed to find a fault.

**What this pile builds.** One job type, `twinoak.report`, that returns the console's own lines by
section (01), and the three things the unit forgets today that a fault-finder needs (02).
meshmaster.dk's side is its tasks 216 and 217.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [what-the-unit-says](01-what-the-unit-says.md) | — | `twinoak.report`: eleven sections of the console's own lines, bounded, read only, with no secret and no stranger's name in them |
| 02 | [what-is-kept](02-what-is-kept.md) | 01 | The last eight failed calls and what came back; why a wake sent nothing, for every wake of the list; four wake counters in the heartbeat |

```powershell
.\tasks\run-all.ps1 -Phase '13'
```

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **Release** with the button, then offer and apply the Walter's own firmware to the unit from
  meshmaster.dk. The report can be asked for once meshmaster.dk's task 216 is deployed.
- **The bench, when there is time:** unchanged from phase 11, and `docs/bench.md` §3's *An
  attention wakes the Walter* is still unticked.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
