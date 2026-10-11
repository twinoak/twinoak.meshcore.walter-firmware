# 02 — The release

**Depends on:** 01.
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) (all of it); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-6, S-8; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §11.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Pushing a tag `vX.Y.Z` on a commit CI has passed makes a GitHub Release of this
firmware: the image a running Walter updates itself with, the image for a
module out of the box, a `manifest.json` that says what each is, the signature
over it, and `SHA256SUMS`. *Run workflow* rehearses it and publishes nothing.
And from this task on, the image says inside itself what it is.

## Read first

The agent firmware's `.github/workflows/release.yml` at
`51faafb427cba41b0de9983736b6621bc155f829` (the shape: `ci-green`, build,
publish, a dry run that is the same jobs), and `spec/tools/manifestref.py`,
which the tooling here **imports rather than re-implements**.

## Deliverables

- **The descriptor** (`components/core/image/`): the 96 bytes of
  FIRMWARE-MANIFEST §4 — role `manager` (16), board `walter`, band `lte-m`,
  the version, the link version this firmware speaks (1.0) — as a `const`
  object the linker keeps and the firmware references, on a 4-byte boundary,
  its CRC-32 computed at build time. A portable builder and parser of the same
  bytes with host tests against `spec/fixtures/manifest/index.json`
  (the builder is used here; the parser and the finder are what phase 2 reads
  the radio's images with — write them now, tested against the valid and the
  invalid fixture images).
- **`tools/release/build_release.py`**: given a version, a commit and the
  build output, writes into `out/release/`:
  - `twinoak-walter-vX.Y.Z.bin` — the application image (`use: ota`, its
    `load_address` the first application slot's offset from the partition
    table, not a typed constant);
  - `twinoak-walter-vX.Y.Z-factory.bin` — bootloader, partition table, OTA
    data and application merged, flashed at `0x0` (`use: factory`). **It must
    end below NVS and the image store**: `check_partitions.py` holds that;
  - `manifest.json` (product `twinoak-walter-firmware`, one image
    `manager.walter.lte-m`), validated with `manifestref.validate` before it is
    written, with exactly one descriptor found in each file and held against
    the entry (`--require-descriptor` is on from the start here);
  - `SHA256SUMS`.
  Tests: against the spec's fixtures for the manifest's shape; a factory image
  that reaches NVS (refused); an image with no descriptor, and with two
  (refused).
- **Signing** — the same rules as the repeater firmware's release, because it
  is the same key: `tools/release/sign_manifest.py` takes the private key from
  an environment variable; the workflow reads the secret
  `RELEASE_SIGNING_KEY`; **a tag with no such secret fails**, with the
  instructions; a dry run signs with the spec's test key and is version
  `0.0.0-dryrun+<sha7>`; the tool refuses to sign a release version with the
  test key.
- **`keys/`**: `README.md` (what goes here, who makes it, that the private half
  is never in the repository) and the file the firmware's trust table will be
  generated from in phase 2 — empty today, which means *no update is
  accepted*, and that is correct.
- **`.github/workflows/release.yml`**: on a tag `v[0-9]+.[0-9]+.[0-9]+` and on
  `workflow_dispatch` with a `ref`. `ci-green` on `main`; the build in the
  pinned image with the version baked in; `build_release.py`; sign; verify the
  signature with the public key before publishing; `gh release create`, or an
  artifact in a dry run. The notes: version, commit, ESP-IDF and modem-library
  versions, the one `esptool` line for the factory image, and that a factory
  flash keeps the settings and the stored radio images (and the test that
  holds it).
- **`tools/verify.sh`** gains: the release tooling's tests, and a local dry
  run over the build it just made — manifest validated, signed with the test
  key, verified.
- **`docs/release.md`**.

## Acceptance criteria

- [ ] `tools/verify.ps1` green, with a local dry-run release in `out/release/`: `python spec/tools/manifestref.py verify <test public key> out/release/manifest.json` says *valid*, and `manifestref.py descriptor` reads role `manager` from both images.
- [ ] The host tests cover the descriptor builder, parser and finder against every entry of the spec's `index.json`.
- [ ] A *Run workflow* dry run on this task's commit produces the same set as an artifact; the report links the run.
- [ ] The tooling's tests cover the four refusals above and "signing a release version with the test key".
- [ ] The dry run's log does not contain the test key's private hex.

## Not verified here

- The tag path's `gh release create`, and a real signature: **the release key
  is Thomas's to make** (`spec/SYSTEM.md` §10, item 1).
- That the OTA image, written to the second slot by a running Walter, boots:
  phase 2's task 04 and the bench.

## Out of scope

Verifying a signature *in the firmware* (phase 2, task 01). The self-update
(phase 2, task 04).
