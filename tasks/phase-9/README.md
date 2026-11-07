# Phase 9 — the first unit's weekend (v0.1.2)

Written on the evening of 7 November. With v0.1.1 the first unit did what v0.1.0 could not:

- its radio got repeater v0.1.1 over SWD as its golden image;
- four HTTPS connections in a row went through;
- a telemetry job of its own repeater went through the local door.

Then a `config.read` of that repeater reset the Walter: `task_wdt … IDLE0 … CPU 0: main`. A wait
inside agent-core spun instead of yielding, the same fault p8-01 fixed in the flasher, in another
loop. The weekend's other findings ride along. They are all in
[`../phase-8/FOLLOWUPS.md`](../phase-8/FOLLOWUPS.md): the review of 7 November (S1–S4), and the bench
items below it. Each task names the items it ticks.

**Link 1.2** rides along too (06). The unit's repeater could not be read for its status or its
neighbours through the local door, so it stayed *stale* on meshmaster.dk. The specification is in
`spec/LINK-PROTOCOL.md` §5.9, copied here on 7 November.

**Prerequisites:**

- **The exit-node firmware's pile 1.10, tasks 40 and 41**, merged into its `master`. They change
  `lib/agent-core`, which this repository carries as a hash-checked copy
  (`tools/sync-agent-core.ps1`):
  - 40: a job the board answers locally neither waits for nor spends the airtime budget;
  - 41: such a job logs in with no material at every class.

  Task 05 syncs at that commit and stops (`logs/STOPPED-05.md`) if they have not landed. **Start that
  pile first.**
- Nothing else: phase 8 is merged and v0.1.1 is published.

**Not on the board.** The first unit is on Thomas's bench, at Walter 0.1.1 and repeater 0.1.1
(golden). No session opens its ports; each task adds its bench lines to `docs/bench.md`.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-delay-that-yields](01-a-delay-that-yields.md) | — | `EspClock::delayMs` never spins for 1 ms or more; every busy loop of this firmware found and named; the test that would have caught the config read |
| 02 | [the-sixth-connection](02-the-sixth-connection.md) | — | S2 and S3 of the review fixed; `SocketRotation`'s counters on the console; the library's current `socketConfig`; what the stall after five good connections was, or what the trace build must show |
| 03 | [a-silent-radio-is-not-a-scanner](03-a-silent-radio-is-not-a-scanner.md) | — | S1: a scanner or an empty slot is not a dead radio again, and the refusals it lifted are back |
| 04 | [what-the-bench-could-not-see](04-what-the-bench-could-not-see.md) | — | a first install's journal names its image; the clock check's message tells the truth; `agent id` and `agent jobs` show the local door and a job whose key is not this radio's; a pulled cable ends a wake `agent wake` had raised; `docs/flashing.md`'s times and the recovery that changed a radio's key |
| 05 | [agent-core-with-a-local-door](05-agent-core-with-a-local-door.md) | 01; the exit-node firmware's 40 and 41 | `lib/agent-core` synced at that commit; `LinkMesh` says a job is local; a local config read at 2 a minute takes seconds, and an admin job of the unit's own repeater needs no password |
| 06 | [the-local-request](06-the-local-request.md) | 05 | link 1.2: the local door answers status, neighbours, owner info and the access list from `LOCAL_REQUEST` when the radio has it, and as before when it does not; `rak request`; what meshmaster.dk can rely on |
| 07 | [the-release-0-1-2](07-the-release-0-1-2.md) | 01–06 | `v0.1.2` published as *Latest* and verified; the notes say what a unit at 0.1.1 gains and that it updates over LTE |

```powershell
.\tasks\run-all.ps1 -Phase '9'
```

Run the exit-node firmware's pile 1.10 first or alongside; meshmaster.dk's pile 4.27 and the repeater
firmware's pile 3.9 do not wait for this one. 06 is useful only with the repeater's pile 3.9 (task 02)
released too, and is harmless without it.

## Models

- **Opus**: all seven.

## Afterwards (Thomas)

- **Getting v0.1.1 → v0.1.2 onto the unit.** Over LTE, this is the first Walter update that goes
  that way: *Offer a release* on the unit's page, then an apply. Or flash it by cable, as before.
- **Then the bench:**
  - turn the scheduled config read of the unit's repeater back on;
  - run *Read config* through the unit: seconds, no reset;
  - read `agent id`'s new local-door lines.
- **Link 1.2, once the radio is at repeater v0.1.2 too** (its pile 3.9, offered over LTE like the
  Walter's):
  - `rak hello`: minor 2, and the bit `0x80` in the caps;
  - `rak request 01`: the status, field by name;
  - on meshmaster.dk, the repeater leaves *stale* at its next status read, and its graphs fill.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
