# 05 — The agent behind the seam

**Depends on:** 04; meshmaster.dk's pile 4.14 merged.
**Spec:** meshmaster.dk at today's `master`: `docs/dev/unit-protocol.md` (pile 4.14, task 127 — **the page this task is written against**: what a unit does at each wake, the four job payloads and results and their fixture names, how to download, what a 200 means, what the server never does), `tests/fixtures/contracts/` for `twinoak.fw.offer`, `twinoak.fw.apply`, `twinoak.fw.state`, `twinoak.power.cycle` (copied by 01's sync — bump the sync to the commit that has them), `spec/EXIT-NODES.md` §5–§7 and §9 *as built* for the unit; the exit-node firmware's `docs/library.md` (*the lifecycle*, `wake()`, `IJobExtension`); this repository: [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §7, §8, §10, §13, `tasks/phase-3/05-the-wake-cycle.md` and its report (`Backend::exchange(what_we_know, deadline)`, the step table, the budgets, `NullBackend`), `tasks/phase-2/02…05` and their reports (`Store`, `RadioUpdate`, `SelfUpdate`, `Supervisor`, the journal), `tasks/phase-3/02-the-https-client.md` (`Fetch`), `tasks/phase-0/03-the-board.md` (`RadioPins::killCycle`); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5…S-7, S-12.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

Phase 3 left a `Backend` that does nothing. This task puts the agent behind
it: one wake of the library's loop — heartbeat, claim, run, results,
observations — with the four firmware jobs mapped onto the image store and
the update machines of phase 2, and jobs for the unit's own repeater served
over the link. After this the unit is an exit node for its neighbours and a
remotely updatable repeater, and nothing it does was invented here: the
server's page says what it must do, the library does the agent's part, phase
2 does the flashing.

## Deliverables

