# 06 — The release 0.1.1

**Depends on:** 01–05 merged.
**First:** check that `keys/release-keys.txt` names the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-06.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md` (the five files, `NOTES.md`, the refusals, the dry run), `keys/README.md` (the trust table), `.github/workflows/release.yml`, `tools/release/` (`build_release.py`, `sign_manifest.py`, `verify_release.py`), `tasks/phase-7/01-the-first-release.md` and its report (how v0.1.0 was tagged, checked and made *Latest*); `docs/flashing.md`, README.md (*The state of things*), `docs/bench.md`; this phase's README (*In the morning*) and the reports of 01–05.
**Size:** one session.
**Model:** claude-opus-5

## Goal

v0.1.0 went onto the first unit on 6 November and could not do the thing a Walter is for: put an image
on its radio. 01–05 fix what the evening found. This task publishes them as v0.1.1, with a guide for
the one unit that has 0.1.0, so that Thomas can flash it in the morning and finish the radio's first
install.

## Deliverables

1. **The release notes' hand-written top:**
   - what the first unit showed, one line each, with the task that fixed it;
   - that link 1.1 arrived and a 1.0 radio still works.
2. **`docs/flashing.md`, *A unit at 0.1.0*:**
   - **The Walter's own update goes by cable**: the `esptool` line with this release's
     `-factory.bin`. What it keeps: `nvs`, `journal`, `rakfw`, so the stored candidate and a radio
     slot marked *stuck* survive. **Not over LTE.** A self-update fetches from github.com, whose root
     the modem refused on 23 October (`../phase-6/FOLLOWUPS.md`, root 1). Read the code and say
     whether that still holds at 0.1.1; if it no longer does, say how a 0.1.0 unit gets 0.1.1 over
     the air.
   - **Then the radio's first install**:
     1. `rak hello`;
     2. if the candidate held is not the one wanted, `walterctl.py push --as candidate`, which
        handles 0.1.0's conversion by itself now (02);
     3. `rak update --first-install`.

     A committed first install makes the image golden and ends the *stuck* mark.
   - **Then meshmaster.dk**: *Read the unit's state*, and the release can be offered to the unit from
     then on.
3. **README's *The state of things*:** the first unit, as of this release. Name what is still
   unverified on a board, from `docs/bench.md`.
4. **The tag.**
   - Tag `v0.1.1` as `docs/release.md` says.
   - CI is green and the release is published as **Latest**.
   - `verify_release.py` passes against the trust table, on the files as downloaded from the release.
   - The report has the URL, the five files with their hashes, and `verify_release.py`'s last line.

## Acceptance criteria

- [ ] `v0.1.1` is published as *Latest*, every file verifies, and the report shows it.
- [ ] The notes' top and `docs/flashing.md`'s *A unit at 0.1.0* are written; whether 0.1.0 can reach 0.1.1 over LTE is answered from the code.
- [ ] README's state of things is current.

## Out of scope

The board: the flash is Thomas's in the morning. The repeater firmware's release (its pile 3.8).
