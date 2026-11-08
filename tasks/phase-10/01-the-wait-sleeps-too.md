# 01 — The wait sleeps too

**Depends on:** **the exit-node firmware's pile 1.10 tasks 42 and 43 merged** into its `master`.
**First:** in `twinoak.meshmaster.exitnode-firmware` (read only), find the commit of `master` where `tasks/phase-1.10/reports/42.md` and `43.md` exist. If either is missing, write `logs/STOPPED-01.md` saying which, change nothing else, and end.
**Spec:** `tools/sync-agent-core.ps1` (`-Ref`, the two manifests, `-Verify`), `components/agent-core/` and `host_test/agent-core/`; the exit-node firmware's reports 42, 43 (and 44, which comes with the sync); `components/agent/agent_backend.cpp` (`saveState`, the restore around L180–200, `kStateKey`, the `agent state` lines ~L505); `components/interfaces/include/interfaces/clock.h` (*restarts at zero after a deep sleep*; *assume the worst*); `components/core/cycle/wake_reason.h` and `wake_cycle.cpp` (why this boot is awake, and the interval the last sleep asked for); `components/agent/include/walter_agent/link_mesh.h` (the local door, `setOwnKey`, 40's `answersLocally`); `tasks/phase-9/reports/05.md` (how the last sync was done); `tasks/phase-9/FOLLOWUPS.md`, *The wake still prices a local job as air*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

8 November: the unit woke every 15 minutes all night, and meshmaster.dk heard from it on about every
sixth wake. A call that fails sets a hold of 30 s or more. The library saves what is left of it,
puts it back after the sleep, and until the exit-node firmware's 42 the sleep counted for nothing.
So a 30 s hold was spent five seconds at a time across six short wakes, each of which sent nothing:

```
agent state: restored across the last sleep (1 restored, 0 saved, 0 refused)
  version 1; backoff 30000 ms, 1 consecutive failure(s) for 1436464 ms, hold 0 ms left
  counters: 31 heartbeat(s), 31 http failure(s), ...
```

At the usual 60-minute interval that is six hours of silence after one failed call. It may well be
what Thomas saw on 6 November, when the unit seemed never to wake.

42 lets `restore()` take the time the board was away off the hold. This task syncs it, and tells
the library how long that was.

## Deliverables

1. **The sync**, as p9-05 did it: `tools/sync-agent-core.ps1 -Ref <that commit>`, both manifests,
   `-Verify` green. The PR names the commit and lists 42, 43 and 44 with one line each on what they
   change here.
2. **The time away, measured deterministically.**
   - The rule: **never more than really passed.** The time away is what this Walter knows was slept.
     A timer wake has slept the interval the last sleep asked for; take off the timer's own error
     (name the figure and where it comes from). Any other wake (the radio's line, a power-on, a crash,
     a person's cable) passes 0 unless this firmware already has a measure of that sleep it can
     defend. Say which in the report.
   - `restore(state, awayMs)` is called with it. A state restored at a power-on keeps today's
     behaviour.
3. **43's virtual in `LinkMesh`**: true for the unit's own key once it is known, false otherwise, and
   in agreement with `answersLocally()`. A local `config.read` is priced at its reply waits (test).
4. **The console.** `agent state` says how much of the hold the sleep took off (*hold 0 ms left;
   30 s owed before the sleep, 900 s slept*).
5. **Tests** with the fake clock and the real `AgentBackend`:
   - a failed call, a 900 s timer sleep: the next wake heartbeats;
   - the same at a 60-minute interval;
   - a wake on the radio's line soon after: the rest of the hold is still owed, as before;
   - a Retry-After of 1 800 s and a 900 s sleep: the next wake still holds, for the 900 s left.
6. **`tasks/phase-9/FOLLOWUPS.md`**: *The wake still prices a local job as air* ticked, naming this
   task.

## Acceptance criteria

- [ ] agent-core synced at the commit with 42 and 43, both manifests verified; the PR lists what came with it.
- [ ] A timer sleep takes its own length off the hold, never more than was slept; other wakes as before (tests).
- [ ] A local job is priced as local; `agent state` shows the time away; `tools/verify.ps1` green.

## Out of scope

Why so many calls fail (pile 9's 02 and its trace build). The journal's words (02).
