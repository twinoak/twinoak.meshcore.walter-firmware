# Phase 14 — the scope a unit's logins go out in

Written on 14 November.

**What happened.** Thomas asked where `region default dk` is set, because the unit at Bellevue
seemed to advert without a scope. Its adverts are scoped. What is not is every flood the unit's
radio sends for this board: a login or a request to a repeater it holds no path for leaves with no
transport code, whatever the radio's own default scope is. The exit nodes that are companions
flood their region first and keep one try without it (meshmaster.dk D-47). On the day their
heartbeats counted nine floods answered in the scope and five answered only by the try without.

**What Thomas decided.** Jobs sent through a Walter carry the default scope too.

**What it takes.** The box link had no word for a scope, so it is version 1.3 now
(`spec/LINK-PROTOCOL.md` §6.5, copied here on 14 November with `link-v1.3.json`). In a session of
1.3 the radio floods the client's packets in its default scope, as it does its own adverts; one
request, `FLOOD_SCOPE`, reads the scope's name and asks for one send without it; and a send's
answer says whether it went out scoped. The rule — the scope first, then one try without it — is
already in the agent library this board runs. It has had nobody to tell.

**What this pile builds.** The Walter's half: it speaks 1.3 and gives the library its scope (01),
and the heartbeat carries the three counts, kept over sleep (02). The radio's half is the repeater
firmware's pile 3.13. Either may be released first: against a radio of 1.2 this board says nothing
about scope, as today, and a radio of 1.3 floods unscoped for a Walter of 1.2, as today.

**And one carried from phase 13**, tagged for this pile: the release's pull request says no check
runs on it, and one does (03).

**And one from the same afternoon.** Thomas watched the radio's update to 0.1.5 say nothing for
eighteen minutes and asked for a longer timeout. The unit's journal says a flash takes fifteen
minutes; the lease it asks for is fifteen minutes, and it cannot ask again while it flashes. It
ended 8 seconds before the lease did (04).

**And one from the morning after.** v0.1.6 was applied to the unit on 15 November and rolled
itself back: *the backend was not reached inside the trial window*. A wait left by the wake
before it kept the trial's one heartbeat from being sent. v0.1.6 is on the unit's bad list, and
with it the diagnostics of pile 13 (05). **05 runs first, alone**, so that the next release can
be installed at all:

```powershell
.\tasks\run-all.ps1 -Phase '14' -From 05 -To 05
```

**And the same root, where it was first met.** On 13 November *Wake it up* reached the unit's
radio and nothing came of it (meshmaster.dk job 28399). A wait is put back whole at every boot
that is not a timer wake, so a wake a person asks for sends nothing whenever the wake before it
ended in a failed call. Phase 13's first follow-up named the cure: the radio never sleeps, and
its uptime before the sleep and after it is the time away (06). Written on 15 November, after
05 had been started and stopped. It runs with the rest, last.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-scope-on-the-link](01-the-scope-on-the-link.md) | — | link 1.3 spoken: `FLOOD_SCOPE`, its capability, a send's `scoped`; the mesh tells the library the radio's default scope and sends the one flood without it; `rak scope`; room made in the heap at start before any of it |
| 02 | [the-three-counts](02-the-three-counts.md) | 01 | the heartbeat's `flood` object, with totals that survive sleep, a restart and a power cut; `agent state` and the report say the same line |
| 03 | [the-notes-say-what-is-true](03-the-notes-say-what-is-true.md) | — | the notes' pull request and `docs/release.md` say what GitHub does with its check: a run held for approval, to approve or to merge past |
| 04 | [an-apply-keeps-its-lease](04-an-apply-keeps-its-lease.md) | — | a radio's update extends its lease before each flash and posts its journal as it is written; what a rollback longer than the wake's bound does, shown and settled |
| 05 | [a-trial-the-last-wake-cannot-sink](05-a-trial-the-last-wake-cannot-sink.md) | — | a trial boot owes no wait from the wake before it; its heartbeat is tried again while the window lasts; a rollback's result goes in the first wake that can; the journal says how many heartbeats were tried |
| 06 | [a-wake-a-person-asked-for](06-a-wake-a-person-asked-for.md) | 05 | the time away is measured by the radio's uptime for every kind of wake; a wake a person asked for owes no wait; `agent state` says where the figure came from |

```powershell
.\tasks\run-all.ps1 -Phase '14'
```

## Models

- **Opus**: 01, 02, 03, 04, 05, 06.

## Afterwards (Thomas)

- **After 05 alone: release** with the button (v0.1.7: phase 13's lines and 05's), merge its
  notes pull request, then offer and apply the Walter's own firmware to the unit from
  meshmaster.dk. The Diagnostics tab of its exit node appears after its next check-in.
- **After the rest** (01 to 04, and 06): release again, offer and apply.
- **Then a wake**, with that release on the unit: press *Wake it up*, and *Fetch now* on the
  Diagnostics tab. `agent state` in the report says where the time away came from and whether
  a wait was set aside.
- **The radio's half**: the repeater firmware's pile 3.13, released, offered and applied to the
  unit's radio. Until both are on the unit nothing changes on the air.
- **Then look** at the unit's exit node on meshmaster.dk: *Floods scoped dk; one unscoped retry;
  answered …*, as a companion exit node's page says it.
- **The bench, when there is time:** task 02's lines in `docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
