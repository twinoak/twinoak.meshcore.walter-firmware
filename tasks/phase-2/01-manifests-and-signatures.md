# 01 — Manifests and signatures

**Depends on:** phase 1 (and phase 0's task 02 for the descriptor code).
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) (all of it, §5 above all); [`spec/fixtures/manifest/`](../../spec/fixtures/manifest/) with its `index.json`; [`spec/tools/manifestref.py`](../../spec/tools/manifestref.py); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-8.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The Walter can decide whether a file may be flashed. Six checks, in order —
the signature, the manifest, the right image, the file's hash, the descriptor
inside it, the region — and a file that fails any of them goes nowhere near a
radio. A build with no release key compiled in passes nothing, which is the
state this repository is in until Thomas makes the key.

## Deliverables

- **Ed25519 verification** (`components/core/image/crypto/`): one small,
  well-known implementation of RFC 8032 Ed25519 **verification only**, vendored
  at a named version with its origin, licence and hash in a header, compiled
  for the board and the host alike; and SHA-256 and SHA-512 from whichever
  source serves both builds identically (say which — if the board uses
  ESP-IDF's and the host another, a test must hold them against the same
  vectors). The report justifies the choice against the alternatives in a few
  lines (size, audit history, whether signing code comes along unused).
- **The trust table** (`keys/` → generated header): up to three public keys
  with their key ids, generated at build time from `keys/release.pub` (one hex
  key per line; empty today). The **test key** of the specification is
  included **only** in the host tests and in a build made with an explicit
  `-D` flag that also stamps the version string with `+TESTKEY` and prints a
  warning at every boot; the release tooling refuses to release such a build
  (add that check to phase 0's `build_release.py`, with a test).
- **The signature line** parsed strictly: `ed25519 <16 hex> <base64 of 64
  bytes>`, one line; anything else is "no signature". The key is chosen by id
  from the table; an unknown id fails without trying the others.
- **The manifest** (`components/core/image/Manifest`): a parser for schema 1
  into fixed-size structures — no heap, a bounded number of images and files,
  bounded string lengths, and an explicit error for a manifest that exceeds
  them — and `validate()`, the rules of FIRMWARE-MANIFEST §2 exactly as
  `manifestref.validate` applies them. JSON parsing by a small pinned library
  that builds for both targets; unknown fields ignored, an unknown `schema` or
  `product` refused.
- **Choosing the image**: given what a radio slot holds or may hold (role,
  board, band) and what this unit is (the Walter's version, the link major it
  speaks), pick the one image and its `swd` file — or say which requirement is
  not met. For the Walter's own product, the `ota` file.
- **The file**: a streaming SHA-256 (the bytes arrive in pieces, from a
  download or a USB transfer) and the size check; then the descriptor — exactly
  one, valid (phase 0's finder) — held against the manifest entry: role,
  board, band, version, link major.
- **The region**: the manifest's `app_region` must lie inside the flasher's
  board profile (phase 1, task 05) and the file inside the manifest's region.
- **`ImageVerdict`**: one function that runs the six checks in order and
  returns either "accepted: this file, for this slot, with this hash and
  descriptor" or the **number of the check that failed** and a short reason.
  It is the only way to obtain the token the image store (task 02) requires to
  accept a file — make that a type, not a convention.
- **The console**: `trust` — the key ids this build trusts, or *none: no
  update will be accepted*.

## Tests (host)

- RFC 8032's test vectors for Ed25519 (at least the first three, and the one
  in the spec's `index.json`); a signature with one bit flipped; a
  non-canonical `S` (it must be rejected — check that the vendored
  implementation does, and say so); the SHA vectors.
- Every entry of `index.json`'s `signatures`: valid ones accepted, each
  invalid one refused at check 1.
- Both fixture manifests parse and validate; the canonical bytes are not
  required (a manifest with reordered keys and different whitespace, signed
  with the test key in the test, is accepted — what is signed is the bytes).
- A manifest with: an unknown schema; an unknown product; a duplicate image
  id; a `swd` file outside its region; a region outside the board profile; a
  missing `load_address`; a name with a path separator; more images than the
  parser's bound. Each refused with its own reason.
- Choosing: the right image among several; no image for this board; a
  `requires.walter` newer than this build; a `link_major` this build does not
  speak.
- Files: the fixture images accepted against their manifests; a wrong size; a
  wrong hash; `image-two-descriptors.bin` and `image-broken-descriptor.bin`
  refused at check 5; a descriptor whose board differs from the manifest's.
- **With an empty trust table, everything is refused at check 1.**
- A property test: a valid manifest and signature with any single byte of
  either changed is refused.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The verdict's six checks map one to one onto FIRMWARE-MANIFEST §5, in order, and the report shows a refusal at each.
- [ ] No allocation in the parser or the verifier; the bounds are named constants with their reasoning.
- [ ] A normal build contains neither the test key nor any signing code (search the image for the test key's bytes; say how the absence of signing code was checked).
- [ ] The vendored crypto's origin, version, licence and hash are in the tree, and the report explains the choice.

## Not verified here

A real release key: Thomas makes it (`spec/SYSTEM.md` §10 item 1). Until
`keys/release.pub` has a line, a unit accepts no update — check on the bench
that `trust` says so.

## Out of scope

Storing the file (02). Fetching it (phase 3). Flashing it (03).
