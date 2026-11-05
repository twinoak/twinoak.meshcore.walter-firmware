# 01 — The first release

**Depends on:** phases 0–5 merged (they are); the release key, which exists: key id `73efdd383a957596`, its public half in `keys/release-keys.txt` since 20 October (`5627c27`), `RELEASE_SIGNING_KEY` set here and in the repeater firmware. Not on phase 6: this release is what the bench walk flashes.
**First:** check that `keys/release-keys.txt` names that key and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-01.md` saying which, change nothing else, and end: the key is Thomas's, and a desk cannot make it.
**Spec:** `docs/release.md` (the five files, `NOTES.md`, the refusals, the dry run), `keys/README.md` (the trust table, one key shared with `twinoak.meshcore.repeater-firmware`), `.github/workflows/release.yml`, `tools/release/` (`build_release.py`, `sign_manifest.py`, `verify_release.py`), README.md (*The state of things*, *Flashing a Walter*, *Making a release*), `docs/console.md`, `docs/provisioning.md`, `docs/bench.md`, `docs/field.md`; `spec/SYSTEM.md` §10 item 1 (the key); exit-node firmware piles 1.8/35 and 1.9/37 (how a release was tagged, checked and made *Latest* there).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Thomas, 5 November: the unit's repeater came up on the mesh, its Walter (*LTE
test node* on meshmaster.dk, `0.0.0-dev`, last heartbeat 23 October) did not,
and *we're missing the firmware as releases on GitHub, together with a small
guide and release notes.* No tag has ever been pushed here.

The key has existed since 20 October, so a build of `main` trusts it, and a
Walter flashed by cable with this release can take every later one over LTE.
README still says *no release key exists yet* and *no board has run this
firmware*; the first is wrong and the second has been since the unit checked
in on 23 October.

## Deliverables

1. **A short guide**, `docs/flashing.md`, for a person with the unit on a desk:
   - **The Walter:** USB-C, BOOT if no port appears, the one `esptool` line
     with the release's `-factory.bin` at `0x0`. What a factory flash keeps
     (`nvs`, `journal`, `rakfw`, `coredump`, as `docs/release.md` promises).
     The console's banner, and the three lines to look at: slot, trust, modem.
   - **The RAK:** the repeater firmware's own guide (its `docs/flashing.md`)
     for a cable flash, or later through this Walter (`rak` on the console,
     *Offer a release* on meshmaster.dk).
   - **Checking in:** what *the unit checks in* looks like on meshmaster.dk:
     the exit node's page, heartbeat, firmware version.
   - **When it doesn't:** the first three things to look at on the console,
     with a link to `docs/bench.md` for the rest.

   One page. README's *Flashing a Walter* points to it.
2. **Release notes a person reads.** The release body is a short hand-written
   part at the top, then the generated `NOTES.md`:
   - what this release is (the first, and what has and has not been on a
     board);
   - the link to `docs/flashing.md`;
   - which repeater-firmware release it goes with.

   Keep the hand-written part in the repository (`release-notes/vX.Y.Z.md`),
   and have the workflow put it in front of `NOTES.md`. A tag without that
   file is refused, the way a tag without a key is.
3. **The release.**
   - Tag `v0.1.0` on `main` once this task's pull request has merged and CI
     is green on the merge.
   - Watch `release.yml` to the end: the six checks of
     `spec/FIRMWARE-MANIFEST.md` §5 pass before `gh release create`.
   - Make it *Latest*, not a pre-release (Thomas's rule for his firmware
     while he is its only user).
   - Download the published files once, and check `SHA256SUMS` and the
     manifest's signature with `tools/release/verify_release.py` against
     `keys/release-keys.txt`. Say in the report that they match.
4. **Words:** README's *The state of things*:
   - a release exists, and it is `v0.1.0`;
   - the key exists (its id, never more than the public half);
   - a unit checked in with meshmaster.dk on 23 October on a development
     build;
   - the bench walk is still to come.

   Fix `keys/README.md` and the comment block of `keys/release-keys.txt`, which
   still say the key does not exist.

## Acceptance criteria

- [ ] The first check passes on the key that is there, and would stop the task if the line or the secret were gone.
- [ ] `docs/flashing.md` exists, one page, linked from README.
- [ ] `v0.1.0` is a published *Latest* GitHub Release with the five files, `SHA256SUMS`, the signed manifest and a body that starts with the hand-written part.
- [ ] The downloaded files verify against `keys/release-keys.txt` (report).
- [ ] `tools/verify.ps1` green, CI green.

## Out of scope

The bench walk (phase 6). Changing what the firmware does. The repeater
firmware's release (its own pile 3.7).
