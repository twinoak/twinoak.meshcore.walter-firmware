# 01 — The base in the store

**Depends on:** phase 3; `spec/` copied with S-5a and `FIRMWARE-MANIFEST.md` §4a.
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §2 (the `provision` use, `format` `hex`, the rules after the signature), §4a (the base's seven rules, in order), §5 (check 5p), §6 (the new fixtures); `spec/tools/manifestref.py` `validate`, `parse_ihex`, `validate_provision` (the same rules as code, and the words each refusal says); `spec/fixtures/manifest/index.json` → `provision`, `manifest-repeater-base.json` and its `.sig`, `base-*.hex`; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5a, §5 (*images*); this repository's `components/core/image/` (`manifest.cpp` `kUseNames`/`validate`, `verdict.h` `ImageVerdict`/`ExpectedFile`/`AcceptedImage`/`Check`, `store.h` `Which`, `file_check`), `components/core/include/core/flasher/board_profile.h` (`kRak4630Single`), `tools/walterctl.py` (`push`, `WHICH`), `docs/ARCHITECTURE.md` §6–§7 (the image store and its doors), `docs/console.md` (`image …`).
**Size:** one session.
**Model:** claude-opus-5

## Goal

A release of the repeater firmware now carries, for each image, the board's base: an Intel HEX file
with the MBR, the SoftDevice, the bootloader and the UICR words they need. This task makes the
Walter read that entry, judge the file by the rules of §4a against its own board table, and keep
it — one per radio slot, beside golden, candidate and scanner — where only task 03's command can
take it out. It writes nothing to a radio.

## Deliverables

- **The manifest** (`image/manifest.cpp`): `provision` joins the uses; `format` must be `hex`;
  `load_address` must be absent; the image's `mcu` must be `nrf52840` and it must have an
  `app_region`; at most one `provision` file in an image. The refusal reasons are
  `manifestref.validate`'s, one `ValidateReason` each, with the reference's words in the comment
  beside it. An existing manifest without a `provision` file is accepted exactly as before (the
  phase 2 fixtures, unchanged, pass unchanged).
- **The base reader** (`image/base.{h,cpp}`, no ESP-IDF include): §4a's seven rules over the file's
  bytes **as a stream** — the store's file reader in chunks, never the whole file in RAM (a real
  base is ~450 KB of text). Two passes are fine (the seventh rule needs `NRFFW[0]`, which may come
  late in the file); say in the report what each pass keeps and how many bytes of RAM that is. It
  returns either the rule that failed, as a reason whose words are `index.json`'s `why`, or a
  summary: the bootloader's address, the flash byte count, the UICR words present, `NRFFW[1]` if
  present — the same fields as `manifestref.validate_provision` returns. The addresses §4a names
  (the SoftDevice's information structure, `NRFFW[0]`, `UICR.APPROTECT`, UICR's bounds
  `0x10001000`–`0x10001307`) are constants of `image/base.h`, cited to §4a — **never** `nrf52/regs.h`'s
  `uicr::` names, which rail R3 allows in `regs.h` alone: a base is a file being judged, not a register
  being written. It also offers what task
  03 needs to write the base without parsing twice in the command: an iterator over the file's data
  as `(address, word)` runs in address order, flash first, then UICR, with partial words padded with
  `0xFF` (a word the file gives none of is never written; a word it gives part of is written with
  the rest erased — say so in the header with the reason: erased flash reads `0xFF`, and NVMC can
  only clear bits).
- **Check 5p** (`image/verdict`): for a `provision` file, after checks 1–4 (signature, manifest,
  image for this radio position, size and SHA-256), the image's `app_region` must lie inside the
  region this firmware's board table gives the board (`kRak4630Single`: `0x26000`–`0xED000`), as
  check 6 holds a `swd` file, and the file must pass the base reader against **the board table's**
  region, not the manifest's — the stricter of the two (§5, 5p). Checks 5 and 6 are not run for it
  (it carries no descriptor). The token it
  yields is a new kind (`AcceptedBase`), not an `AcceptedImage`: a base can never be passed where an
  image is wanted, and the reverse — make the types say so, so a mistake does not compile.
- **The store** (`image/store`): `Which::kBase`, one per slot. Its write door takes only an
  `AcceptedBase`; a promotion never moves a pointer to or from it; `image erase base` removes it;
  the bad list does not apply (a base is not health-checked; S-5a's read-back is its check). The
  partition has room (state the arithmetic: three images and a base per slot, the largest real
  sizes known — the repeater's `swd` file from its release, ~450 KB for a base). A power cut in the
  middle of storing a base leaves no base, as for an image (reuse the store's record order; add the
  power-loss case to its tests).
- **The console and the cable**: `image list` shows the base (hash, size, bootloader address, the
  SoftDevice line); `image verify base` re-runs 5p over the stored bytes; `image receive` and
  `walterctl.py push --as base <release-dir>` take the `provision` file of the image for this
  slot's board (one file: the `--image` option picks among images as it does today). Docs:
  `docs/console.md`, `docs/ARCHITECTURE.md` §6–§7 (one paragraph and the store's table), and
  `tools/test_walterctl.py`.
- **Tests (host)**, failure cases first: every `base-*.hex` of `spec/fixtures/manifest/` refused with
  the reason `index.json` gives, word for word; `base-rak4630.hex` accepted with `index.json`'s
  summary; the run iterator over it equals a reference walk of the same file (write the reference
  in the test from the fixture's records, not from the code under test); `manifest-repeater-base.json`
  passes 1–4 and 5p with the test key (its example region, `0x26000`–`0xD4000`, lies inside the
  board's; the base is judged against the board's); the same manifest with its image's
  `app_region` widened past the board's is refused; a `provision` file with a `load_address`, or with
  `format` `bin`, or two in one image, refused; an `AcceptedBase` cannot be stored as golden (a
  compile-time test, the same way the store's doors are proven today); power loss at every step of
  storing a base.

## Acceptance criteria

- [ ] Every base fixture is accepted or refused exactly as `index.json` says, with its words (test).
- [ ] A base and an image cannot be mistaken for each other at compile time (test), and a base can be stored, listed, verified and erased, never promoted (tests).
- [ ] The phase 2 manifest and image tests pass unchanged.
- [ ] `walterctl.py push --as base` works against `console_host` (its own test).
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, §8 *Images*: `walterctl.py push --as base` of a real release over the cable;
`image list` shows the base with bootloader `0x000F4000` and *S140 6.1.1*; `image verify base` passes
after a power cycle.

## Out of scope

Writing it to a radio (03), and the door that allows that (02). The release that carries the base
(the repeater firmware's pile 3.4, task 01).
