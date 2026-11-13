# 02 — A release from the button

**Depends on:** 01 (it rewrites the same pages).
**First:** read only. `gh api repos/twinoak/twinoak.meshcore.walter-firmware/actions/permissions/workflow` must say the workflow's own token may create pull requests. If it may not, write `logs/STOPPED-02.md` naming the setting (*Settings → Actions → General → Allow GitHub Actions to create and approve pull requests*), change nothing else, and end: a setting is Thomas's to change.
**Spec:** `.github/workflows/release.yml` (the two triggers ~L36; the version from the tag ~L82; what a tag is refused for ~L97–130; the boot gate before the signing; `gh release create` ~L321), `.github/workflows/ci.yml`, `tools/release/` (`build_release.py`, which refuses a release without `release-notes/<tag>.md`; `verify_release.py`, `boot_gate.py`, `sign_manifest.py`, `test_release.py`), `release-notes/README.md` (*What belongs in one*: the four things a top says) and the five tops, `docs/release.md` (*The two things a tag needs*, *Releasing*, *Rehearsing*, *What the tooling refuses*), `CMakeLists.txt` (`PROJECT_VER`), `tasks/RUNBOOK.md`, `tools/verify.ps1` and `verify.sh` (the local release step, S-10), `tasks/phase-11/04-a-release-starts-first.md` and its report; **meshmaster.dk, read only, as the model**: its `.github/workflows/release.yml` (*Compile the release notes* ~L168; the pull request that files them, opened and merged by the job's own token, ~L341–410), `tools/ci/Compile-ReleaseNotes.ps1`, `release-notes/unreleased/`, and its `tasks/RUNBOOK.md` on a task's release note.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

On 13 November Thomas pressed *Run workflow* on the exit-node firmware's `release` and nothing was
published: in the firmware repositories a run by hand is a dry run, and a release is a pushed tag
with its notes written first. v0.1.4 here went out on 10 November by a notes file committed and a
tag made through github.com, because nobody was at the workstation to do it the usual way.

**Decided, 13 November (Thomas):** the firmware repositories release the way meshmaster.dk does.

- **The button publishes.** Nothing is typed. A *dry run* box is there for a rehearsal.
- **The notes write themselves.** A task that changes something a person will notice leaves one
  line. A release is those lines.
- **The version is in the repository.** It names the release that comes next, and the workflow
  moves it to the next patch after a release, in a small pull request of its own.
- **It refuses when there is nothing to say.**

A release is still signed or it is not a release, still refused on a commit CI has not passed,
and still booted in QEMU before it is signed.

## Deliverables

1. **The version is in the repository, in one place, and it names the next release.** Today that
   is `0.1.5`. The workflow reads it from there and from nowhere else; `PROJECT_VER`, the image's
   descriptor and the manifest carry it with the commit as now.
2. **A task leaves a line.** `release-notes/unreleased/<task>.md`: what a person with a unit will
   notice, and **what has and has not been on a board**, in a sentence or two.
   `tasks/RUNBOOK.md` gets the rule in meshmaster.dk's words: a change somebody will notice has a
   line, a change nobody will has none.
3. **`workflow_dispatch` publishes**, from `main`'s head and no other commit:
   - CI green for that commit, the boot gate, and the signature with `RELEASE_SIGNING_KEY`, all as
     now and in that order. A release without the key still fails;
   - the body's top is the unreleased lines, in the order the tasks merged, where the
     hand-written file was. The generated part below it is unchanged;
   - the release is created with its tag on that commit, as **Latest**;
   - a tick box **dry run** does what a run by hand does today: the same jobs, the
     specification's test key, `0.0.0-dryrun`, nothing published. Only a dry run may name another
     `ref`.
4. **What every top said by hand** (`release-notes/README.md`'s four things). The link to the
   flashing guide and anything else that can be generated is generated. What cannot is a standing
   paragraph in the repository that the task which changes it edits. `release-notes/README.md`
   is rewritten to say what a line is.
5. **It refuses, before anything is built, and says why in one sentence:**
   - no unreleased line: there is nothing to release;
   - the tag of the version already exists: the version was not moved on after the last release,
     and the sentence says how to finish that;
   - a release asked for from a ref that is not `main`.
6. **After the release is out, one pull request, opened and merged by the workflow:** the lines
   move from `unreleased/` into `release-notes/vX.Y.Z.md`, as published, and the version moves to
   the next patch. Where `main`'s rules do not let the workflow merge it, the pull request is left
   open and the run's summary says so. The release stands either way, and the next one refuses
   until the version has moved.
7. **The commit that pull request makes** touches only the release's own files. Say how the
   *ci is green* gate reads it at the release after: it has no CI run of its own.
8. **The tag trigger goes.** One way to release. A tag pushed by hand starts nothing.
9. **The parts that can be tested are.** Compiling the lines, the refusals and the move after a
   release, in `tools/release/` beside the tests that are there, against a scratch repository.
   The local release step of `tools/verify` follows the new path. A dry run from this task's
   branch is in the report.
10. **The words.** `docs/release.md`, rewritten where it says *tag*; `tasks/RUNBOOK.md`; README;
    and this task's own line in `release-notes/unreleased/`.

This task publishes nothing and pushes no tag. The key's private half stays where it is: in the
secret, and in no file, no log and no commit.

## Acceptance criteria

- [ ] The version is read from one place, and a build's descriptor and manifest carry it (test).
- [ ] Two unreleased lines compile into the body's top in merge order; none is a refusal with its sentence (tests).
- [ ] An existing tag of the version, a ref that is not `main`, and a release with no key are each refused before the build (tests).
- [ ] After a release in a scratch repository: the lines are in `release-notes/vX.Y.Z.md`, `unreleased/` is empty and the version is the next patch (test).
- [ ] The five published notes in `release-notes/` are untouched.
- [ ] The boot gate still runs before the signing, on a release and on a dry run.
- [ ] The dry run from this branch is green and published nothing; its run is in the report.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

A release. The board. The repeater's and the exit node's repositories, which get the same in their
own piles.
