# Phase 3 — the uplink and staying alive

Six tasks that give the Walter a way out — the modem, a verified HTTPS client,
settings a person can provision — and a way to last: sleep, an interval the
battery can pay for, and one wake that does everything in order and then stops.

Almost nothing here can be proven without a SIM and a tower, and the sessions
have neither. So the work is split the way the rest of the repository is:
**the policy is in `core/` and tested against a fake modem that fails in every
way a real one does**; the part that calls the modem library is thin, compiles,
and waits for the bench. A task's report must say plainly which of its claims
are of the first kind and which are of the second.

This pile ends at a seam. The wake cycle talks to a *backend interface* whose
only implementation does nothing; the MeshMaster agent goes behind it when the
server side is decided (`spec/proposals/`). **No task in this phase invents a
server endpoint, a JSON shape or a job.**

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-modem](01-the-modem.md) | phase 2 | the modem behind the uplink interface: SIM, attach, time, signal, power saving; a failure taxonomy with a backoff each; a fake modem |
| 02 | [the-https-client](02-the-https-client.md) | 01 | small requests and ranged, resumable downloads over TLS that is verified — and a test that it cannot be otherwise |
| 03 | [settings-and-console](03-settings-and-console.md) | 01 | a settings store, secrets that are never printed, the whole console in one grammar, a provisioning session |
| 04 | [the-duty-cycle](04-the-duty-cycle.md) | 03 | deep sleep with the pads held; wake by timer, by the radio, by USB; the battery chooses the interval |
| 05 | [the-wake-cycle](05-the-wake-cycle.md) | 02, 04 | one wake, start to finish, each step bounded; the backend seam; readings kept while there is no uplink |
| 06 | [the-rails-and-the-bench](06-the-rails-and-the-bench.md) | 05 | every rail has a test or a grep; `docs/bench.md` is one ordered walk; `docs/field.md`; the README says what this firmware is |

01 → 02 → 03 → 04 → 05 → 06.

```powershell
.\tasks\run-all.ps1 -Phase 3
```

## Models

- **Fable** — 02. Trust on the wire: the place where "verify nothing" is one
  enum away.
- **Opus** — 01, 03, 04, 05, 06.

## The exit criterion

On the bench, with a SIM: the Walter attaches, reads the network's time, sets
the radio's clock from it, fetches a release's manifest and image from GitHub
over verified TLS into the candidate slot, and sleeps; it wakes on its timer,
on a line from the radio, and when USB is plugged in; and a day of that costs
the battery what the policy table said it would. Thomas's.

In the sessions: `tools/verify.ps1` green, and a simulated month of wakes —
with a modem that drops, a radio that goes quiet and a battery that sinks — in
which nothing runs twice, nothing runs unbounded, and every rail holds.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
