# 04 — The release 0.1.4

**Depends on:** 01–03 merged.
**First:** check that `release-notes/bench/v0.1.4.md` exists. It is Thomas's record of the candidate on the bench, and it names:
- the commit the candidate was built from;
- its first boot's lines by cable, including the banner and the `heap:` line;
- `self status` after `docs/bench.md` §10's *push the next version and switch* from v0.1.2, saying the new version is `valid (confirmed)`;
- `reboots`.

If it is missing, write `logs/STOPPED-04.md` saying which file and what it must hold, change nothing else, and end. Check too that `keys/release-keys.txt` names the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-04.md` saying which, change nothing else, and end.

**Spec:** `docs/release.md`, `tools/release/build_release.py` and `test_release.py`, `.github/workflows/release.yml`; `tasks/phase-10/07-the-release-0-1-3.md` and its report (the shape of a release task); `release-notes/v0.1.3.md`; `docs/flashing.md` (*From 0.1.2 to 0.1.3, over LTE*), README.md (*The state of things*); this phase's README and the reports of 01–03.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Publish 01–03 as v0.1.4, and make sure the next release cannot do what 0.1.3 did: be published
without ever having started on a board.

## Deliverables

1. **A release has started on a board.** `build_release.py` refuses a tagged release when any of
   these holds:
   - `release-notes/bench/vX.Y.Z.md` is missing;
   - it lacks the three things *First* lists;
   - its commit differs from the release commit in anything outside `docs/`, `tasks/` and
     `release-notes/` (`git diff --name-only`, nothing else).

   A dry run says what is missing and goes on. `test_release.py` has a test for each refusal.
   `docs/release.md` says this is now a step of every release, and why: 9 November.
2. **v0.1.4's notes**, hand-written like every release's:
   - what 0.1.3 did and that it was withdrawn;
   - how to bring back a unit that was flashed with 0.1.3 by cable: flash 0.1.4's factory image,
     and say what that leaves on the unit and what it does not, per `docs/flashing.md`'s
     partition lines;
   - the way from 0.1.2 over LTE: offer, apply;
   - what 03 now reports when a trial fails.
3. **v0.1.3 marked.** If its notes do not already start with Thomas's *Do not install* line,
   `gh release edit` puts it there. v0.1.4 is published as *Latest* and verified, as phase 10's 07
   did.
4. **The docs.**
   - `docs/flashing.md`: *From 0.1.2 to 0.1.4, over LTE* replaces the 0.1.3 section, and there is a
     line for a unit stuck on 0.1.3.
   - README's *The state of things*.

## Acceptance criteria

- [ ] `build_release.py` refuses without the record, with a record that lacks a line, and with a record whose commit differs in code (tests).
- [ ] v0.1.4 published as *Latest*, the five files' hashes in the report, verified with key `73efdd383a957596`.
- [ ] v0.1.3's notes start with the warning.
- [ ] `tools/verify`, the host tests and 02's rail green on the release build.
