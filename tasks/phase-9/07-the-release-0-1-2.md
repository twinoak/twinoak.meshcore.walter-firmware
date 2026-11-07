# 07 — The release 0.1.2

**Depends on:** 01–06 merged.
**First:** check that `keys/release-keys.txt` names the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-07.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md`, `keys/README.md`, `.github/workflows/release.yml`, `tools/release/`, `tasks/phase-8/06-the-release-0-1-1.md` and its report, the phase-8 review's notes on it (`../phase-8/FOLLOWUPS.md`: list all five files' hashes in the report); `docs/flashing.md`, README.md (*The state of things*), `docs/field.md` (an update over LTE); the reports of 01–06; the repeater firmware's tags (read only).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Publish 01–06 as v0.1.2. It is the first release whose way onto a unit already in the field is over
LTE: a unit at 0.1.1 can carry its own download, as 7 November showed.

## Deliverables

1. **The release notes' hand-written top.** Each of 01–06 in a line, with what a person saw on
   7 November: the config read that reset the Walter, the stall after five connections, the job that
   needed a password nobody reads, the repeater that stayed *stale*.
   - 06 needs the radio at repeater v0.1.2 (link 1.2). Say so, and which repeater release is current
     when this runs, from that repository's tags. With a radio at 0.1.1 the unit works as before.
   - Check every sentence against the code. The phase-8 review found two wrong sentences in 0.1.1's
     notes.
2. **`docs/flashing.md` and `docs/field.md`: from 0.1.1 to 0.1.2 over LTE.**
   - *Offer a release* on the unit's page with the Walter's product, then the apply, and what the
     console and the unit's page say at each step.
   - Read the code before writing that it works: the Walter's own image comes from meshmaster.dk's
     URL, held to ISRG Root X1.
   - The cable stays the way for a unit at 0.1.0.
3. **README's *The state of things*:** current.
4. **The tag.**
   - Tag `v0.1.2` as `docs/release.md` says.
   - CI is green and the release is published as **Latest**.
   - `verify_release.py` passes on the downloaded files.
   - The report has the URL, **all five files with their hashes**, and the verifier's last line.

## Acceptance criteria

- [ ] `v0.1.2` is published as *Latest* and verifies; all five hashes are in the report.
- [ ] The notes are checked against the code; the LTE update is written from the code.
- [ ] README current.

## Out of scope

The board: the update is Thomas's.
