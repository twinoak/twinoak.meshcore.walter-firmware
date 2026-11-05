# Phase 6 — the bench

The first pile whose sessions touch the board. Everything phases 0–5 left under *Not verified
here* is in `docs/bench.md`, one walk from an empty desk; this pile walks the parts of it a session
can walk with nobody in the room, through one tool that decides what a session may do to the rig and
refuses everything else before it opens a port — the way the repeater firmware's `tools/bench/`
(its pile 3.3) already does for the radio's USB. What needs a scope, a meter, a hand on the power or
a person's judgement stays a person's, and the tool prints those lines instead of guessing at them.

The goal of the pile, in one sentence: **a Walter on the production chain, flashed from this
repository, that has given a factory radio its base and its first image, speaks to it over the link,
and checks in with meshmaster.dk** — after which a person can register its repeater from its key
and update the radio over LTE (the hardware repository's `software/FIRST-UNIT.md`, the end of the
walk).

**Prerequisites:**

- phases 4 and 5 merged (the agent; the radio's base);
- the repeater firmware's pile 3.4 task 01 merged (its release carries the base) and a release of it
  built locally (`docs/release.md` there) — for task 03;
- meshmaster.dk's pile 4.16 deployed (a unit registered before its repeater) — for task 04;
- **the rig, on this machine, as `bench.local.json` says** (task 01 writes its shape): the unit
  assembled, powered from the bench supply on connector B's J7 with SW1 on — **not the battery** until
  U3 on the Walter adapter is reworked (the hardware repository's `hardware/pcb/BRING-UP.md` step 6:
  on the boards of 2026-10-10 the Walter's USB 5 V reaches the battery rail) — the
  Walter's USB-C to this machine, an antenna on the LTE port, and on the LoRa port **a dummy load**
  for any task that puts a repeater image on the radio (task 03: the repeater firmware's rule, which
  this tool keeps — a repeater on an antenna on a desk is a stranger on the Danish mesh);
- for task 04: a SIM in the Walter, and the unit's settings typed by Thomas over the cable once —
  `lte.apn`, `lte.pin`, `server.url`, `server.token` (the token from meshmaster.dk's *New exit node*,
  transport `native`, no repeater). **No session ever types, reads, logs or is given a secret**; the
  tool sees only *set* / *unset*.

Phase 3's handover (`tasks/phase-3/FOLLOWUPS.md`, *For the bench pile*) said a session must not do
a ⚠ item by itself. Here it does one only when Thomas has said so for this radio in
`bench.local.json` (`"radio": "disposable"`, and `"rf": "dummy-load"` for a repeater image) — the
answer the repeater firmware's bench gave with `"identity": "disposable"`. The kill line stays a
person's in every configuration.

With no `bench.local.json`, or no Walter on the port it names, tasks 02–04 **stop** (`RUNBOOK.md`
§5: a step needs a human) — they do not pretend. Task 01 (the tool) is finished on the host either
way.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [a-bench-a-session-may-use](01-a-bench-a-session-may-use.md) | phases 4, 5 | `tools/bench/`: the Walter found by its USB serial number, flashed without losing its settings, its console driven line by line, every ⚠ refused unless `bench.local.json` allows it; the repeater firmware's lock per radio shared |
| 02 | [the-first-walk](02-the-first-walk.md) | 01 | `docs/bench.md` §1, the console half of §2, §4 on silicon: DPIDR, `INFO.VARIANT`, APPROTECT, the SWD clock's ceiling through the D-subs, measured |
| 03 | [the-radio-gets-its-base](03-the-radio-gets-its-base.md) | 02; repeater 3.4 task 01 | §4a, §3, §5, §8, §9 on silicon: a factory radio provisioned, its first image, the link, the deliberate bad image rolled back |
| 04 | [the-unit-checks-in](04-the-unit-checks-in.md) | 03; meshmaster.dk 4.16 deployed | §12–§14 and *The agent*'s lines: attach, time, TLS, a heartbeat with the radio's key, a wake that ends in sleep |

```powershell
.\tasks\run-all.ps1 -Phase 6
```

One driver at a time **on this rig**: the repeater firmware's bench and this one take the same lock
per radio (task 01), so the two repositories' runs never drive one unit at once — but do not start
both drivers on the same evening anyway; a lock held is a stopped task.

## Models

- **Fable** — 01, 03. What a session may do to hardware; erasing and flashing a radio.
- **Opus** — 02, 04.

## The exit criterion

On the rig, by the sessions: every line of `docs/bench.md` §1, §3, §4, §4a, §5, §8, §9, §12–§14 that a
session can walk is ticked *(bench, date, commit)*, or written into its task's follow-ups as what it
saw; every number those lines measure is in *What to bring back*; the radio runs a repeater image the
Walter put there and answers `HELLO`; the unit's heartbeat reached meshmaster.dk with the radio's key.

Then Thomas's: register the repeater from the key on the exit node's page, and the update over LTE —
`software/FIRST-UNIT.md` in the hardware repository, its last part.

## Not in this phase

The kill line (§6), the supervisor's ladder (§11), sleep on a scope (*Sleep* ★) and a day (*A day*): each
needs a hand on the power, a jumper or a scope, and stays a person's walk. A firmware update of the
Walter itself over LTE (§10 with a server) — after the first release key exists.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
What a walk saw that differs goes there too, tagged with the task that wrote the line.
