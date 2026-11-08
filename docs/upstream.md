# Keeping up with the agent core

This firmware is a MeshMaster agent because somebody else already wrote the
hard half of being one. The job logic — thirteen job types and their rules, the
login sequence and its four classes, the config read and apply engines, the
session cache, the airtime budget, the agent API's envelopes, the reply codec,
the observation ring, the offline result buffer and the image a sleep keeps it
in, the backoff and the server policy, and the loop that drives all of it — is
`twinoak/twinoak.meshmaster.exitnode-firmware`'s `lib/agent-core/`, a library
that stands on three interfaces (and an optional fourth, for a consumer that
sleeps) and knows nothing of Arduino, the ESP-IDF,
WiFi, NVS or MeshCore (`spec/proposals/03`, `spec/SYSTEM.md` §8).

This firmware is built **against** that library, not forked from it. The pin is
one line, twice:

```
components/agent-core/MANIFEST.txt   # source: twinoak/twinoak.meshmaster.exitnode-firmware/lib/agent-core at commit 6290e092…
host_test/agent-core/MANIFEST.txt    # source: … at commit 6290e092…
```

`6290e092` is **eight commits past `v0.6.0`**, which is what `AGENT_CORE_VERSION`
in the copy says: the pin is a *commit* and not always a tag, because the work a
consumer needs is sometimes in the library before its next release. Task p9-05
is the first such pin — the exit-node firmware's tasks p1.10-40 and p1.10-41,
which the local door could not do without — and task p10-01 is the second, for
p1.10-42 and p1.10-43, which the unit's nights could not do without. The two manifests are the
only places the commit is written down, and
`host_test/rails/test_rail_agent_core.cpp` fails if they disagree: the board
would be running one release of the agent and the suites testing another.

The library moves — a release when an interface changes or a job type is
added — and this page is how this repository moves with it. Three tiers, and
the first one is the common case.

## How the library gets here, and why it is a copy

