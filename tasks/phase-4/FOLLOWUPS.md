# Phase 4 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

## Raised by task 01

### For the reviewer, not for a task of this phase

These are not findings against a task. They are decisions task 01 could not take by itself, and
each one is a sentence somebody has to answer; `tasks/phase-4/reports/01.md` has the whole of the
reasoning.

- [ ] **The agent core arrives as a checked copy, not as a component-manager dependency, because
  `twinoak/twinoak.meshmaster.exitnode-firmware` is private.** The component manager resolves a
  `git` source by cloning *inside the build* — in the ESP-IDF container, which has no credential,
  and on the CI runner, whose `GITHUB_TOKEN` cannot read another private repository — so a build
  pinned the way `docs/library.md` §2 describes is a build nobody can run, CI included. What the
  copy keeps: one commit, two places, a manifest of SHA-256 and size per file, `-Verify` in
  `tools/verify.ps1` and in CI, never edited here. What it costs: a bump needs a machine that can
  read that repository. **The decision to take:** make the exit-node repository public, or accept
  the copy as the arrangement. (A token would also do it, but it is a token in CI *and* in every
  session's container, for bytes that are already in this repository and verified — which is
  worse than either of the other two.) `docs/upstream.md`, *How the library gets here, and why it
  is a copy*, says exactly what changes if the dependency becomes possible; it is small and it
  touches no line of tasks 02–05.
- [ ] **The weekly upstream watch needs one secret before it can do anything**:
  `AGENT_CORE_READ_TOKEN`, a fine-grained token with *Contents: read* on the exit-node repository.
  Without it `.github/workflows/upstream.yml` stops at its first step, green, with the whole
  explanation in the run summary — so the *rehearsal of its pull-request path could not be run*,
  which is acceptance criterion four of task 01 and the reason that task stopped rather than
  merged itself. `gh secret set AGENT_CORE_READ_TOKEN --repo twinoak/twinoak.meshcore.walter-firmware`
  is the whole of it, and then *Run workflow* with `version = 0.1.4` is the rehearsal.

### Reported upstream, for the exit-node repository

Both are in `tasks/phase-4/reports/01.md` as diffs. Neither is patched here (task 01's *Out of
scope*: changing the library is reported upstream), and neither needs a patch file —
`tools/patches/` does not exist and nothing applies one at build.

- [ ] **`lib/agent-core/src/ConfigReply.cpp` has a parameter named `unix`.** GCC predefines `unix`
  as `1` in every GNU dialect on a Linux host, so that file is fifteen errors in any consumer who
  compiles the library as `gnu++17` or later — which is what this repository compiles its own code
  as. The board build never sees it (the xtensa cross compiler has no such predefine) and
  upstream's own host check is `-std=c++17`, so nothing there catches it. Worked around here by
  giving the library and its suites the strict dialect; the fix upstream is a one-word rename.
- [ ] **ArduinoJson 7.4.3 at `-O2` on GCC 13 trips `-Wmaybe-uninitialized`** inside its own
  `CollectionIterator`, attributed to the two library sources that include it
  (`ConfigReply.cpp`, `JobEnvelopes.cpp`) because the diagnostic survives inlining and
  `-isystem` does not silence it. Turned off per file here, as
  `core/third_party/tweetnacl/tweetnacl_verify.c`'s two exemptions are. Upstream may want the same
  exemption in its own host check, or a newer ArduinoJson.
