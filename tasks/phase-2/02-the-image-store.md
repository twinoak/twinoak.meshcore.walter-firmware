# 02 — The image store

**Depends on:** 01.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §6, §11; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5; [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §5.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The Walter keeps the radio's firmware in its own flash: the **golden** image —
the last one that proved itself — a **candidate** waiting to be tried, and a
**scanner** image for the day a site needs NoiseScope. Each is either wholly
there, with its hash and its description, or not there at all; a power cut
while one is being written cannot leave something that looks like an image and
is not. And because there is no server yet, a laptop can load one over USB.

## Deliverables

- **`components/hal/`**: a flash-region interface — read, write, erase in
  sectors, the sector size — and its ESP-IDF implementation over the `rakfw`
  partition of phase 0's table.
- **`components/core/image/Store`**:
  - **Layout**: per radio slot, three image slots (*golden*, *candidate*,
    *scanner*), each large enough for a radio image (the board profile's
    region size, rounded up) plus a header sector. The store is laid out for
    **two** radio slots from the start (phase 1, task 06), so a dual unit does
    not need the partition rewritten. The arithmetic is checked at build time
    against the partition's size.
  - **A record**: magic, a format version, the image's size, its SHA-256, its
    load address, a copy of its 96-byte descriptor, the manifest version and
    commit it came from, when it was stored, and a CRC over the header. The
    record is made valid by **one final small write**, after the image's bytes
    are in place and have been read back against the hash; it is invalidated
    by one small write before anything is overwritten. State what happens for
    a power loss on each side of each of those writes.
  - **Writing** only with task 01's acceptance token — there is no way to
    store bytes that did not pass the six checks — streamed in pieces, with a
    running hash, resumable after an interruption when the token says the same
    hash (a download that broke at 60 % continues, it does not restart).
  - **Reading**: a slot's record or "empty"; the image's bytes as a stream for
    the flasher, re-hashed as they are read so that flash rot is noticed before
    it reaches a radio.
  - **Promotion**: *candidate becomes golden* without copying megabytes where
    a pointer can move — a small index record, itself written atomically (two
    copies with sequence numbers, the newer valid one wins). The image that
    was golden is kept until the new one is in place. **There is never a
    moment with no golden image once there has been one.**
  - **The bad list**: hashes of candidates that failed their health check, a
    short bounded list; an image on it is refused at the door.
- **Loading over USB** (the bench's stand-in for a server): console command
  `image receive <golden|candidate|scanner> [@slot]` switches the console to a
  framed binary transfer — reuse the link's framing (COBS, CRC-16) for chunks
  with a sequence number and an acknowledgement; no new framing code — taking
  first `manifest.json`, then its `.sig`, then the file. The six checks run on
  the Walter; the laptop is not trusted. And **`tools/walterctl.py`** on the
  host: `walterctl.py push --port COM7 --as candidate <release-dir> [--image
  <id>]`, using `spec/tools/linkref.py` for the framing and `pyserial`.
  Loading *golden* directly is allowed only when the slot's golden is empty
  (a unit's first provisioning); afterwards golden is reached only through an
  update that passed.
- **The console**: `image list` (per slot: role, board, version, size, hash
  prefix, state, when), `image verify <which>` (re-hash), `image erase
  <candidate|scanner>` (never golden), `image bad` (the list).
- **`host_test/sim/SimFlash`**: a flash region that only clears bits, erases
  by sector, counts erases per sector, and can **lose power at the Nth write
  or erase** leaving that operation half done. Reused by the journal of task
  03.

## Tests (host)

- Store, read back, verify; a second image over the first.
- **Power loss at every write and erase of a store operation, in a loop**:
  afterwards the slot holds the old valid record or none — never a record that
  validates over bytes that do not match — and the operation can be run again
  to completion.
- The same loop over a promotion: at every point there is exactly one golden
  image, the old or the new.
- Resume: half an image, a "restart", the rest; a resume with a different hash
  starts over.
- A flipped bit in a stored image is caught by `verify` and by the read
  stream.
- No token, no store (a compile-time property — say how it is enforced); an
  image on the bad list refused.
- The transfer protocol against a fake console: a lost chunk, a duplicated
  chunk, a reordered one, a manifest too large, a file whose hash does not
  match its manifest (refused at check 4 with nothing stored).
- Wear: storing a thousand images cycles the sectors evenly enough — state the
  erases per sector per stored image and what that means in years.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] Both power-loss loops cover every flash operation; the report gives the counts.
- [ ] The layout arithmetic for two radio slots is in the report, with the partition's size and what is left.
- [ ] `walterctl.py push` completes against the host build's console behind a pipe (so the tool is tested before a board exists), using the spec's fixture release and the test key in a test build.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "6. Images":

- A test-key build of this firmware and a repeater release signed with the
  test key (its dry run): `walterctl.py push --as golden`, `image list` shows
  it; `image verify golden` passes.
- A normal build refuses the same push at check 1, and `trust` says why.
- Pull the Walter's power during a push: afterwards `image list` shows the slot
  empty or the old image, and the push can be repeated.

## Out of scope

Flashing from the store (03). Downloading (phase 3).
