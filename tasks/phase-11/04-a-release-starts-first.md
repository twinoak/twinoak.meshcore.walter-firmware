# 04 — A release starts before it is published

**Depends on:** 01–03 merged.
**Already done, by hand, on 10 November:** `v0.1.4` itself. Thomas was away from the workstation and wanted to try the LTE update again, so `release-notes/v0.1.4.md` was committed to `main` (62538fa) and the tag pushed from github.com. `release.yml` published it as it publishes every tag. **This task publishes nothing, tags nothing and does not touch v0.1.4's notes.** Read them, and if the update over LTE has happened by the time you run, `docs/flashing.md`'s and README's sentences say what it showed (ask nobody; the unit's page on meshmaster.dk is not yours to read, so say *not known when this task ran* if it is not in the repository).
**Spec:** `.github/workflows/release.yml`, `tools/release/` (`build_release.py`, `verify_release.py`, `test_release.py`), `tools/verify.sh` and `verify.ps1` (the local release step, S-10), `docs/release.md`; the QEMU the pinned image ships (`/opt/esp/tools/qemu-xtensa/…`, 01's report ~L121–135 and `tools/heap_programme.gdb`: how 01 and 02 booted this firmware in it and read it); `release-notes/v0.1.4.md`, `docs/flashing.md` (*From 0.1.2 to 0.1.3, over LTE*), README.md (*The state of things*), `docs/bench.md` §1.
**Size:** one session.
**Model:** claude-opus-5

## Goal

v0.1.3 was published without ever having started anywhere: no board, no emulator. 02's rail now
stops the one way it failed at build time. This task makes a release **start before it is
published**, in CI, with nobody at a bench:

- **Not a bench record.** An earlier version of this task asked Thomas for one. A dry run is
  signed with the specification's test key and a v0.1.2 unit accepts only manifests signed with
  `73efdd383a957596`, so the switch from the release before cannot be walked before publishing.
- **Not a step a person must do.** It would hold every release to the workstation.

The QEMU that 01 and 02 already used to read the heap is the board this gate has.

## Deliverables

1. **The boot gate.** `release.yml` boots the release's factory image in the pinned image's QEMU
   (esp32s3), **before the manifest is signed**, on a tag and on a dry run. `tools/verify`'s local
   release step runs the same gate (S-10: local first). It refuses unless, within a bound you
   choose and state:
   - the image reached `app_main`;
   - the `heap:` line was printed, with the main task's stack taken;
   - the banner names the version being released.

   Read it the way that works in this QEMU: its console, if it carries this firmware's USB
   output, or `gdb` breakpoints the way 01 and 02 did. Say which, and why. What QEMU cannot give
   (the modem, the radio, the trial's conditions) is not asked for: the gate is *it starts*, and
   nothing more.
2. **Proof both ways.** In the report, with the output:
   - the gate refuses `v0.1.3` rebuilt at its tag, with the reason it printed;
   - it passes `v0.1.2` and `main`;
   - `test_release.py`, or a test beside the gate, holds its parsing of the output to a passing
     and a failing transcript.
3. **The words.**
   - `docs/release.md`: the gate, what it proves and what it does not. The first unit over LTE
     is still the first time the radio, the modem and the trial run on a release; a failed trial
     rolls back, and from the release after 0.1.4 it says why (03).
   - `docs/flashing.md`: *From 0.1.2 to 0.1.4, over LTE* replaces the 0.1.3 section (the same
     walk), plus a short *A Walter stuck on 0.1.3* that says to flash 0.1.4's factory image by cable.
   - README's *The state of things*.
   - `docs/bench.md` §1: the dry-run factory image is still how a person checks the `heap:` line
     on silicon, and it is no longer a gate.

## Acceptance criteria

- [ ] The gate refuses `v0.1.3` and passes `v0.1.2` and `main` (output in the report).
- [ ] A dry run of `release.yml` on this task's branch is green, with the gate's lines in its summary.
- [ ] Nothing published, no tag pushed, `release-notes/v0.1.4.md` unchanged.
- [ ] `tools/verify`, the host tests and R22 green.
