# Releasing this firmware

A release of the Walter firmware is five files in a GitHub Release, and one person's decision to
push a tag. Nothing else releases anything: merging a pull request does not, and neither does a
green CI run.

| File | What it is |
|---|---|
| `twinoak-walter-vX.Y.Z.bin` | the application image — what a Walter in service downloads and writes into its other application slot (`use: ota`, load address `0x00010000`, the first slot's offset from `partitions.csv`) |
| `twinoak-walter-vX.Y.Z-factory.bin` | bootloader, partition table, OTA data and application as one file, flashed at `0x0` on a module out of the box (`use: factory`) |
| `manifest.json` | what each of them is: `spec/FIRMWARE-MANIFEST.md` §2, one image `manager.walter.lte-m` |
| `manifest.json.sig` | one Ed25519 signature over the manifest's bytes (§3). Because the manifest carries each file's SHA-256, one signature covers the whole release |
| `SHA256SUMS` | for `sha256sum -c`, over the two images and the manifest |

The release notes are generated with them (`NOTES.md`): the version, the commit, the ESP-IDF and
modem-library versions, the one `esptool` line, and what a factory flash does and does not touch.

## The one thing that is missing

**The release key does not exist yet.** It is Thomas's to make, once, and the same key signs the
repeater firmware's releases (`spec/SYSTEM.md` §10 item 1):

```powershell
python spec/tools/manifestref.py keygen
```

- the `private` line becomes the repository secret **`RELEASE_SIGNING_KEY`**, here and in
  `twinoak.meshcore.repeater-firmware`, and lives nowhere else;
- the `public` line and the key id become one line of [`keys/release-keys.txt`](../keys/README.md)
  in both repositories.

Until then a tag **fails on purpose**, before anything is published:
`tools/release/sign_manifest.py` stops at the missing secret and
`tools/release/verify_release.py` stops at the empty trust table, each printing those
instructions. A unit with no key compiled in accepts no update at all, which is the same safe
state seen from the other end (`spec/FIRMWARE-MANIFEST.md` §3). A dry run is unaffected: it signs
and verifies with the key pair the specification publishes on purpose.

## Releasing

1. `main` is at the commit you want, and its CI run is green.
2. `git tag v0.3.1 <commit> && git push origin v0.3.1`.
3. `.github/workflows/release.yml` takes it from there:

   | Job | What it does, and what stops it |
   |---|---|
   | `ci is green for this commit` | the commit must be one of `main`'s, and `ci.yml` must have completed **successfully** for it. A red or unknown CI releases nothing |
   | `build X.Y.Z+<sha7>` | `tools/verify.ps1 -Version X.Y.Z+<sha7>` — the whole verification, in the pinned image, at the version being released, so that what is published is what was verified. The images become the run's artifact |
   | `publish the release` | `build_release.py` (the files, the manifest, the refusals below), `sign_manifest.py` (the secret), `verify_release.py` (every check of §5 that applies, with the keys a firmware build would trust), then `gh release create` |

4. The version the firmware reports is `X.Y.Z+<sha7>`, not the tag alone: `PROJECT_VER` at build
   time, which is what the console prints, what the 96 bytes inside the image say (§4) and what
   the manifest's `version` is. The three cannot disagree — `build_release.py` holds them to each
   other and refuses if they differ.

## Rehearsing

Two ways, and they run the same code.

**Locally, on every verified commit.** `tools/verify.ps1`'s `release` step makes the whole thing
in `out/release/` from the build it just made, signs it with the specification's test key and
takes it through `verify_release.py`. So the tag path is never run for the first time on a tag
(`spec/SYSTEM.md` S-10). A step on its own:

```powershell
.\tools\verify.ps1 -Only build
.\tools\verify.ps1 -Only release
.\tools\verify.ps1 -Version 0.3.1+1a2b3c4     # everything, at the version a release would use
```

**In the workflow.** *Actions → release → Run workflow*, with `ref` set to the commit to rehearse
on (empty for the workflow's own ref). The same three jobs run; the version is
`0.0.0-dryrun+<sha7>`, the files are named `v0.0.0-dryrun`, the manifest is signed with the test
key, and **nothing is published** — the files are the run's artifact. A dry run reports what CI
says about the commit and does not wait for it, because it releases nothing.

## What the tooling refuses

Each of these is a test in `tools/release/test_release.py`, and each is a thing that, published
once, would cost a unit on a mast:

- **a factory image that reaches `nvs`, `journal`, `rakfw` or `coredump`.** A factory image is
  flashed at `0x0` and ends where the application ends; everything a unit must keep — its
  settings, the update journal, the stored radio images — is above the two application slots.
  `tools/check_partitions.py` owns that rule on every build, and the release refuses to publish a
  file that breaks it. This is why the release notes can promise that a factory flash keeps a
  unit's settings and its radio images: the file exists only because the check passed.
- **a file with no descriptor inside it, or with two** (§4). None and the file does not say what
  it is; two and it cannot be believed.
- **a descriptor that disagrees with the manifest entry above it** — role, board, band, version,
  slot or link version (§5 check 5, applied where the release is made as well as where it is used).
- **a factory image that does not carry the very application beside it**, byte for byte at the
  slot offset. One build makes both files; a pair that does not match came from two.
- **an application whose `esp_app_desc_t` names another version or another project.** ESP-IDF
  writes that structure and our build writes the descriptor; both come from `PROJECT_VER`.
- **a manifest that does not validate**, or is not in the canonical form §2 describes.
- **signing a release version with the specification's test key**, and signing with no key in the
  environment — and verifying a release with a trust table that names no key, or one that names
  the test key.
- **a test-key build** (task p2-01): a build made with `-DTWINOAK_TRUST_TEST_KEY=1` trusts the
  specification's public test key, so it would accept anybody's firmware. `build_release.py`
  refuses it twice over — by the `+TESTKEY` its version carries, and by the key's own 32 bytes
  found in either image — and `tools/verify.ps1 -TestKey` ends by showing that refusal.

## The files in this repository

| | |
|---|---|
| `tools/release/build_release.py` | the files, the manifest and `SHA256SUMS`; the refusals above. Imports `spec/tools/manifestref.py` and `tools/check_partitions.py` rather than re-implementing either |
| `tools/release/sign_manifest.py` | the signature. The private key comes from `RELEASE_SIGNING_KEY` and from nowhere else — there is no `--key` argument and no key file |
| `tools/release/verify_release.py` | the gate before publishing: §5, with the trusted keys |
| `tools/release/test_release.py` | their tests (`tools/verify.ps1 -Only release-tests`) |
| `keys/` | the public halves, and why the table is empty today |
| `components/core/image/` | the descriptor: the builder that puts it in this image, and the parser and finder phase 2 reads a radio's images with. Host tests against the spec's fixtures |

## The same six checks inside the firmware

From phase 2, task 01, the Walter makes the checks of `spec/FIRMWARE-MANIFEST.md` §5 itself
(`components/core/image/`, `docs/ARCHITECTURE.md` §6): the signature against the trust table
compiled in from `keys/release-keys.txt` — empty today, so every manifest is refused at check 1
and `trust` on the console says so — then the manifest, the image for the slot, the file's hash,
the descriptor inside it, and the region. `tools/release/verify_release.py` remains the gate on
the release side; the two are written from the same section and tested against the same
fixtures.

## Not in here yet

- **The Walter updating itself** with the `ota` image — phase 2, task 04, and the bench.
