# 01 — agent-core v0.7.3

**Depends on:** nothing.
**Written by:** `tools/new-sync-task.ps1 -Tag v0.7.3` on 20 November 2026.
**Size:** one session.
**Model:** claude-opus-5

## What happened

The agent core — `twinoak/twinoak.meshmaster.exitnode-firmware`'s
`lib/agent-core/`, copied here at a commit (`docs/upstream.md`,
`docs/ARCHITECTURE.md` §13) — released `v0.7.3`, and the bump from
`6290e0927a99d252ddcec7929f63719cbb32332d` to `b6d4cfb242cee93c2be5adfbdaa54bf8ef42c60b` is **not green**, which is the only case that
needs a session: a green bump is merged by a person and costs nobody anything.

pull request #87

The failing job, as the reviewer read it on 20 November (run 44695217610, job *verify*, its
second attempt): `tools/verify.ps1` stops in its `host-tests` stage on compile errors in the
copied `host_test/agent-core/test/test_loop/test_loop.cpp` (~L1174, ~L1183, ~L1192, ~L2876,
~L2935, ~L2943 and after): `fake::FakeUplink` has no member `updateResults`, `askForAnUpdate`,
`loseTheAnswerToObservations`, `seenBatches`. The stages before it passed (`spec`,
`spec-tools`, `deps`); nothing after it ran, so this is the first failure and not the list.

## What was read before the run (the reviewer, 20 November)

