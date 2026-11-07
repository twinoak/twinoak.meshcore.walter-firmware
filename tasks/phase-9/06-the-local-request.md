# 06 — The local request (link 1.2)

**Depends on:** 05 (the local door as the agent-core sync leaves it).
**Spec:** `spec/LINK-PROTOCOL.md` §5.9 (new in 1.2), §5.1 (the capability bits, *Versions*), §3; `spec/fixtures/link/link-v1.2.json` (the fourteen cases 1.2 adds); `components/core/include/core/link/codec/` (`messages.h`, `wire.h`: the type, the bit, the minor this end sends), `components/core/link/client/` (the typed requests; `lppGet` is the pattern), the host tests that run `link-v1.1.json`; `components/agent/include/walter_agent/link_mesh.h` (the paragraph *the local door*) and `link_mesh.cpp` (`localRequest`, which drops `data` today), `LinkMeshCounters`; agent-core's `src/JobRunner.cpp` (read: `runStatus`, `runNeighbours`, the access-list read of `config.read`, what `data` each sends); the console (`components/core/console/console.cpp`: `rak cli`, `rak status`, and 04's local-door lines in `agent id`), `docs/console.md`, `docs/bench.md`. Read only: meshmaster.dk's `src/MeshMaster.Core/Transport/LocalDoor.cs` and its `tasks/phase-4.27/176-a-unit-reads-its-own-status.md`; the repeater firmware's `tasks/phase-3.9/02-the-local-request.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The local door answers a status, neighbours or owner-info request for the unit's own repeater with
*Failed*, and the access list with silence. The link had no request that yields their bytes. So the
first unit's repeater (41 on meshmaster.dk) never left *stale* and had none of the graphs a status
read feeds.

Link 1.2 adds that request, `LOCAL_REQUEST` (§5.9): the radio answers those four as its own handler
answers an admin over the mesh, with nothing on the air. The repeater firmware's pile 3.9 (task 02)
answers it. This task asks it, and hands the bytes on exactly as a mesh exchange would have.

## Deliverables

1. **The codec and the client at 1.2.**
   - The type `0x0D`, the bit `0x80`; the `HELLO` this end sends says minor 2.
   - The codec held against every case of `link-v1.2.json`, with the 1.0 and 1.1 runs kept.
   - The client's typed `localRequest(req_type, data)`.
2. **The local door uses it.**
   - `localRequest` takes the request's `data` and `len`, and passes them on.
   - When the session has the bit, status (`0x01`), the access list (`0x05`), neighbours (`0x06`) and
     owner info (`0x07`) go as `LOCAL_REQUEST`:
     - `OK`: a held *Response* whose data is the answer, as the access list's and telemetry's are;
     - `ERR_RANGE`: silence, which is what a mesh client hears when the handler answers nothing;
       counted in `local_silent`;
     - anything else: *Failed*, as today.
   - Without the bit (a radio at 1.1), everything as today: status, neighbours and owner info
     *Failed*, the access list silent.
   - Telemetry stays `LPP_GET`. Discovery and trace stay *Failed*: an exchange of a radio with itself
     over zero hops means nothing.
   - *The local door* paragraph in `link_mesh.h` says all of this, dated.
3. **What meshmaster.dk is told.**
   - Its pile 4.27 task 176 decides from what the heartbeat already carries: this firmware's
     `agentVersion`, and the radio's version in `companionFirmware` (`HELLO`'s `fw_version`).
   - Check both are what a 0.1.2 Walter with a 0.1.2 radio will report, and what it reports with a
     0.1.1 radio.
   - Write in the report exactly what meshmaster.dk can rely on. Include the 0.1.2 Walter with a
     0.1.1 radio: it answers *Failed*, as today.
4. **The console.**
   - `agent id`'s local-door lines (from 04) say which of the four requests this radio answers, from
     the session's bit.
   - `rak request <type> [hex data]` sends one `LOCAL_REQUEST` and prints the status in words and the
     answer as hex. For a status (`01`) it also prints the fields by name, in the layout the fixtures
     were written from: a person reads that on the bench.
5. **Tests**, through the real runner and the real `LinkMesh` over the fake radio, which gains
   `LOCAL_REQUEST` from the fixtures:
   - a `status` job of the unit's own repeater, with a 1.2 radio, ends *ok* with the fixture's
     reading;
   - a `neighbours` job pages through the local door: two pages, the second with an offset;
   - a `config.read`'s access list has the fixture's entry;
   - with a 1.1 radio, each of those exactly as today;
   - `ERR_RANGE` reaches the runner as silence;
   - `client_tx` does not move.
6. **Docs**: `docs/console.md` (`rak request`); the agent's doc (the local door); `docs/bench.md`
   (Thomas's lines after both v0.1.2s are on the unit: `rak hello` shows minor 2 and the bit;
   `rak request 01`; the repeater's page on meshmaster.dk after its next status read).

## Acceptance criteria

- [ ] `link-v1.2.json` passes beside 1.0's and 1.1's; this end says 1.2.
- [ ] With a 1.2 radio, the local door answers status, neighbours, owner info and the access list from `LOCAL_REQUEST`; with a 1.1 radio, exactly as before (tests, both).
- [ ] `rak request` and `agent id`'s lines exist and are documented; the report says what meshmaster.dk can rely on.
- [ ] `tools/verify.ps1` green.

## Out of scope

The radio's side (repeater pile 3.9, task 02), meshmaster.dk (pile 4.27, task 176), the release (07).