Not a dependency the component manager resolves, which is what
[`docs/library.md`](https://github.com/twinoak/twinoak.meshmaster.exitnode-firmware/blob/master/docs/library.md)
§2 describes and what task p4-01 set out to do:

```yaml
# what a consumer with access to a public upstream would write
dependencies:
  twinoak/agent-core:
    git: https://github.com/twinoak/twinoak.meshmaster.exitnode-firmware.git
    path: lib/agent-core
    version: 6290e0927a99d252ddcec7929f63719cbb32332d
```

**That repository is private.** The component manager resolves a `git` source by
cloning it, and the clone happens *inside the build*: in the ESP-IDF container
on this machine, which has no credential, and on the CI runner, whose
`GITHUB_TOKEN` is scoped to this repository alone. Both fail with *Repository
not found*, so a build pinned that way is a build nobody can run — including
the one CI runs, which is the one that decides whether a branch may merge.

So the library arrives the way `spec/` arrives, which is this repository's own
answer to exactly this question: **a checked copy at one commit, never edited
here, held byte for byte against a manifest.**

| | |
|---|---|
| `tools/sync-agent-core.ps1 -Ref v0.6.0` | `git archive` of that commit out of a clone beside this one, into the two trees, and a `MANIFEST.txt` of SHA-256, size and commit for each. Reads the exit-node clone; writes nothing in it |
| `tools/sync-agent-core.ps1 -Verify` | every file of both trees against its manifest line, plus the two commits against each other. No network, no clone. `tools/verify.ps1` runs it **before the container starts**, and so does CI |
| `components/agent-core/` | the library, as an ESP-IDF component — its own `CMakeLists.txt` and `idf_component.yml`, unedited; ArduinoJson 7.4.3 comes from the public component registry, which needs no credential |
| `host_test/agent-core/` | the four fakes, both fixture sets, the library's own two suites and upstream's `platformio.ini`, at **upstream's own paths**, so that every case of both suites runs here unmodified |

What the copy costs, stated plainly, is the thing a dependency gives for free:
nobody can bump it without a machine that can read the upstream repository. The
weekly watch below is that machine's job, and it needs one secret to be it.

**What would make the component manager's way work**, whenever somebody wants
it: the exit-node repository made public, or a fine-grained token with
*Contents: read* on it available to **both** the container (a git credential
inside `tools/verify.ps1`'s `docker run`) and CI. The second is two secrets, one
of them on every session's machine, for a library whose bytes are already in
this repository and verified; the first is one decision. Either way the change
here is small: `main/idf_component.yml` gains the `git`/`path`/`version` block
above, `host_test/CMakeLists.txt` points `AGENT_CORE` at
`managed_components/twinoak__agent-core`, and the sync script keeps the fakes
and the fixtures (which the component manager does not carry — they are not
part of the library). Not one line of `components/agent/` changes: the include
path is `<agent/X.h>` either way.

## What a bump actually costs us

Four things, and nothing else of that repository needs watching
(`docs/library.md` §10). The first three stop the build; the fourth does not,
and is the dangerous one.

| Watch | Where | Why we care |
|---|---|---|
| the interfaces | `agent/IMesh.h`, `agent/IUplink.h`, `agent/IPlatform.h`, `agent/IRetained.h`, and `agent/IJobExtension.h` and `agent/TextOut.h` | they are *our* code's contract. A method added, removed or re-signatured is a compile error in `components/agent/` — which is the whole point of them being a contract |
| the `MeshReply` constants | `kMeshReplyDataCap`, `kMeshPathCap` in `agent/IMesh.h` | MeshCore's `MAX_PACKET_PAYLOAD` and `MAX_PATH_SIZE` at *upstream's* pin, written as the library's own. A reply that does not fit is an overrun and not a truncation, so the link's caps are held against them with a `static_assert` |
| `library.json` | the library's version, and ArduinoJson's exact version | the one dependency we also have to resolve, and the version `dependencies.lock` must agree with |
| `tests/fixtures/contracts/MANIFEST.txt` | the dialect's commit | the envelopes the library *writes* are meshmaster.dk's at that commit. **Nothing of ours stops compiling when this moves**, which is why it is watched by name |

`CHANGELOG.md` at the root of the exit-node repository is written for exactly
this reader — one section per tag, naming any change to those four — and a
bump's pull request quotes it.

## Tier one — the weekly pull request (automatic, with one secret)

[`.github/workflows/upstream.yml`](../.github/workflows/upstream.yml), Monday
mornings and on demand:

1. `tools/upstream-check.ps1` reads the exit-node repository's `v*` tags and
   compares the newest with the version in the copy. Nothing newer: it says so
   and stops. **It compares versions and not commits**, so a pin taken past a
   tag — as task p10-01's is, eight commits past `v0.6.0` — reads as *up to date*
   until the next tag is cut. That is the right answer for a watch whose job is
   to notice releases, and it is why the row in *The pin's history* below carries
   the commit as well as the version.
2. Something newer: `tools/sync-agent-core.ps1 -Ref <tag>` over a clone the job
   made in `/tmp`, a branch `upstream/<tag>` carrying both re-copied trees and
   both rewritten manifests, pushed, and a pull request **agent-core \<tag\>**
   whose body is the upstream changelog section and `git log` between the two
   pins — what changed, in their words.
3. CI runs on it like any pull request. **Green means the bump compiled for the
   board and for the host, the library's own runner and loop suites still pass
   against the copied fakes, and every rail still holds.** A person merges it.

**The one secret.** That repository is private and a workflow's own
`GITHUB_TOKEN` cannot read it, so the watch needs `AGENT_CORE_READ_TOKEN` — a
fine-grained personal access token with *Contents: read* on
`twinoak/twinoak.meshmaster.exitnode-firmware` and nothing else:

```powershell
gh secret set AGENT_CORE_READ_TOKEN --repo twinoak/twinoak.meshcore.walter-firmware
```

### What the watch cannot do without a credential

Without that secret the workflow stops at its first step, **green**, with the
whole explanation in the run summary, and changes nothing. That is deliberate:
a watch that failed red every Monday would be noise, and one that pretended to
have looked would be worse. Until it is set, the check is a command on a machine
that has the clone — which is every machine a session runs on:

```powershell
.\tools\upstream-check.ps1 -Upstream ..\twinoak.meshmaster.exitnode-firmware
.\tools\sync-agent-core.ps1 -Ref v0.3.0     # the bump, when there is one
.\tools\verify.ps1                          # and then the test of it
```

Nothing is lost but the Monday morning: the bump is the same two commands, and
CI on the branch is the same test.

**One click before CI runs**, once the secret is there. The pull request is
opened by `github-actions[bot]` with the workflow's own `GITHUB_TOKEN`, and
GitHub holds workflows on such a pull request as *action required* rather than
starting them — its guard against a workflow that sets itself off. So the run
waits for **Approve and run** on the Actions tab (or
`gh api -X POST repos/twinoak/twinoak.meshcore.walter-firmware/actions/runs/<id>/approve`),
and that click is the same click as reading the bump.

Only one bump is open at a time: if `upstream/<tag>` is already on `origin`, the
workflow leaves it alone.

You can rehearse the "something newer" path without waiting for upstream:
*Run workflow* with `version = 0.1.4` makes the check believe the copy is three
tags back, so it bumps to the newest one on a real branch and a real pull
request. Close it and delete the branch afterwards — the rehearsal is a test,
not a bump.

## Tier two — a red bump is one command

CI red on such a pull request is the case that needs a session, and there is one
command for it:

```powershell
.\tools\new-sync-task.ps1 -Tag v0.3.0 -Pr 12 -LogUrl <the failing job>
.\tasks\run-all.ps1 -Phase sync
```

It writes `tasks/sync/NN-agent-core-<tag>.md` from
[`tasks/sync/TEMPLATE.md`](../tasks/sync/TEMPLATE.md) — the tag, the pull
request, the failing log, and the standing instructions: read the changelog in a
consumer's terms, answer the compile errors rather than shimming them, re-read
the constants a `static_assert` of ours is held to, compare the fixtures'
manifest commit (the change that stops nothing compiling), and run the
library's own two suites, which are the proof the copy is whole. The template
also lists the six rails that must survive a bump, because a bump is exactly
when one gets quietly lost.

If the watch pushed `upstream/<tag>`, the session continues that branch; if the
watch had no credential, the session branches from `main` and makes the copy
itself. Both are in the template.

## Tier three — a change to the wire protocol

Rare, and not a firmware change first. If the *dialect* moves — an envelope the
library writes, a result shape, a field the server reads — then the reference is
the server and not the library:

1. **meshmaster.dk first.** `MeshMaster.Contracts`' round-trip tests generate
   the fixtures, and that is where the new shape is established.
2. **The exit-node repository second.** Its
   `tools/sync-fixtures.ps1 -Commit <that commit>` brings the fixtures across
   and the library is brought to them; a release follows.
3. **Here third**, as an ordinary tier-one or tier-two bump.

The order is not negotiable: there is one dialect, it is the .NET agent's, and a
firmware that invents a second one is a bug however well it works. Nothing in
this repository may answer a dialect question by editing a fixture: both
fixture sets arrive under `host_test/agent-core/tests/fixtures/` as copies with
manifests of their own, naming the meshmaster.dk commit they came from, and
`-Verify` holds them to it.

## Reading the library you build against

It is on disk, in this repository, at the pin:

```
components/agent-core/include/agent/     the three interfaces and everything else
components/agent-core/src/               the twenty-three sources the board compiles
host_test/agent-core/test/support/       the fakes the suites run against
```

That is not a transcription of it and not a memory of it — it is upstream's
bytes, and `-Verify` says so. The page that explains how to *use* it, method by
method, is `docs/library.md` in the exit-node repository (§11 of it is this
firmware's own mapping, interface by interface); the page for how it fits into
this firmware is [`ARCHITECTURE.md`](ARCHITECTURE.md) §13.

## The pin's history

| Tag | Commit | When | Why |
|---|---|---|---|
<!-- upstream.yml inserts the newest row directly under this line -->
| `v0.6.0`+8 | `6290e092` | 8 November 2026 | task p10-01, for the night of 8 November: `AgentLoop::restore` gained a second parameter, *how long the board was away*, and takes it off a hold or a `Retry-After` (p1.10-42) — without which one failed call bought six hours of silence at the unit's hourly wake. `IMesh::answersLocallyFor(key)` came with it (p1.10-43), so the wake prices a job the local door answers at its reply waits instead of at airtime gaps it never spends, and `cliReplyValue` reads a bare `>` as the empty value rather than a raw line (p1.10-44). No release between the two pins, so `AGENT_CORE_VERSION`, `library.json`, `idf_component.yml` and ArduinoJson's 7.4.3 are all unmoved: this bump is three commits of behaviour and not one byte of version |
| `v0.6.0`+4 | `8c24da5b` | 8 November 2026 | task p9-05, and the first pin that is not a tag: `IMesh` gained `answersLocally()` (p1.10-40) and `loginMaterialFor` a second parameter (p1.10-41), which together are the local door's *no airtime, no password* — the work the unit needed four commits before the library's next release. Three releases came with it (`v0.3.1` the UTF-8 strings, `v0.4.0` the heartbeat's observer and `failedAt`, `v0.5.0` the flood scope and the neighbours, `v0.6.0` a lighter TLS on a Heltec), every one of them additive: nothing of this repository changed to compile against them |
| `v0.3.0` | `cc5a3436` | 22 October 2026 | the pin task p4-01 landed with (its second session): `IRetained` and the image a sleep keeps, a wake that keeps its budget, `IMesh::supports()`, `MeshReply` without `viaPath`/`snr`, and both of p4-01's upstream findings taken — so this repository compiles the library in its own dialect and exempts no warning of it |
| `v0.2.0` | `bbe7ae05` | 15 October 2026 | the first copy (task p4-01, first session): the first release of the library a consumer may pin, and the first ESP-IDF build of it anywhere - never on `main` |
