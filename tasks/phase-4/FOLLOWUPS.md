# Phase 4 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

## Before the run resumes (20 October, from the reviews of the exit-node firmware's pile 1.6 and meshmaster.dk's 4.14–4.15)

- [ ] **[01] Pin `agent-core` v0.3.0, not v0.2.0.** The exit-node firmware's pile 1.6 closes v0.3.0
  with its task 24 (`IRetained`, a wake that keeps its budget, `HeartbeatIdentity::transport`,
  `IMesh::supports()`, `MeshReply` without `viaPath`/`snr`, `uniqueTime()` gone); Thomas tags it
  after 24 merges. When this task resumes on its branch, take the copy at the tag `v0.3.0`, and drop
  the two local measures its report 20 made unnecessary (the `unix` parameter, the narrow
  `-Wmaybe-uninitialized`) — say in the report which of the library's changes moved what. If
  `v0.3.0` is not tagged when the task resumes, stop (`STOPPED-01.md`) rather than pin an untagged
  commit.
- [ ] **[01] The rehearsal moves to after the merge.** `STOPPED-01.md` asks for the *upstream*
  workflow's rehearsal before merging, but GitHub starts a `workflow_dispatch` workflow only from a
  file on the default branch, and `upstream.yml` is on this task's branch alone — so the criterion
  cannot be met first. When the task resumes (`-From 01`): the checked copy stands (resuming is
  Thomas accepting it; making the exit-node repository public first is the other choice, and then
  `docs/upstream.md` says what changes); `AGENT_CORE_READ_TOKEN` is set or not — the workflow is
  green either way; the fourth checklist line becomes *rehearsal after merge, Thomas's* with the
  command (`gh workflow run upstream.yml -f version=0.1.4`, then close the pull request it opens with
  `--delete-branch`); and the task merges when everything else is green.
- [ ] **[02] What v0.3.0 asks of `LinkMesh`** (exit-node report 23, *For the Walter*): implement
  `supports(jobType)` — `trace` is `false` until `LINK-PROTOCOL.md` has a trace request (it does
  not; the link's §6 has no `overheard` and no per-type counters either — two specification gaps
  for the hardware repository, in the report); no `viaPath` or `snr` in a reply; the timestamp of a
  CLI send returned, not taken.
- [ ] **[04] A heartbeat with no radio key.** The task says that with no radio identity
  (`rak role scanner`, a silent radio) *no heartbeat is built*. Since 20 October a unit is
  registered at meshmaster.dk **before** its repeater, and its radio may have no identity yet
  (RUI3, no base, no application). The unit still checks in: a heartbeat **without**
  `companionPublicKey` (meshmaster.dk's `Envelopes.cs` makes it optional; the server keeps the key it
  had) and with **no job types** — a unit without a radio can run no job, and an empty list is how it
  says so — so that its page shows it alive and *radio not identified*. `agent id` says the same.
  The first heartbeat after the radio has an identity carries the key, and that is what pile 4.16's
  task 132 links on. Host tests: both heartbeats against meshmaster.dk's contract fixtures — the one
  without a key against `AgentHeartbeatNoKey.json`, which meshmaster.dk's pile 4.16 task 132 adds
  (`AgentHeartbeatNoWake.json` still carries a key); if it is not on meshmaster.dk's `master` when
  this task runs, build the expected body from `AgentHeartbeat.json` minus the field and say so; the
  identity appearing between two wakes.
- [ ] **[06] Number the agent's bench sections after the ones that exist.** `docs/bench.md` already
  has §15 *Sleep* and §16 *A day*; the task's *15. The agent* and *16. A day as an exit node* become
  §17 and §18 (and the walk's table at the top gains them), so that no section number means two
  things.
- [ ] **[06] The bench lines for the unit before its repeater** (`docs/bench.md` *The agent*): the exit node
  registered with no repeater; the first wake with a radio that has a key; *Register this unit's
  repeater* on the exit node's page (pile 4.16 task 132), and the repeater's page showing the unit as
  its uplink at once.

## After the run (23 October, from task 06 — the specification gaps this phase raised)

Nothing here is this repository's to change: `spec/` is a copy (`spec/README.md`) and the other two
repositories are read-only to every task of this pile. Each item names **the repository it belongs
to** and the report that raised it, and each is a thing this firmware has already worked around in
the smallest way that fits — so none of them blocks the bench.

### The box link, for `LINK-PROTOCOL.md` 1.1 (the hardware repository)

- [ ] **§6.4 has no `overheard` and no count by payload type** (report 02, report 04). The
  library's heartbeat carries both — `overheard` (frames of the mesh's own chat heard and not read)
  and `rxByType[16]` — and nothing on this link can tell either, so `LinkMesh::overheard()` answers
  0, which `docs/library.md` §3.2 defines as *this transport cannot see chat*, and the by-type
  counts are never filled.
