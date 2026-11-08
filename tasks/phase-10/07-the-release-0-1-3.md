# 07 — The release 0.1.3

**Depends on:** 01–06 merged.
**First:** check that `keys/release-keys.txt` names the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-07.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md`, `keys/README.md`, `.github/workflows/release.yml`, `tools/release/`, `tasks/phase-9/07-the-release-0-1-2.md` and its report (the shape of the report, and all five files' hashes); `docs/flashing.md` (*From 0.1.1 to 0.1.2, over LTE*: the same path), README.md (*The state of things*), `docs/bench.md`; the reports of 01–06; meshmaster.dk's `tasks/phase-4.28/` (read only: whether 177 has merged).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Publish 01–06 as v0.1.3. The way onto a unit is over LTE, an offer and an apply, once meshmaster.dk has 177 deployed. Until then it is the cable.

## Deliverables

1. **The release notes' hand-written top.** Each of 01–06 in a line, with what a person saw on
   8 November:
   - a unit that woke every 15 minutes and was heard on every sixth wake;
   - a journal that said *reached* when nothing was sent;
   - a radio without its identity that looked like one awaiting its install;
   - a download message that promised a resume;
   - an offer that failed in two seconds, because no release had ever been fetchable over LTE;
   - a stall that could only be traced with a special build.

   Check every sentence against the code. Say that the radio's key was **not** kept on 6 November,
   correcting v0.1.1's notes, and that a radio at repeater v0.1.2 refuses instead.
2. **`docs/flashing.md`**: from 0.1.2 to 0.1.3 over LTE, with what 04 and 05 changed in what the
   console and the page say, and that it needs meshmaster.dk's 177 deployed. Say whether 177 has
   merged when this runs.
3. **README's *The state of things*:** current.
4. **The tag.**
   - Tag `v0.1.3` as `docs/release.md` says.
   - CI is green and the release is published as **Latest**.
   - `verify_release.py` passes on the downloaded files.
   - The report has the URL, **all five files with their hashes**, and the verifier's last line.

## Acceptance criteria

- [ ] `v0.1.3` is published as *Latest* and verifies; all five hashes are in the report.
- [ ] The notes are checked against the code, and correct the key sentence of v0.1.1.
- [ ] README current.

## Out of scope

The board: the update is Thomas's.
