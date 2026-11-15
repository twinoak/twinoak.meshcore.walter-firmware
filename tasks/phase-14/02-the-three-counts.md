# 02 — The three counts

**Depends on:** 01
**Spec:** `components/agent-core/include/agent/Envelopes.h` (`HeartbeatFlood`) and `src/Envelopes.cpp` (where `flood` is written), `components/agent-core/src/AgentLoop.cpp` (how the library fills it: the scope from the mesh, `floodUnscopedRetry()`, `floodCounts()`), `components/agent/identity.cpp` (`Identity::fillHeartbeat`, this board's own writer), `components/agent/include/walter_agent/agent_backend.h` (`agent_state`); meshmaster.dk's `AgentFlood` in `src/MeshMaster.Contracts/Envelopes.cs`, read only
**Size:** one session.
**Model:** claude-opus-5

## Goal

With task 01 the library applies its rule on this board: the first flood of a transaction goes
out in the radio's scope and the second without it. Whether that second try still finds
repeaters the first one misses is a number, and the number decides the day the try is switched
off (meshmaster.dk D-47). A companion exit node sends it in every heartbeat —
`flood: { scope, unscopedRetry, answeredScoped, answeredUnscoped, unanswered }` — and
meshmaster.dk prints *Floods scoped dk; one unscoped retry; answered 5 scoped, 0 unscoped, 56 not
at all*.

This board writes its own heartbeat (`Identity::fillHeartbeat`) and leaves `flood` out. And the
library's counts are *since boot*, on a board that boots at every wake: sent as they are they
would read 0, 0, 0, with a 1 now and then.

## Deliverables

1. **The `flood` object in the heartbeat**, exactly when the library would write one:
   `floodScope()` is not null. `scope` and `unscopedRetry` as `AgentLoop::buildHeartbeat` sets
   them. When `floodScope()` is null — a radio of link 1.2 — the heartbeat is byte for byte what
   it is today.
2. **Totals that outlive a wake.** `answeredScoped`, `answeredUnscoped` and `unanswered` are
   totals, kept where `httpFailures` is kept, and they survive deep sleep, a restart and a power
   cut as it does. A wake adds what the library counted in that wake, once. Say in the report
   where the addition is made, and why a wake that dies between a transaction and the save loses
   at most that wake's counts and never counts a transaction twice. `FloodPolicy` cannot be
   seeded and is a copy: the totals are this board's, beside it.
3. **One line for a person.** `agent state` prints the scope, the rule and the three totals, in
   the library's own words where it has them (`JobRunner::describeFlood`), with the totals and
   not the wake's counts. `twinoak.report`'s `agent` section has the line because it is that
   command. Nothing else about the report changes.
4. **The saved state grows and loses nothing.** A unit that wakes on this firmware with state
   saved by v0.1.6 keeps every counter it had, and its three totals start at 0.
5. **Room.** Task 01 made it. `tools/heap_at_start.py` over this task's build, and the margin in
   the report; if it is below main's before this pile, stop and say so.
6. **The bench**, as lines in `docs/bench.md` for a person with the unit and a repeater it holds
   no path for: what `rak scope` says; a login that floods, and what the job's line or
   `rak mesh status` says of its first try and its second; the heartbeat's `flood` on
   meshmaster.dk afterwards. Not run by this session.
7. **The words.** `docs/console.md` (`agent state`), wherever the heartbeat's members are listed
   for a reader, and a line in `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] A radio of 1.2: the heartbeat's bytes are those of the commit before this task (test).
- [ ] A radio of 1.3 whose scope is `dk`: the heartbeat has `flood` with `scope` `dk` and `unscopedRetry` true, where the library writes the member and in its order; held against the copied contract fixture that has a `flood` object, named in the report, or against the library's own serializer if there is none (test).
- [ ] A radio of 1.3 with no default scope: `scope` is `*` and `unscopedRetry` is false (test).
- [ ] Four wakes of a scripted unit: a flooded login answered in the scope in the first, one answered only without it in the second, one never answered in the third. The fourth wake's heartbeat says 1, 1, 1 (test).
- [ ] A wake that restarts after its transaction and before its save: the next heartbeat's totals are what they were before that wake, or hold it once. Which, and why, in the report (test).
- [ ] State saved by v0.1.6 is read: every counter it held is kept, and the totals are 0 (test, with a blob of that shape).
- [ ] `agent state` shows the line, and a report's `agent` section carries it (test).
- [ ] The heap margin is not below what it was before this pile. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

meshmaster.dk: its page reads the object already. A setting that turns the retry off. The radio's
half. Anything under `spec/` or `components/agent-core/`. The board: no session opens a port. A
release: Thomas presses the button.
