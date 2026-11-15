# 01 — The scope on the link

**Depends on:** —
**Spec:** `spec/LINK-PROTOCOL.md` 1.3 (the note at its top; §4; §5.1's *Versions*, *The session's version* and the capability bits; §6.2's `scoped`; §6.5) and `spec/fixtures/link/link-v1.3.json`; `components/agent-core/include/agent/IMesh.h` (`floodScope`, `setFloodMode`) and `agent/FloodScope.h`; `components/agent/include/walter_agent/link_mesh.h`; `tasks/phase-13/FOLLOWUPS.md` (*The heap at start is down to 116 bytes of margin*)
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

A flood this unit sends for the agent — a login or a request to a repeater it holds no path for,
a path discovery — leaves the radio with no transport code. `LinkMesh` inherits
`IMesh::floodScope()`, which answers null; the library reads that as *this mesh says nothing
about scope*, so `FloodPolicy` never asks for anything and the heartbeat has no `flood` object.
On the other side of the UART the radio's mesh client calls plain `sendFlood`. Neither end was
wrong: link 1.2 had no word for a scope.

Link 1.3 has one. This task makes the Walter speak it and gives the library what it has been
waiting for: the name of the scope, and a way to send one flood without it.

## What 1.3 is, for this end

The specification is the page. This is the short of it; where the two differ the specification
is right, and the report says where.

| | |
|---|---|
| `HELLO` | this end asks for minor 3. A radio of 1.3 answers 3 and has `0x100 FLOOD_SCOPE` in its `caps`; a radio of 1.2 answers 2 and has not |
| the floods | in a session of 1.3 the radio sends the client's floods in its default scope **without being asked**, and with no transport code when it holds no default scope. In a session of 1.2, and after a restart until the next `HELLO`, it floods unscoped as it always did |
| `FLOOD_SCOPE`, `0x2C` | request `mode : u8`: 0 ask, 1 scoped, 2 the next send without the scope. Answer `mode : u8` (1 or 2), `scope : str8`: the default scope's name, 1 to 30 bytes, or empty. `mode` 2 is for **one** send; the radio is back at 1 after the next `SEND_*` it executes, and after `JOB_BEGIN` and `JOB_END` |
| a send's answer | ends with `scoped : u8` in a session of 1.3: 1 when the packet went out as a flood carrying the scope's transport code. In an older session the byte is not there |

## Deliverables

1. **Room first.** `tools/heap_at_start.py` over main before anything else, and the margin in the
   report. Phase 13 left 116 bytes and its follow-up says what the next task that adds static
   storage does: it makes room first, in `agent::kKeptScratchBytes` or beside it, and not by
   shaving bytes. This task adds a name of 31 bytes and a little state, and task 02 adds three
   totals. Make the room both need, here, so that the margin after this task is not below
   main's, and say what gave it up and what that costs. Rail R22 is the judge.
2. **The codec at 1.3.** `kProtoMinor` 3. `kFloodScope = 0x2C`, `cap::kFloodScope = 0x100` and
   its line in `capabilityOf`. The request and its answer. `SendResponse` gains `scoped`, with
   *absent* told apart from 0: an answer without the byte is a 1.2 answer and not a malformed
   one. Held against **all four** fixture files, the nineteen cases 1.3 adds among them
   (`host_test/core/link/codec/test_link_fixtures.cpp`, `host_test/fixtures/link_fixture.h`).
3. **The client's request.** `floodScope(mode)` on `lc::Client`, refused without the capability
   as `localRequest` is, before anything is sent.
4. **The mesh says its scope.** `LinkMesh::floodScope()`:
   - the session has no `FLOOD_SCOPE` capability, or settled on less than 1.3: null, as today;
   - it has, and the radio named a scope: that name. Nothing copies the string (`IMesh.h`), so
     it is a member with a named bound;
   - it has, and the radio holds none: `mm::kFloodScopeUnscoped`, the library's `*`.

   The name is asked with `mode` 0 once a session, where the unit's own key is asked (task
   p4-04's `IDENTITY`, before every claim): before the heartbeat is built, because the heartbeat
   is where task 02 puts it. A session whose ask went unanswered says null to the heartbeat and
   asks again before a mesh job begins; a job whose ask fails then, fails as one whose
   `JOB_BEGIN` failed. The radio scopes that job's floods whether or not this end knows the
   name, and a job run without the name would have no try without it.
5. **The mesh sends the one flood without it.** `LinkMesh::setFloodMode(mode)` keeps the mode for
   the next send. Before a `SEND_LOGIN`, `SEND_REQUEST`, `SEND_CLI` or `SEND_DISCOVERY` whose mode
   is `Unscoped`, and only then, it sends `FLOOD_SCOPE` with `mode` 2.
   - Not when the job holds a path: the send goes direct, and a path has no scope. Not for a
     trace. Not for the unit's own repeater: the local door puts nothing on the air. Not when
     `floodScope()` is null or `*`.
   - A `FLOOD_SCOPE` that is refused or unanswered fails the send as `Failed` and nothing is
     transmitted. A try meant to go without the scope that goes out with it is the first packet
     again, and airtime for nothing.
   - Nothing is sent for `Scoped`. It is what the radio does unasked, and `mode` 2 is over after
     one send.
6. **What came back is believed.** The answer's `scoped` is kept beside `LastRecord`'s `flood`.
   When a flood's `scoped` is not what this end asked for, the name is asked again (`mode` 0) and
   `floodScope()` says what the radio says now; a counter in `LinkMeshCounters` counts those.
   Say in the report what the library's own count does with such a send. If it counts it under
   the wrong scope, leave a line under FOLLOWUPS: the library is a copy and is not changed here.
7. **A restarted radio.** A changed `boot_id` forgets the name with everything else
   (`lc::RestartWatcher`). The radio is back at 1.0 until the next `HELLO` and floods unscoped
   until then. Show in a test what this end does between the restart and that `HELLO`, and that
   it tells the library no scope the radio is not using.
8. **`rak scope`** at the console: `FLOOD_SCOPE` with `mode` 0, printing the mode and the name, or
   *none*, or why not (*this radio speaks 1.2*). `rak hello` names the new capability as it names
   the others. `rak mesh status <key>` prints the send's `scoped` beside *flood* or *direct*.
9. **The scripted radio** of the host tests answers as 1.3 and as 1.2, by a switch, so that every
   test below runs against both.
10. **What says 1.2 today.** `this_image.h`'s `kLinkMinor` follows `kProtoMinor`, so this image's
    descriptor says 1.3. Find what else states the minor — the manifest a release builds,
    `docs/ARCHITECTURE.md`'s paragraph on the three fixture files, `docs/console.md` — and make
    each true. A radio's image is chosen by the link's **major** alone (`choose.cpp`); that does
    not change, and a test says so for a 1.3 Walter and a 1.2 radio's image.
11. **The words.** `docs/console.md` (`rak scope`, and the lines that gained a word),
    `docs/ARCHITECTURE.md`, and a line in `release-notes/unreleased/` for this task: *a login
    this unit floods goes out in the radio's default scope, with one try without it, once the
    radio has its 1.3 too*.

## Acceptance criteria

- [ ] The heap margin of this task's build is not below main's; both numbers in the report. Rail R22 green.
- [ ] All four fixture files pass through the codec in both directions (test).
- [ ] Against a scripted radio of 1.2: `floodScope()` is null, no `FLOOD_SCOPE` is ever sent, and the frames of a flooded login with two tries are today's, byte for byte, but for `HELLO`'s minor (test).
- [ ] Against a scripted radio of 1.3 whose default scope is `dk`: `floodScope()` is `dk` before the heartbeat is built. A flooded login's first try is one `SEND_LOGIN`; its second is `FLOOD_SCOPE` 2, then `SEND_LOGIN`; the send after that is one `SEND_*` again (test, by the order of frames).
- [ ] The same job with a path: no `FLOOD_SCOPE` at all (test).
- [ ] The unit's own repeater, through the local door: no `FLOOD_SCOPE`, nothing on the air (test).
- [ ] A radio of 1.3 with no default scope: `floodScope()` is `*`, and no `FLOOD_SCOPE` 2 is sent (test).
- [ ] `FLOOD_SCOPE` 2 unanswered: the send that would have followed is not made, and the library sees `Failed` (test).
- [ ] A flood asked scoped and answered `scoped` 0: the name is asked again and `floodScope()` follows the answer (test).
- [ ] The radio restarts under a job: nothing of the old session's scope reaches the library afterwards (test).
- [ ] `rak scope` in its three states, and on a radio of 1.2 (test).
- [ ] `python spec/tools/specsync.py verify spec` passes; nothing under `spec/` or `components/agent-core/` was edited.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The heartbeat and the counts: task 02. The radio's half: the repeater firmware's pile 3.13. A
setting for `flood.unscoped never`: the library's default, *retry*, holds. The board: no session
opens a port. A release: Thomas presses the button.
