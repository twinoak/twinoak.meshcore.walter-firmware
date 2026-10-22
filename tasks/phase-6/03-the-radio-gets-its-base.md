# 03 — The radio gets its base

**Depends on:** 02; the repeater firmware's pile 3.4 task 01 merged, and its release built locally (`docs/release.md` there: the dry run, signed with the test key, under its `out/release/`). **Stop before the first line** if that `out/release/manifest.json` has no `provision` file in its `rak4630-single` image (a dry run from before 3.4's task 01).
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5, **S-5a**, §6 (the update, step by step), §10.2–10.3; [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §3 (the test key: a build that trusts it is a test build), §4a, §5; `docs/bench.md` §3 *The link*, §4a *The radio's base* (phase 5, task 03), §5 *Flashing*, §8 *Images*, §9 *Updating the radio*; `docs/provisioning.md` *A factory radio*; `docs/console.md`; the repeater firmware's `docs/provisioning.md` §1 (what a blank radio does on its first boot: a random admin password nobody knows, the identity made from radio noise) and `docs/bench.md` §0a (`rf`); `spec/tools/manifestref.py` (`sign`, `descriptor`).
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

The two ★ steps of the bench and the one this repository was written for: on silicon, through the
D-subs, the Walter takes a factory RAK4630 with RUI3, gives it its base (S-5a), gives it its first
application, hears it say `HELLO` over the link — then flashes it a signed image that is broken on
purpose and watches the golden image come back. Only with `"radio": "disposable"` and
`"rf": "dummy-load"` in `bench.local.json`; without them this task stops before its first line.

## Deliverables

- **The build on the Walter is a test build** (it trusts the test key, `FIRMWARE-MANIFEST.md` §3),
  flashed with `bench flash`; the releases it is given are the repeater firmware's local dry run,
  signed with the test key. Say in the report which build and which release (commits, hashes). A
  release build never gets the test key, and nothing here is published.
- **`bench walk 4a`** — §4a by the tool: `walterctl.py push --as base` of the release's `provision`
  file (read from the repeater firmware's `out/release/`, never written there); `image list` and
  `image verify base`; `rak provision check`; the typed line, built from what `check` printed —
  `rak provision <DEVICEID> confirm`, or the `locked` form `check` printed; the six steps and their
  times; the port after the reset. If `rak_usb` is configured, the radio's USB-C now shows the UF2
  bootloader's port with that serial number (the repeater firmware's bench expects the bootloader to
  report the FICR id; say whether it does). If the radio was not a factory one (Thomas may already
  have provisioned it with the hardware repository's bench tool), say so: with `disposable` the walk
  provisions it again, which is exactly the re-provisioning S-5a allows.
- **If the port reads *closed* after the reset, stop there** (`STOPPED-03.md`, with the variant and
  the base's bootloader version): a revision 3 radio whose bootloader does not open its port cannot be
  reached over SWD after any reset, so neither the first install nor any later update can reach it.
  That is the bootloader's to fix (the repeater firmware's pile 3.4 task 01), and Thomas's to decide
  (the hardware repository's `software/FIRST-UNIT.md` §2.8) — not something a session works around.
- **The first image** — `walterctl.py push --as candidate` of the release's `swd` file for this
  board and band, `rak update --first-install` (a first install flashes the candidate; the image that
  passes its health check becomes golden), and the health check's verdict: `HELLO` within the window,
  version and role as flashed. **This is §10.3 on silicon**: the Adafruit bootloader starts an
  application written over SWD with the settings page as the flasher leaves it. Write what it did
  into the report and *What to bring back*, whatever it was.
- **`bench walk 3`** — §3, the link, now that the radio runs a repeater: `rak hello`, `rak ping`,
  `rak id` (the radio's public key and name, as the heartbeat of task 04 will send them), `rak time
  set`, `rak status`, `rak power`, `rak env`, `rak events`; error counters before and after a minute
  of `rak ping` (the link through two bulkheads at 115 200 — zero CRC errors expected).
- **`bench walk 5`** — §5's session lines: `rak flash-test confirm`, then the real image again, the
  timing of each phase (erase, write, verify, settings page, boot), the bytes per second at the
  clock 02 measured and at 125 kHz. The power pulled half-way (★) is a person's: print it as *needs a
  person*, with the exact moment the tool would have pulled it.
- **`bench walk 8`** and **`bench walk 9`** — §8 images, §9 the deliberate bad image (★): the bad
  image is the release's good `swd` file with **its reset vector (the image's second word) set to
  `0xFFFFFFFF`** — the descriptor untouched, so every check of §5 passes and only the health check can
  find it — re-listed in a copy of the release's manifest with the new size and SHA-256, signed with
  the test key (`manifestref.py sign`, the key from `spec/fixtures/manifest/TEST-KEY.txt`), all of it
  under `out/bench/`, nowhere else. `push --as candidate`, `rak update`, and then: the health check
  fails, the golden image is written back, the radio says `HELLO` with the golden version, and the bad
  hash is on the bad list — a second `rak update` of it is refused. Every step's time into *What to
  bring back*.
- **At the end** the radio runs the golden repeater image, its identity made on its first boot (it
  stays: task 04 needs it), and `bench restore` puts back every non-secret setting the walks changed.
- `bench walk 4a|3|5|8|9 --tick`; result files under `out/bench/`; what differs into the follow-ups,
  tagged for the task that wrote the line (phase 5 task 03 for §4a, phase 1 task 05 for §5, phase 2
  task 03 for §9, …).
- Fix what the walk finds broken under the same rule as 02: in the code the walk exercised, small,
  reproduced on the host first; larger is a follow-up.

## Acceptance criteria

- [ ] The task refused to start without `radio: disposable` and `rf: dummy-load` (the tool's log), and ran with them.
- [ ] A factory (or re-provisioned) radio has its base, the port's state after the reset is recorded, and the variant table's choice for `UICR.APPROTECT` is in the report with `INFO.VARIANT`.
- [ ] The first image boots after an SWD flash and answers `HELLO` (§10.3 answered on silicon, either way, in the report).
- [ ] The bad image is rolled back to golden and refused a second time.
- [ ] `tools/verify.ps1` green, including whatever the walks made this task fix.

## Not verified here (Thomas)

The power pulled half-way through a flash (§5 ★). The RAK's USB-C on a laptop after §4a if `rak_usb`
is not configured: the UF2 drive. Whether the identity the radio made is one MeshMaster has seen
before (it cannot be: it is new).

## Out of scope

The modem and the server (04). A release signed with the real key (Thomas, after the key exists:
`software/FIRST-UNIT.md` in the hardware repository).
