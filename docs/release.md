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

The notes a release is published with (`NOTES.md`) are two parts: the hand-written
[`release-notes/vX.Y.Z.md`](../release-notes/README.md) of this repository — what this release is,
what has and has not been on a board, which repeater-firmware release it goes with — and then the
generated part: the version, the commit, the ESP-IDF and modem-library versions, the one `esptool`
line, and what a factory flash does and does not touch.

## The two things a tag needs

**The release key**, which exists: Thomas made it on 20 October 2026, key id `73efdd383a957596`,
and the same key signs the repeater firmware's releases (`spec/SYSTEM.md` §10 item 1). Its public
half is one line of [`keys/release-keys.txt`](../keys/README.md) in both repositories; its private
half is the repository secret **`RELEASE_SIGNING_KEY`** in both, and lives nowhere else. Were it
gone, a tag would **fail on purpose**, before anything is published:
`tools/release/sign_manifest.py` stops at a missing secret and
`tools/release/verify_release.py` at a trust table that names no key, each printing the
instructions for making one (`python spec/tools/manifestref.py keygen`). A unit with no key
compiled in accepts no update at all, which is the same safe state seen from the other end
(`spec/FIRMWARE-MANIFEST.md` §3).

**The hand-written notes**, `release-notes/vX.Y.Z.md`: `tools/release/build_release.py` puts that
file in front of the generated notes and **refuses a release that has none** — a release nobody
wrote a word about is not a release. Write it on the branch that is tagged;
[`release-notes/README.md`](../release-notes/README.md) says what belongs in one and what the
generated part already says.

A dry run needs neither: it is version `0.0.0-dryrun+<sha7>`, it signs and verifies with the key
pair the specification publishes on purpose, its body is the generated part alone, and it
publishes nothing.

## Releasing

1. `main` is at the commit you want. Its CI run does not have to have finished — the first job
   below waits up to 80 minutes for it — but it has to end **green**, so there is nothing to gain
   by tagging before you would have looked.
2. `git tag v0.3.1 <commit> && git push origin v0.3.1`.
3. `.github/workflows/release.yml` takes it from there:

   | Job | What it does, and what stops it |
   |---|---|
   | `ci is green for this commit` | the commit must be one of `main`'s, and `ci.yml` must have completed **successfully** for it — waited for, up to 80 minutes, because that run is 25 to 45. A red CI releases nothing, and so does one that never finishes |
   | `build X.Y.Z+<sha7>` | `tools/verify.ps1 -Version X.Y.Z+<sha7>` — the whole verification, in the pinned image, at the version being released, so that what is published is what was verified. The images become the run's artifact |
   | `publish the release` | `build_release.py` (the files, the manifest, the refusals below), **the boot gate** (`boot_gate.py`: the factory image is started in QEMU, below), `sign_manifest.py` (the secret), `verify_release.py` (every check of §5 that applies, with the keys a firmware build would trust), then `gh release create` |

4. The version the firmware reports is `X.Y.Z+<sha7>`, not the tag alone: `PROJECT_VER` at build
   time, which is what the console prints, what the 96 bytes inside the image say (§4) and what
   the manifest's `version` is. The three cannot disagree — `build_release.py` holds them to each
   other and refuses if they differ.

## Does it start? The boot gate

**On 9 November 2026 `v0.1.3` was published and could not start.** It was built in the pinned
image, passed every host test, fitted its slot, carried its descriptor, was signed with the
release key and verified against every check of `spec/FIRMWARE-MANIFEST.md` §5 — and ESP-IDF could
not create its main task, so every boot of it ended in
`assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)` before a line of this
firmware ran. A unit on a mast found out: it fetched the release over LTE, switched to it, and the
bootloader brought `v0.1.2` back by itself. Nothing in this repository had ever asked an image to
*start* ([`ARCHITECTURE.md`](ARCHITECTURE.md) §14 is the whole account).

Two things ask now. [`tools/heap_at_start.py`](../tools/heap_at_start.py) refuses a build whose
main task would not fit, from its map, its DWARF and its `sdkconfig`, on every build
(rail **R22**). And **the release's factory image is booted**, by
[`tools/release/boot_gate.py`](../tools/release/boot_gate.py), in the QEMU the pinned ESP-IDF
image ships — the same emulator tasks p11-01 and p11-02 read the heap out of this firmware in.
It runs in the publish job **before the manifest is signed**, and in `tools/verify.sh`'s `release`
step on a laptop and in CI, over the file `out/release/` holds (rail **R23**). It refuses unless,
within 90 seconds:

- the image **reached `app_main`** — which is exactly what `v0.1.3` could not do;
- the **banner names the version being released**, so a build stamped with another version cannot
  be published under this one;
- `app_main`'s **`heap:` line was printed, with the main task's stack taken** (task p11-02): the
  board's own account of the heap, read off the console a second into the boot.

It reads those three through **gdb**, not through a serial port: this firmware's console is the
ESP32-S3's USB Serial/JTAG peripheral, QEMU's `esp32s3` machine does not have that peripheral, and
ESP-IDF drops every console byte when no USB host is connected. So the gate breaks where the bytes
are handed over and reads them there — and breaks on `esp_system_abort` as well, which is how a
refusal can name the panic and the frame it came from. The script's own docstring says how.

