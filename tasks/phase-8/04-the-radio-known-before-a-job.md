# 04 — The radio known before a job

**Depends on:** nothing in this phase.
**Spec:** `components/agent/agent_backend.cpp` (`begin`, `refreshIdentity`), `components/agent/identity.cpp` (`Identity::refresh`: *no session: nothing is known and nothing is asked; opening one is `HELLO`, and that is whoever runs the wake's - the supervisor's check at step 2, or `rak hello`*; the job types it offers, about line 269), `components/agent/include/walter_agent/link_mesh.h` and `link_mesh.cpp` (*the local door*: a job for the unit's own repeater, known by `setOwnKey`, sends nothing on the mesh; `job_.local`), `components/core/cycle/` (a timer wake's steps, the supervisor's check that opens the session), `components/core/console/console.cpp` (`agent wake`: which steps it runs and which it skips), agent-core's claim (`components/agent-core`: can a claim name the job types it takes?); `spec/SYSTEM.md` S-1 (one node, one identity), `spec/LINK-PROTOCOL.md` §5.1, §5.6; `../phase-6/FOLLOWUPS.md`, *A unit's own repeater, read through its own Walter*, part (1).
**Size:** one session.
**Model:** claude-opus-5

## Goal

6 November. The radio had just been recovered and the Walter restarted. Thomas paired the unit with
its own repeater on meshmaster.dk (advice that was wrong: pile 4.25 there removes the need) and typed
`agent wake`. The association's discover, addressed to the unit's own repeater, went out **on the
air**: a radio flooding a discover to itself, which nothing will ever answer.

The local door is armed only once `Identity::refresh()` has the radio's key, and that needs a session
(`HELLO`) in this boot. A timer wake opens one in the supervisor's check. The console's `agent wake`
does not, and the boot's banner said so: `agent: no identity read yet (the radio has not been asked
anything in this boot)`. Typing `rak hello` first made the next `agent wake` behave. A firmware that
depends on a person remembering that is wrong.

## Deliverables

1. **No job is claimed before the Walter can tell a local one.**
   - Before the agent claims in any wake, timer or console:
     - with no session, it opens one (`HELLO`);
     - it reads the identity (`IDENTITY`);
     - only then does it claim.
   - Take the supervisor's own step if it fits, rather than a second copy of it.
   - A radio that restarted since (a changed `boot_id`) is asked again, as `Identity::refresh` already
     handles.
2. **A silent radio.**
   - When the radio does not answer, the unit's own job types (`twinoak.fw.*`, `twinoak.power.cycle`)
     are still claimed and run. They do not need the radio's key, and a power cycle is exactly for a
     radio that does not answer.
   - Mesh jobs are not claimed in that wake. If agent-core's claim can name the types it takes, name
     them. If it cannot, read how the claim chooses and take the smallest deterministic rule. Never
     claim a job that would then be refused, which leaves meshmaster.dk a lease nobody honours (pile
     4.25 explains why that costs a job).
   - One console line says which of the two it was, and why.
3. **Tests.**
   - With the fake radio and no prior `HELLO`, an `agent wake` serves a job addressed to the unit's
     own key through the local door: `client_tx` does not move, and a discover is refused locally as
     today.
   - With a silent fake radio, a `twinoak.fw.state` job is claimed and answered, and no mesh job is
     claimed.
   - The console's `agent wake` and a timer wake take the same path.
4. **Docs.**
   - `docs/console.md`: `agent wake` no longer needs a `rak hello` before it; remove any line that
     says it does.
   - `docs/bench.md`, for Thomas: after a power cycle, `lte up` and `agent wake`, with no `rak hello`,
     show the identity read before the claim.

## Acceptance criteria

- [ ] Every wake has the radio's key before it claims, or claims only the unit's own job types; one line says which.
- [ ] Tests for the local job with no prior `HELLO`, for the silent radio, and for the console and timer wakes being one path.
- [ ] Docs updated; part (1) of the `../phase-6/FOLLOWUPS.md` item ticked (parts 2 and 3 are meshmaster.dk's pile 4.25 and a decision of Thomas's).
- [ ] `tools/verify.ps1` green.

## Out of scope

Status, neighbours and owner-info through the local door: there is no request on the link for them yet,
and that is Thomas's decision (repeater firmware pile 3.8, `FOLLOWUPS.md`).
