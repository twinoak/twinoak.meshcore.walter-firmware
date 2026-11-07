# 04 — What the bench could not see

**Depends on:** nothing in this phase.
**Spec:** `../phase-8/FOLLOWUPS.md`: *A first install's `check` entry journals a zero hash*; *the clock check was skipped: … until phase 3's modem*; *The local door's counters are printed nowhere*; *[bench, 7 November] The first install worked on the board* (the time); the review's nits on `docs/bench.md` §9. `components/core/update/radio_update.cpp` (when `check` is journalled, and from what), `components/core/update/journal.h`; the clock check's message (grep it); `components/agent/include/walter_agent/link_mesh.h` (`LinkMeshCounters`: `local_jobs`, `local_silent`, `local_requests`, `local_unserved`; `beginJob`'s `sameKey`), the console's `agent id` and `agent jobs` (`components/core/console/console.cpp`, `components/agent/`); `docs/flashing.md` (*A unit at 0.1.0*: *seven minutes or so*), `docs/bench.md`; the repeater firmware's `tasks/phase-3.8/FOLLOWUPS.md` (*The first radio did not keep its key*, read only).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Small things the first unit showed on 7 November. None of them broke anything, and each one cost a
person time to read.

## Deliverables

1. **The journal names its image.** `rak update status` showed the 6 November run's first entry as
   `check - first install, slot 0, image sha256 0000000000000000...`. Find why: is `check`
   journalled before the image's record is loaded? The first entry of a run carries the image's hash,
   at 0.1.1, with a test.
2. **The clock check says why it was skipped.** *This Walter has no time to give until phase 3's
   modem* has been wrong since phase 3.
   - It was skipped because the uplink was not up in that boot, so there was no network time. Say
     that, and that `lte up` before `rak update` lets it run.
   - Or, if the update can ask the uplink for the time when the uplink is up, do that, and say which
     you chose.
3. **The local door, visible.**
   - `agent id` prints the local door's counters.
   - `agent jobs` says, per job, whether it went through the local door.
   - When a job names a repeater key that is not this radio's, while the key is known, `agent jobs`
     says so, with both keys shortened.

   On 7 November a telemetry job went on the air because meshmaster.dk held the radio's **old** key,
   and only the server's audit log could tell.
4. **A wake a person ended by pulling the cable ends.** `../phase-6/FOLLOWUPS.md`, item *[04] A
   person's `agent wake` keeps the 20-minute bound for the rest of the boot*, which no pile has taken
   until now.
   - On 7 November it happened again. After a bench session with several `agent wake`s, Thomas pulled
     the cable and waited for the drop from about 41 mA to 8 mA. It did not come: the guard's bound
     was 20 minutes from the boot.
   - When a console `agent wake` returns, the bound goes back to what it was, or ends when the cable
     goes plus the grace. Choose one, deterministically, and test it with the fake clock.
   - The console says, when the cable is pulled, until when this wake will stay up and why.
   - Tick the phase-6 item.
5. **`docs/flashing.md` and `docs/bench.md`.**
   - The first install's time: on the bench it was about a quarter of an hour at 125 kHz, not
     *seven minutes or so*. Give a figure from the code's page and poll counts, with the bench's
     measurement as the line to fill.
   - A radio recovered by `.uf2` **may come back with a new key**, as the first one did. How to see
     it: `rak hello`, `agent id`, the unit's companion key on meshmaster.dk. What then: confirm the
     key, re-register the unit's repeater, and re-apply the settings. Link the repeater firmware's
     guide for the cause, which its pile 3.9 is finding.
   - The review's nit: §9's *the run that the watchdog ended* ended on a flash failure.

## Acceptance criteria

- [ ] A run's first journal entry carries the image's hash (test); the clock check's message tells the truth.
- [ ] `agent id` and `agent jobs` show the local door and a key mismatch (tests against the fakes).
- [ ] A pulled cable ends a wake that a console `agent wake` raised, at a time the console names (test).
- [ ] The docs as listed; the `../phase-8/FOLLOWUPS.md` items ticked; `tools/verify.ps1` green.

## Out of scope

The cause of the lost key (the repeater firmware's pile 3.9).