**What it proves:** this file, flashed at `0x0`, brings up the ESP32-S3, the bootloader, ESP-IDF's
start-up, the main task and this firmware's own first lines on the chip model the Walter has.

**What it does not prove.** QEMU has no modem, no radio, no SIM and no server: the gate ends two
lines into the boot and asks nothing of them. **The first unit over LTE is still the first time
the radio, the modem and the trial run on a release** — and that is deliberately not a release
step, because a dry run is signed with the specification's test key and a unit accepts only
`73efdd383a957596`, so the switch from the release before cannot be walked before publishing.
What catches that half is the trial: an image that does not reach its main loop, HELLO its radio
and reach the backend is rolled back by the bootloader, and **from the release after `0.1.4`
the unit says why** — the reset reason and the core dump's panic line, in `self status`, in the
apply's result and in `twinoak.fw.state` ([`update.md`](update.md), task p11-03).

## Rehearsing

Two ways, and they run the same code.

**Locally, on every verified commit.** `tools/verify.ps1`'s `release` step makes the whole thing
in `out/release/` from the build it just made, **boots its factory image** (*The boot gate*
above, about a second), signs it with the specification's test key and takes it through
`verify_release.py`. So the tag path is never run for the first time on a tag
(`spec/SYSTEM.md` S-10). A step on its own:

```powershell
.\tools\verify.ps1 -Only build
.\tools\verify.ps1 -Only release
.\tools\verify.ps1 -Version 0.3.1+1a2b3c4     # everything, at the version a release would use
```

At a release's own version — `X.Y.Z+<sha7>`, the last line above and what the workflow's build job
passes — that step makes the files, boots the factory image and runs every refusal over them (the
hand-written notes among them) but **does not sign**: the test key may not sign a release version, which is a rail, and the
release key is a secret of the workflow. So the signature and the `verify_release.py` gate happen
once, in the publish job, immediately before `gh release create`. The step says so in its log.

**In the workflow.** *Actions → release → Run workflow*, with `ref` set to the commit to rehearse
on (empty for the workflow's own ref). The same three jobs run; the version is
`0.0.0-dryrun+<sha7>`, the files are named `v0.0.0-dryrun`, the factory image is booted in QEMU
and its transcript is in the run's summary, the manifest is signed with the test key, and
**nothing is published** — the files are the run's artifact. A dry run reports what CI
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
- **a release with no hand-written notes**: no `release-notes/vX.Y.Z.md`, or an empty one. It is
  the one refusal that is about a person rather than about bytes, and it is here for the same
  reason as the rest — a release that nobody explained is one nobody can decide whether to flash.
- **an image that does not start** (task p11-04, *The boot gate* above): the factory image is
  booted in the pinned image's QEMU before the manifest is signed, and it must reach `app_main`,
  print the banner with the version being released in it and print its `heap:` line. `v0.1.3` is
  what this refusal is made of.
- **a test-key build** (task p2-01): a build made with `-DTWINOAK_TRUST_TEST_KEY=1` trusts the
  specification's public test key, so it would accept anybody's firmware. `build_release.py`
  refuses it twice over — by the `+TESTKEY` its version carries, and by the key's own 32 bytes
  found in either image — and `tools/verify.ps1 -TestKey` ends by showing that refusal.

## The files in this repository

| | |
|---|---|
| `tools/release/build_release.py` | the files, the manifest and `SHA256SUMS`; the refusals above. Imports `spec/tools/manifestref.py` and `tools/check_partitions.py` rather than re-implementing either |
| `tools/release/boot_gate.py` | the boot gate: the factory image started in the pinned image's QEMU, through gdb, before anything is signed (*The boot gate* above). Run by `tools/verify.sh`'s `release` step and by the publish job |
| `tools/release/sign_manifest.py` | the signature. The private key comes from `RELEASE_SIGNING_KEY` and from nowhere else — there is no `--key` argument and no key file |
| `tools/release/verify_release.py` | the gate before publishing: §5, with the trusted keys |
| `tools/release/test_release.py` | their tests (`tools/verify.ps1 -Only release-tests`) |
| `keys/` | the public halves: one key, `73efdd383a957596`, since 20 October 2026 |
| `release-notes/` | the hand-written top of each release's body, one file per tag, and what belongs in one |
| `components/core/image/` | the descriptor: the builder that puts it in this image, and the parser and finder phase 2 reads a radio's images with. Host tests against the spec's fixtures |

## The same six checks inside the firmware

From phase 2, task 01, the Walter makes the checks of `spec/FIRMWARE-MANIFEST.md` §5 itself
(`components/core/image/`, `docs/ARCHITECTURE.md` §6): the signature against the trust table
compiled in from `keys/release-keys.txt` — one key, so a manifest signed by anything else is
refused at check 1 and `trust` on the console says which key is accepted — then the manifest, the
image for the slot, the file's hash,
the descriptor inside it, and the region. `tools/release/verify_release.py` remains the gate on
the release side; the two are written from the same section and tested against the same
fixtures.

## Not in here yet

- **The Walter updating itself** with the `ota` image — phase 2, task 04, and the bench.
