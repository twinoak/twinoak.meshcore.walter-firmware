# 01 — The library arrives

**Depends on:** phase 3; the exit-node firmware's pile 1.5 merged and tagged.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) §8 and [`spec/proposals/03-agent-core-as-a-library.md`](../../spec/proposals/03-agent-core-as-a-library.md) (*The shape*: "the Walter firmware consumes the library at a pinned commit, as this repository consumes MeshCore"); `twinoak.meshmaster.exitnode-firmware` at the tag `v0.2.0` (read with `git -C ../twinoak.meshmaster.exitnode-firmware show v0.2.0:<path>`, or `gh api`): `docs/library.md` (all of it), `lib/agent-core/library.json`, `CMakeLists.txt`, `idf_component.yml`, `test/support/`, `tests/fixtures/contracts/` with its `MANIFEST.txt`, `docs/upstream.md` (*this repository is an upstream*); this repository's `tasks/phase-0/01-the-toolchain.md` and its report (the component manager, the lock file, how `host_test/` is built), [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1 and §12, `tools/verify.ps1`, `tools/check_rails.py`; the repeater firmware's `tasks/phase-0/03-the-upstream-watch.md` (the three-tier arrangement to copy).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The agent's job logic arrives as a dependency with a version, not as files
copied by hand: the exit-node firmware's `lib/agent-core/` as an ESP-IDF
component at one commit, built for the board and for the host, its fakes and
the contract fixtures beside it under a checked copy, and a watch that tells
us when the library moves. This is the first time the library's ESP-IDF face
compiles anywhere; whatever is wrong with it is this task's to report
upstream, not to patch here.

## Deliverables

- **The component.** `main/idf_component.yml` (or the root's) gains
  `agent-core` from `git: https://github.com/twinoak/twinoak.meshmaster.exitnode-firmware.git`,
  `path: lib/agent-core`, `version:` the **commit** of `v0.2.0` (a commit, not
  the tag: a tag can move), and `bblanchon/arduinojson` at the exact version
  the library's `library.json` names, from the registry or vendored — say
  which the lock file ends up with. `dependencies.lock` updated. The library's
  `CMakeLists.txt` must register without edits; if it needs one, the report
  carries the diff for the exit-node repository and this task carries a
  `tools/patches/agent-core.patch` applied at build with a loud comment —
  temporary, tracked in FOLLOWUPS.
- **The host build.** `host_test/CMakeLists.txt` compiles the component's
  sources from the managed-components checkout (the same bytes the board
  gets) with the host compiler and this repository's sanitizer flags, so a
  warning in the library is a warning here. `components/core/` keeps its
  rule — no ESP-IDF include — and the library is not under `components/core`;
  it is a component of its own, and the rails' grep (`check_rails.py`) adds
  it to the roots it scans for TLS bypasses and logged secrets, and asserts
  the library's own boundary (no `Arduino.h`, no `esp_*`) as the library's
  own test does, so a bump that breaks the boundary is caught here too.
- **The fakes and the fixtures, by checked copy.** `tools/sync-agent-core.ps1`
  copies `test/support/` and `tests/fixtures/contracts/` from the exit-node
  repository **at the same commit** into `host_test/agent-core/` with a
  `MANIFEST.txt` of SHA-256 and size and the commit, in the shape
  `spec/MANIFEST.txt` has; `-Verify` runs in `tools/verify.ps1` and CI, and
  a test asserts the manifest's commit equals the component's pinned commit
  (one line in two places is how pins drift). Never edited here.
- **The library's own tests, here.** The suites of `test/test_runner/` and
  `test/test_loop/` (pile 1.5, 17–18) compiled into `host_test/` against the
  copied fakes: they are googletest; this repository's host tests are
  whatever phase 0 chose — if not googletest, add it to the host build for
  these suites only, pinned, with a sentence. They pass here as they do
  there, which is the proof the copy is whole.
- **The upstream watch**, as the repeater has for MeshCore: a weekly
  workflow that reads the exit-node repository's `v*` tags, opens a pull
  request moving the commit in `idf_component.yml`, the lock file and the
  sync manifest together, with the upstream commit list in the body; CI on
  the pull request is the test. `docs/upstream.md` here: what a bump
  re-reads — the three interfaces and `IJobExtension` (tasks 02–05 implement
  them), the `MeshReply` constants, the fixtures' manifest, `library.json`'s
  ArduinoJson version — and the rehearsal input. `tools/new-sync-task.ps1`
  for a red bump, writing into `tasks/sync/`.
- **`docs/ARCHITECTURE.md`** §1 gains the component and §12 the row; a new
  §13 *The agent*, the design in a page, written from `docs/library.md`: what
  the library does, what tasks 02–05 provide, where `wake()` sits in §10's
  sequence (step 8, the backend).

## Tests (host)

- The library compiles for the host under `-Werror` and the sanitizers;
  the board build links it (size before and after in the report).
- `sync-agent-core.ps1 -Verify` green; the commit-equality test green; a
  deliberately edited copied file fails `-Verify` (then restored).
- The copied suites pass.
- `check_rails.py` scans the component; a planted `setInsecure(` in a scratch
  copy of a library file is caught (then removed).

## Acceptance criteria

- [ ] `tools/verify.ps1` green with the component in the board build and in the host build; the report gives the board image's size delta.
- [ ] The pinned commit appears in exactly two places (`idf_component.yml`, the sync manifest) and a test holds them equal.
- [ ] The library's runner and loop suites pass in this repository's host build.
- [ ] The upstream workflow's rehearsal opened and closed a pull request; the report links the run.
- [ ] Every edit the library needed to build under ESP-IDF is in the report as a diff for the exit-node repository, and in FOLLOWUPS as temporary if a patch file exists.

## Not verified here

Nothing on a board changes yet: the library is linked and unused until 05.

## Out of scope

Implementing any seam (02–05). Changing the library (report upstream).
