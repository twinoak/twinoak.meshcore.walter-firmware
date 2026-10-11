# Phase 2 — images and updates

Five tasks that turn "the Walter can flash the radio" into "the radio cannot be
bricked from afar": nothing is flashed that is not signed and meant for this
radio; a known-good image is always on hand; a new image has to prove itself or
is replaced; the Walter updates itself the same way; and when the radio goes
quiet, something sensible happens, slowly.

Everything here is logic about failure, so everything here is tested by making
it fail: a flash store that loses power at every write, a radio that never
comes back after an update, a signature with one bit changed, a Walter that
resets between two steps. **A test in which nothing goes wrong proves very
little in this phase.**

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [manifests-and-signatures](01-manifests-and-signatures.md) | phase 1 | Ed25519, the manifest, the descriptor: the six checks of the specification, against its fixtures |
| 02 | [the-image-store](02-the-image-store.md) | 01 | golden, candidate and scanner images per radio, in flash, with records that are whole or absent; a way to load one over USB |
| 03 | [the-radio-update](03-the-radio-update.md) | 02 | flash, health check, commit or roll back — journalled, resumable, and never retried with an image that failed |
| 04 | [the-self-update](04-the-self-update.md) | 02 | the Walter's own A/B update, confirmed only when it can reach the radio and the server; never both updates in one wake |
| 05 | [the-supervisor](05-the-supervisor.md) | 03 | the radio's heartbeat and the ladder: ask, reset, power-cycle, reflash the golden image, stop — with waits and daily caps |

01 → 02 → 03 → 04 → 05.

```powershell
.\tasks\run-all.ps1 -Phase 2
```

## Models

- **Fable** — 01, 03, 04. What is trusted, and what happens when an update
  goes wrong.
- **Opus** — 02, 05.

## The exit criterion

On the bench: load a repeater image and a deliberately broken one over USB;
update to the good one (it becomes golden); update to the broken one (it fails
its health check, the golden one comes back, and the broken one is refused a
second time); pull the Walter's power in the middle of each and watch it finish
or roll back by itself; hold the RAK in reset with a jumper and watch the
supervisor climb its ladder and stop. Thomas's.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
