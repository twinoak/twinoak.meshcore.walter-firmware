# 05 — agent-core with a local door

**Depends on:** 01; **the exit-node firmware's pile 1.10 tasks 40 and 41 merged** into its `master`.
**First:** in `twinoak.meshmaster.exitnode-firmware` (read only), find the commit of `master` where `tasks/phase-1.10/reports/40.md` and `41.md` exist. If either is missing, write `logs/STOPPED-05.md` saying which, change nothing else, and end.
**Spec:** `tools/sync-agent-core.ps1` (`-Ref <commit>`, the two manifests, `-Verify`), `components/agent-core/` (the copy) and `host_test/agent-core/`; the exit-node firmware's reports 40 and 41, its `lib/agent-core/include/agent/IMesh.h` (the new defaulted virtual) and `docs/library.md`; `components/agent/include/walter_agent/link_mesh.h` and `link_mesh.cpp` (`job_.local`, `beginJob`, `endJob`); `tasks/phase-4/` (how this repository took agent-core v0.3.0, and what its sync task checked); meshmaster.dk's `src/MeshMaster.Core/Transport/AgentProtocol.cs` (`DecideAsync`, *Not at `admin`, yet*: read only; its pile 4.27 drops that exception for a Walter that has this task).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The exit-node firmware's 40 and 41 gave agent-core two things a unit needs for its own repeater: a
job the board answers locally neither waits for nor spends the airtime budget, and it logs in with no
material at any class. This task brings them into the Walter and wires them to the local door.

The two cases that changed on 7 November:

- **The config read.** A `config.read` of the unit's own repeater took about 40 s per budget slot,
  and reset the Walter (01 ends the reset). It should take seconds.
- **The admin job.** An admin job of that repeater was refused for want of a stored admin password
  that nothing reads. It should need none.

## Deliverables

1. **The sync.** `tools/sync-agent-core.ps1 -Ref <that commit>`; both manifests written; `-Verify`
   green. The PR names the commit and the two tasks. Whatever else came with the sync (38, 39) is
   listed with one line each on what it changes here.
2. **`LinkMesh` says a job is local**, implementing the new virtual from `job_.local`: true from a
   `beginJob` for the unit's own key to its `endJob`, never otherwise.
3. **Tests** through the real runner and the real `LinkMesh` over the fake radio:
   - a 34-line local `config.read` at a budget of 2 a minute finishes with no budget wait, in
     simulated seconds;
   - an `admin`-class local job with no password in the claim runs to its end;
   - a job for another repeater is paced and refused as before;
   - 01's 2 s test still passes.
4. **What meshmaster.dk is told.**
   - Read how meshmaster.dk will know a unit no longer needs the admin password for its own
     repeater: the agent version this release reports, or a field the heartbeat already carries.
   - Make sure the version string it will read is this firmware's (`0.1.2…`).
   - Its pile 4.27 decides on its side. Say in the report exactly what it can rely on.
5. **Docs:** `docs/console.md` or the agent's doc; the local door's description now says a local job
   needs no password and no airtime.

## Acceptance criteria

- [ ] agent-core synced at the commit with 40 and 41, both manifests verified; the PR lists what came with it.
- [ ] `LinkMesh` says a job is local; the local config read takes simulated seconds, and the local admin job needs no password (tests).
- [ ] What meshmaster.dk can rely on is written in the report; `tools/verify.ps1` green.

## Out of scope

meshmaster.dk itself (its pile 4.27).