- **`components/agent/AgentBackend : Backend`** — `exchange(what_we_know,
  deadline)` is `AgentLoop::wake(intent, budget)`:
  - `intent` from 04; `budget` from the deadline and the wake's remaining
    uplink seconds and result bytes (phase 3's budgets);
  - **before the wake**: the loop's state restored from retained storage
    (`AgentLoop::restore`, phase 3's retained-storage hal); **after**: saved
    (`state()`); a backoff the loop asked for becomes *what is asked:
    nothing, and sleep at least N* for the wake cycle;
  - the `WakeReport` mapped into the seam's result: the accepted
    `nextCheckInS` (the wake cycle sleeps by **its own policy**, and the
    server's accepted interval only when it is *longer* — the battery wins);
    the server's time handed to the clock keeper (04); the error counters
    into the wake record;
  - **what is asked** is now what the jobs did: the seam's closed set
    (fetch, radio update, self-update, settings, ladder) is produced *from*
    the extension's jobs rather than from a reply field — the `Backend`
    interface may lose the request enum if nothing else uses it; decide,
    and keep `NullBackend` for the build that has no agent (a build flag
    `WALTER_AGENT=0`, tested in CI as a second host configuration).
- **`components/agent/FirmwareJobs : IJobExtension`**, the four types, each
  exactly as `unit-protocol.md` and the fixtures say:
  - `twinoak.fw.offer` → phase 3's `Fetch` with the payload's `base` under
    `server.url`: manifest and signature (checks 1–3 of the store's
    verdict), the one file for the slot into the candidate slot in ranges,
    resumable across wakes (a half-done download is journalled; the next
    wake's claim of the *same* job id — the server re-queues an expired
    lease — continues it: the extension recognises the job by the manifest's
    hash); progress posted every N kB through `IUplink::postProgress` and
    the lease extended through `extendLease` while the budget holds; result
    `stored` with the image id, hash and bytes, or the failed check's number
    and reason. A wake that ends mid-download posts no result and leaves the
    lease to expire — say in the report that this is the agreed behaviour
    (`unit-protocol.md`) and test that the next wake finishes it.
  - `twinoak.fw.apply` → `RadioUpdate` (the candidate's hash must equal the
    payload's `expectedSha256`, else `refused`), or `SelfUpdate` when the
    payload's product is the Walter's; run **in slices** inside the wake
    with the lease extended; the result's `outcome` and `journal` from the
    machine's journal, truncated to the shape's bounds **by this code**
    (never rely on the server); every precondition the machine refuses maps
    to the result's `refusal` words. The Walter's own apply ends in a
    restart and a trial (phase 2, task 04): the result is posted by the
    **next** boot's first wake, from the journal, and the trial's third
    condition — *reach the server* — becomes *one `postHeartbeat` with
    `ok`*, replacing phase 3's TLS-handshake stand-in. `firstInstall` only
    when the payload says so.
  - `twinoak.fw.state` → the store's records, the running versions
    (`HELLO`, the Walter's own), the journal's last update, the supervisor's
    incident count, into the result shape.
  - `twinoak.power.cycle` → `RadioPins::killCycle(offSeconds)` within the
    board's bounds and the battery's permission (phase 2's rules), then
    `HELLO` within a window; the result's `radioBack` and the new
    `boot_id`.
  Every result is bounded; a `data` object over the library's buffer is a
  bug here, caught by a test.
- **Jobs for the unit's own repeater** run through the library unchanged,
  over 02's local door; a result's `path` is empty and `hops` zero; the
  server's `telemetry` probe over the link is how the unit's power readings
  reach the server (`LPP_GET` carries the INA3221 channels the repeater
  firmware puts in its LPP — confirm against its phase-1 task 06 report).
- **Step 8 of the wake** (`docs/ARCHITECTURE.md` §10) is now this; the
  wake's hard bound and the S-7 rule (one update step per wake) are the
  wake cycle's and still hold around the agent: an `apply` claimed while
  the other MCU's update is unconfirmed is `refused` with `inProgress`.
- **The console**: `agent wake` (one wake now, printing each step and the
  report), `agent jobs` (the last ten jobs as the library remembers them),
  `agent state` (the loop's saved state).
- **`docs/ARCHITECTURE.md` §13** completed; `docs/cycle.md`'s page for the
  implementer of `Backend` replaced by *what the agent does at step 8*.

## Tests (host — the copied fakes, `SimRak` + `FakeMesh`, `FakeHttp`
scripted with the fixtures, `SimFlash`, the simulated chip, a fake clock)

- A wake with nothing waiting: one heartbeat with the intent, sleep by the
  policy's interval; the server's longer interval is honoured, a shorter one
  is not.
- A `status` on a neighbour: through `LinkMesh`, one `client_tx`, the result
  bytes the fixture's shape. A `telemetry` and a `config.read` on the own
  repeater: through the local door, no `client_tx`, results posted.
- **The offer**: three ranges with a drop after the second, then the budget
  ends the wake; the next wake claims the re-queued job and finishes;
  `stored`; the store's record valid; progress and extend calls seen by
  `FakeHttp` in order. A tampered manifest: check 1, nothing stored. A wrong
  board: check 3.
- **The apply**: `committed` after a scripted good candidate (the proving
  period passing under the fake clock, the lease extended); `rolledBack`
  with the journal in the result after a scripted bad one, the candidate on
  the bad list, a second apply of the same hash `refused`; `expectedSha256`
  not matching `refused`; the Walter's own apply: the result arrives from
  the next boot's wake with the trial's outcome, `confirmed` only after a
  heartbeat succeeded.
- `state` matches the store; `power.cycle` within bounds and refused in
  `saving`/`critical`.
- The loop's state round-trips through retained storage across a simulated
  deep sleep; a backoff is honoured by the next wake.
- `WALTER_AGENT=0` builds and runs the phase-3 month unchanged.
- **The month**: phase 3's simulated month with the agent in it — random
  coverage, random jobs for neighbours and for the own repeater, two offers
  and applies (one rolls back), a server away for a week — every wake ends
  in sleep; no job left leased by a finished wake; `client_tx` equals the
  runner's sends; no result over its bound; the supervisor's caps hold; S-7
  holds.

## Acceptance criteria

- [ ] `tools/verify.ps1` green in both configurations (`WALTER_AGENT` on and off).
- [ ] Every scenario above has a test; the month's totals are in the report (wakes, heartbeats, jobs by type, bytes downloaded, flashes, rollbacks).
- [ ] The four job types' payloads and results are read and written through the fixtures' bytes (tests compare).
- [ ] The report maps each step of `unit-protocol.md` to the code that does it, and lists what the page says that this firmware does differently, if anything, as a finding for meshmaster.dk.
- [ ] No reply body, password or token in the log capture over the month (searched).

## Not verified here (Thomas, with the rig, a SIM and meshmaster.dk at 4.14)

Add to `docs/bench.md`, "16. A day as an exit node": register the unit on
`/exit-nodes/new` as *the uplink of* its repeater, `set server.token`, `agent
wake` — the exit node's page shows it recognised, asleep after; schedule a
`telemetry` probe on the own repeater, wait a wake — a sample appears; a
`status` on a neighbouring bench repeater through the unit — one packet; a
test-key release imported on a test server, canary = this unit, offer, apply
— `rak hello` shows the new version and the page shows `committed`.

## Out of scope

The rails and the bench walk (06). NoiseScope as a job. Anything the server
does not yet do.
