# Phase 4 — the agent

Six tasks that put a MeshMaster agent behind the `Backend` seam phase 3 left:
the exit-node firmware's job logic, taken as a library at a pinned commit;
the mesh across the link, the uplink over the modem, the platform on the
Walter; the firmware jobs mapped onto the image store and the update machines
of phase 2; and one wake that is, finally, a check-in with a server.

**Prerequisites — all three:** phase 3 merged; the exit-node firmware's pile
1.5 merged and **tagged** (`twinoak.meshmaster.exitnode-firmware` `v0.2.0`,
the first commit of `lib/agent-core/` a consumer may pin; task 01 pins it);
meshmaster.dk's pile 4.14 merged (the heartbeat fields, the firmware routes,
the four job types with their fixtures, and `docs/dev/unit-protocol.md`,
which task 05 is written against). The bench items also want 4.14 deployed.

Nothing here invents protocol: the agent protocol is meshmaster.dk's and is
spoken by the library; the link is `spec/LINK-PROTOCOL.md`; the job shapes
are `MeshMaster.Contracts`' fixtures. Where a task finds that something it
needs cannot be said with those, it is a specification gap in the report,
not a field invented here.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-library-arrives](01-the-library-arrives.md) | phase 3; exit-node 1.5 | `agent-core` as an ESP-IDF component at a pinned commit, built for the board and the host; its fakes and the contract fixtures brought in by a checked copy; an upstream watch for it |
| 02 | [the-mesh-over-the-link](02-the-mesh-over-the-link.md) | 01 | `LinkMesh : IMesh` — every call one request of LINK-PROTOCOL §6; the library's runner scenarios pass through the UART framing against a simulated RAK |
| 03 | [the-uplink-over-the-modem](03-the-uplink-over-the-modem.md) | 01 | `ModemUplink : IUplink` over phase 3's HTTPS client: the seven calls, the token, the rails |
| 04 | [the-platform-and-the-identity](04-the-platform-and-the-identity.md) | 02, 03 | `WalterPlatform : IPlatform`; the heartbeat's identity from the radio; the job types this unit advertises; observations from `OBS_PULL` into the ring |
| 05 | [the-agent-behind-the-seam](05-the-agent-behind-the-seam.md) | 04; meshmaster.dk 4.14 | `AgentBackend : Backend` = one `AgentLoop::wake()`; the firmware jobs on the image store and the update machines; jobs for its own repeater served over the link; a simulated month as an exit node |
| 06 | [the-rails-and-the-bench-again](06-the-rails-and-the-bench-again.md) | 05 | the agent's rails held over the link; `docs/bench.md` 15–16; `docs/field.md`; the provisioning session with a token; the README says what the unit now is |

01 → 02 → 03 → 04 → 05 → 06 (02 and 03 are independent of each other; the
driver runs them in order anyway).

```powershell
.\tasks\run-all.ps1 -Phase 4
```

## Models

- **Fable** — 02, 03, 05. The mesh across the link (a password crosses it),
  the uplink (a token crosses it), and the agent itself.
- **Opus** — 01, 04, 06.

## The exit criterion

In the sessions: the library's own runner scenarios pass with the mesh across
the simulated link; a simulated month of wakes as an exit node — jobs for
neighbours, jobs for its own repeater, an offer that downloads in ranges with
drops, an apply that rolls back and one that commits, a server that is away
for a week — ends every wake in sleep with no job left leased, no mesh
request beyond what the runner asked, and every rail holding.

On the bench: a provisioned unit registers with meshmaster.dk as *the uplink
of* its repeater, is recognised by its key, sleeps and is shown asleep, runs
a `telemetry` probe on its own repeater over the UART and a `status` on a
neighbour over the air, fetches a test-key release from the server into the
candidate slot in pieces, applies it, and reports `committed`. Thomas's.

## Not in this phase

NoiseScope sessions as a job (the role switch exists; the job type is not
decided). The Walter's historical telemetry ring posted as a batch (the
server has no shape for it; the current reading goes through the `telemetry`
probe). The second radio slot.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
