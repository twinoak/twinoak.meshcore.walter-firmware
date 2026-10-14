# 02 — The mesh over the link

**Depends on:** 01.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §6 (all of it: the job and its path, the five sends, the reply record, `MESH_COUNTERS`), §7 (`OBS_PULL`), §5.7 (`CLI` — the local door to the unit's own repeater), §5.8 (`LPP_GET`), §9, §10; [`spec/fixtures/link/link-v1.json`](../../spec/fixtures/link/link-v1.json) (every §6 message as bytes); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-4, S-12; the exit-node firmware at the pinned commit: `docs/library.md` (*IMesh*, method by method, and *the rails the consumer inherits*), `lib/agent-core/include/agent/IMesh.h`, `test/support/FakeMesh.*` (the simulated stock repeater) and `test/test_runner/` (the scenarios this task re-runs); this repository's `components/core/link/` (the client of phase 1) and `host_test/sim/scripted_radio.*`; the repeater firmware's `docs/console.md` and its phase-1 task 05 report (what the link's `CLI` may do with console privileges, and what it refuses).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

`IMesh` is what the agent's runner believes a radio is: begin a job with a
peer, send one of five things, wait for one reply, read counters. On the
Heltec that is a class in the same process. Here each call is one request on
the UART and the reply is an event — and the password of a login crosses the
link once and is then nowhere. The proof that nothing was lost in the
crossing is the library's own runner scenarios, run again with a simulated
RAK between the runner and the simulated repeater.

## Deliverables

- **`components/agent/LinkMesh : IMesh`**, over the link client, method by
  method as `docs/library.md` contracts them (the report has the table,
  from pile 1.5's task 15):
  - `beginJob(key, path)` → `JOB_BEGIN`; `endJob` → `JOB_END`;
    `setDirectPath`/`forgetPath` → `PATH_SET`; `hasPath`/`pathHopCount`/
    `describePath` → `PATH_GET` (cached between sends; refreshed after a
    reply, because a returned path changes it, §6.3);
  - the five sends → `SEND_LOGIN`, `SEND_REQUEST`, `SEND_CLI`,
    `SEND_DISCOVERY`, `SEND_TRACE`, each with the `timeout_ms` the runner's
    wait would have been; `SendResult` from `send`; the `tag` from the
    response; `sendCli`'s timestamp is `sender_ts` — the Walter supplies
    none, ever (§6.2);
  - `awaitReply`/`awaitText` → the link's event `EV_MESH_REPLY`, with the
    Walter's own timer of `timeout_ms` plus a margin and a `REPLY_GET` when
    the event did not come (§6.3); the record mapped into the library's
    `MeshReply` (state 3 → `Response`, 4 → `Text`, 5 → `Trace`, 2 →
    none); `reply()` the last record;
  - `counters()`/`overheard()` → `MESH_COUNTERS` (`client_tx` is what the
    runner's transaction count is held against — assert it equals the
    number of sends the runner made, exactly, in every test);
  - `loop()` → the link client's pump (events, keep-alive);
  - `setRing` → the observation ring is filled from `OBS_PULL` by 04; here
    the hook.
- **The password's path.** `sendLogin(password)` copies the bytes into the
  request frame, sends, and **wipes the frame buffer and the copy** before
  returning; the link client's transmit buffer is wiped after the frame is
  on the wire (phase 1's client — extend it with a `sendSensitive` that does
  so, and a test that reads the buffer back). A `sendCli` with
  `sensitive` sets the flag bit and takes the same path. Nothing logs the
  text of either; the link's frame log (if phase 1 has one) prints the
  payload of these two as `*` per byte, as the agent firmware's does.
- **The local door.** When `beginJob` is called with **this unit's own
  repeater key** (04 supplies it from `IDENTITY`), `LinkMesh` enters *local*
  mode for the job: `sendLogin` succeeds without a packet (the link has
  console privileges; return `Direct`, a synthetic reply record of a
  successful admin login — build it from the fixture of a login reply at
  admin, cited); `sendCli(text)` → the link's `CLI` request (§5.7) and the
  text reply; `sendRequest(telemetry)` → `LPP_GET` with the permission mask
  of an admin and the bytes as a response; `sendRequest(status)`,
  `sendRequest(neighbours)`, `sendDiscovery`, `sendTrace` → **check the
  pinned repeater CLI** (`CommonCLI.cpp` at the MeshCore pin the repeater
  firmware names) for a line that yields the same information (`neighbors`
  may exist); where there is none, return `Failed` so the runner answers
  `unsupported` — and the report lists each, as the input to a link 1.1
  request. In local mode `MESH_COUNTERS.client_tx` must not move (a test).
- **`host_test/sim/SimRak`**: the RAK's side of §6 as a simulator — it
  decodes the link frames (phase 1's codec), enforces §6's rules (one peer,
  one exchange, `ERR_BUSY`, `ERR_STATE`, the 120 s job timeout, one packet
  per send, two-byte flood hashes), and behind it drives the library's
  **`FakeMesh`** (the simulated stock repeater) for what the peer answers.
  Faults: a lost event (the Walter must `REPLY_GET`), a lost request (the
  link's own retry), a `boot_id` change mid-job (everything forgotten).
- **The scenarios, again.** Every test of the library's `test/test_runner/`
  that drives `IMesh` runs a second time with `LinkMesh` + `SimRak` +
  `FakeMesh` in place of `FakeMesh` alone — same assertions, now through the
  framing. Plus: the lost event; the lost request; the job timeout; a login
  password read back as zeros from every buffer it crossed; local mode for
  each core type; `client_tx` exact.

## Acceptance criteria

- [ ] Every `IMesh` method maps to a §6 (or §5.7/§5.8) request, in a table in the report; any that cannot is a named specification gap.
- [ ] The library's runner scenarios pass through `LinkMesh` + `SimRak` unchanged in their assertions; the report gives the count.
- [ ] A password's bytes are zero in every buffer after `sendLogin` and after a sensitive `sendCli` (tests read them back); nothing logs them (the log capture is searched).
- [ ] `client_tx` equals the runner's sends in every scenario; local mode moves it by nothing.
- [ ] The local-mode table (which core types the unit serves for its own repeater over the UART, and which it answers `unsupported`) is in the report and in `docs/ARCHITECTURE.md` §13.
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "15. The agent" (06 orders it): with a neighbouring
repeater on the bench, a `status` through `rak mesh status <key>` (a console
command this task adds for the bench: begin a job, send, print the reply) —
one packet on the air, one reply, `client_tx` up by one.

## Out of scope

The uplink (03), the identity and the ring (04), running jobs (05).