- [ ] **§6.2 does not say that `timeout_ms` is a ceiling, and §6 has no way to end a wait**
  (report 02). The library gives its timeout at the *await*, not at the send, so the send carries a
  60 s ceiling and the Walter runs its own timer; when it gives up, the only request that ends the
  RAK's wait without ending the job is `JOB_BEGIN` with the same peer, which is what this firmware
  sends. One sentence in §6.2 — or a `REPLY_CANCEL` — would make that the specification's reading
  rather than this firmware's.
- [ ] **§5 has no `STATS_GET` and no `NEIGHBOURS_GET`** (report 02), so a `status` or a
  `neighbours` job for the unit's **own** repeater cannot be served over the UART and is reported
  `failed` at the local door — while the same job for a neighbour on the air is answered. Two
  requests (the bytes of `RepeaterStats` as a mesh status request answers them, and the paged
  binary neighbour list) would close the one real hole in the local door. **The likeliest thing a
  person will notice on the bench**, and the first candidate for link 1.1.
- [ ] **A trace of the unit's own repeater cannot work, and the library's page says the link has no
  trace at all** (report 02). `spec/LINK-PROTOCOL.md` 1.0 *does* have `SEND_TRACE` (§6.2) and a
  trace record (§6.3), so `supports("trace")` is true here; what is true is narrower — a RAK cannot
  trace a round trip to itself, so a trace of the own repeater is `Failed` at the local door. The
  sentence to correct is **the exit-node repository's** `docs/library.md` §11, which says the link
  has no trace request.

### The agent library (`twinoak.meshmaster.exitnode-firmware`, `lib/agent-core/`)

- [ ] **`mm::HeardAdvert` has no field for the advert's own timestamp** (report 04). §7 carries
  `adv_ts` inside the advert; the struct keeps only when *we* heard it, which is what the server's
  `Observation.At` means, so nothing the server has a field for is lost — but the figure is
  thrown away here.
- [ ] **No field for MeshCore's two reserved feature words** (report 04). `ADV_FEAT1`/`ADV_FEAT2`
  are stepped over so that the name begins where the builder put it, and meshmaster.dk's
  `ObservationFields.Flags` is therefore never sent. Worth nothing until MeshCore uses the bits.

### meshmaster.dk

- [ ] **`unit-protocol.md` §5 does not say what a wake that runs out of budget mid-download does**
  (report 05). This firmware posts **no result** and lets the lease expire, which the lease's own
  expiry handles and which keeps the server from seeing a half-answer; a line in §5 saying so would
  remove the ambiguity.
- [ ] **The Walter's own apply result arrives a wake late** (report 05). §5's table reads as though
  a result is posted in the apply's own job; for a self-update it cannot be — the board reboots into
  the image being tried and the result comes from the *next* boot's wake, after the trial's third
  condition. A note in §5, not a conflict: `Settle` is the server's equivalent.
- [ ] **API §3 decides nothing about `X-Agent-Version`'s content** (report 03). The header must be
  present; what it carries is the consumer's choice, and this firmware sends **its own** version
  (`core::version()`), with the library's reported through the heartbeat's identity instead. A
  sentence in §3 would stop the next consumer guessing differently.
- [ ] **`AgentHeartbeatNoKey.json`** (FOLLOWUPS' `[04]` item, pile 4.16 task 132). Task 04 built the
  keyless body from `AgentHeartbeat.json` minus the field because the fixture was not on `master`
  yet; when it lands, task p4-04's `test_heartbeat_body.cpp` should read it instead of constructing
  the expectation.

### For the bench pile (phase 6)

The agent's own items, which phase 6's `tools/bench/` must be able to drive — and the ones it must
refuse:

- [ ] **§§17–18 of `docs/bench.md` are the agent's walk**, and the three marked ★ are what a first
  evening should buy: the first heartbeat recognised, an offer resumed after a pulled cable, an
  apply that rolls back on a bad candidate.
- [ ] **A session must never be given the token.** `set server.token` is a line `tools/bench/`
  refuses, like every other line that sets a secret: Thomas types it once, over the cable, and the
  tool sees *set* or *unset* (`tasks/RUNBOOK.md`, *Phase 6*). The agent's own lines — `agent url`,
  `agent probe`, `agent id`, `agent heartbeat`, `agent obs`, `agent wake`, `agent jobs`,
  `agent state` — are read-only and safe to drive; `agent obs pull` moves records out of the
  radio's ring and is therefore not repeatable, which the tool should say rather than hide.
- [ ] **An agent item needs the server as well as the rig**, so `bench.local.json`'s `"server":
  true` gates §§17–18 the way `"sim": true` gates the modem's. A unit registered at meshmaster.dk
  is a real row in somebody's register: the bench's exit node should be its own, and the tool
  should refuse to drive a token's unit it was not told about.
- [ ] **A job for a neighbour is airtime on somebody else's repeater.** The ⚠ rule for the radio
  (`"radio": "disposable"`) does not cover a *second* repeater on the air; the bench pile should
  decide whether a `status` on a neighbour needs the neighbour named in `bench.local.json` too.
