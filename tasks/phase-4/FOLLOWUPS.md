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