- **The class the compiler means is ours.** `host_test/agent-uplink/shim/support/FakeUplink.h`
  (~L81) shadows upstream's fake, so that the library's loop suite is compiled a second time
  through `ModemUplink` (`agent_core_loop_uplink_tests`). Its header says it *keeps every knob
  and every record of the original*, and the original has four more since the last copy:
  `askForAnUpdate` and `updateResults` (upstream's p1.15-61), `loseTheAnswerToObservations` and
  `seenBatches` (p1.13-55), in `host_test/agent-core/test/support/FakeUplink.h` as the copy
  brought it. The shim gets the four with the original's meaning, copied and said so as the
  rest of it is. The suite's cases are not changed.
- **This bump is thirteen of upstream's tasks, not one.** The copy was at `6290e09`, which is
  0.6.0 and seventeen commits, before v0.7.0. Between it and `b6d4cfb` lie p1.10-45 to 51,
  p1.11-52 and 53, p1.13-55, p1.15-56, 60 and 61, and sixteen headers of `include/agent/`
  moved: `IUplink.h` (a `postUpdateResult`
  with a body of its own), `IMesh.h`, `IRetained.h`, `KeptImage.h`, `JobRunner.h`,
  `LoginSequence.h`, `ReplyCodec.h`, `Observations.h` among them. Step 1 below reads the
  changelog's sections of v0.7.0, v0.7.1, v0.7.2 and v0.7.3, all four.
- **An update is never asked of a Walter.** meshmaster.dk asks it of an exit node that runs the
  exit-node firmware's own product. Say in the report what this firmware does with an `update`
  in a heartbeat's reply after the bump, and build nothing that answers one.
- **The fixtures the copy brings.** `host_test/agent-core/tests/fixtures/contracts/` now holds
  meshmaster.dk's own file for each of the nine that wait in
  `host_test/fixtures/contracts-ahead/`. Eight are the same bytes;
  `FirmwareApplyProgress.json` is the post's `data` member there and the whole post here.
  Nothing in this repository fails on that, and this task leaves the waiting files and the
  four tests that read them as they are: moving them is a task of its own. Say in the report
  whether any test or rail counts the copied fixtures.
- **Task 15/04 waits for this bump and is not part of it**
  (`tasks/phase-15/04-a-batch-keeps-its-id.md`): what a sleep keeps of a batch's id is its
  work. If the bump cannot be made green without part of it, stop and say which part.

## The task

Bring this firmware to `v0.7.3`.

If `upstream/v0.7.3` is on `origin` — the watch had a credential and pushed
it — `git checkout` it and work there; **do not re-copy a second time**, the
branch already carries the copy. Otherwise branch from `main` as any task does
and make the copy yourself:

```powershell
.\tools\sync-agent-core.ps1 -Ref v0.7.3      # both trees and both manifests, at one commit
.\tools\verify.ps1                            # -Verify runs first, then everything
```

Then, in this order:

1. **Read what upstream changed, in a consumer's terms.** `CHANGELOG.md` at the
   root of the exit-node repository is written for exactly this reader: one
   section per tag, naming any change to the three interfaces, the `MeshReply`
   constants, the fixtures' manifest commit and the library's version
   (`docs/library.md` §10). Read it at `b6d4cfb242cee93c2be5adfbdaa54bf8ef42c60b`:
   `git -C ../twinoak.meshmaster.exitnode-firmware show b6d4cfb242cee93c2be5adfbdaa54bf8ef42c60b:CHANGELOG.md`.
   Then the diff of the four things a consumer watches —
   `docs/upstream.md`'s table says which and why.
2. **Answer the compile errors, which are the contract working.** A method
   added, removed or re-signatured on `IMesh`, `IUplink`, `IPlatform`,
   `IClock`, `IJobExtension`, `JobContext`, `ResultWriter` or `TextOut` is a
   compile error in *our* implementations (`components/agent/`, phase 4's
   tasks 02–04) and is meant to be. Bring them to the new signature; do not
   add a shim that hides it.
3. **Re-read the constants a `static_assert` of ours is held to.**
   `kMeshReplyDataCap` and `kMeshPathCap` are MeshCore's numbers at *upstream's*
   pin, written as the library's own. A reply that does not fit is an overrun
   and not a truncation, so the link's own caps must still be held against
   them at compile time (`docs/library.md` §3.1).
4. **The dangerous one: what the library now *writes*.** An envelope or a
   result shape that moved does not stop anything compiling. It shows up as a
   moved commit in `tests/fixtures/contracts/MANIFEST.txt`, which comes across
   with the copy — compare the old and the new manifest and read the fixtures
   that changed. If the server and a fixture disagree, the server is the
   reference and the fix starts in `twinoak/meshmaster.dk`
   (`docs/upstream.md`, the third tier), never in a local edit here.
5. **The library's own two suites are the proof the copy is whole.** They run
   in this repository's host build (`agent_core_runner`, `agent_core_loop`),
   against the copied fakes and both fixture sets. They pass here as they pass
   there, or the copy is not whole.
6. **Verify**: `tools/verify.ps1`, green. `-Verify` holds both trees against
   their manifests, the rails' entry A holds the library's boundary, and
   `host_test/rails/test_rail_agent_core.cpp` holds the pin's two places equal.

## The rails that must survive this bump

A bump is exactly when one gets quietly lost. All six are the library's own
(`docs/library.md` §8) and this firmware inherits them:

- **TLS is verified.** No `setInsecure()` and no cousin of it, in the library
  or in our uplink, at any pin. The rails' entry A greps the copy for ten
  spellings of it.
- **A password is in RAM for the job and gone.** The claim is parsed in place,
  the credential lands in the job's own buffer, and the library names no store
  and no file system — entry A greps for that too. Our `sendLogin` across the
  link keeps no copy either.
- **One send per command.** A `set` sent twice is a change made twice. The
  link's `client_tx` and the runner's transaction count are equal, exactly.
- **Nothing the mesh says is logged as text.** The library cannot see a frame
  it did not ask for; our link client must drop the mesh's own chat before
  anything decrypts it.
- **Path hashes are two bytes or more, never one.**
- **The contract is the .NET agent's.** One dialect. A second one is a bug
  however well it works.

## Acceptance criteria

- [ ] `tools/verify.ps1` green at `b6d4cfb242cee93c2be5adfbdaa54bf8ef42c60b`, locally and in CI; the report gives the board image's size delta.
- [ ] Both manifests name `b6d4cfb242cee93c2be5adfbdaa54bf8ef42c60b` and nothing else does.
- [ ] The library's two suites pass here, with the same case counts as upstream's, or the report says which case changed and why.
- [ ] The six rails above still hold, each with the test or the entry that says so named in the report.
- [ ] The report says what upstream changed that mattered — not "the pin moved", but which of their changes this session had to answer.

## Out of scope

Editing anything under `components/agent-core/` or `host_test/agent-core/`.
They are a copy; a change the library needs is a change upstream, and the
report carries the diff for it. A bug found on the way gets a line in the
owning phase's `FOLLOWUPS.md`.
