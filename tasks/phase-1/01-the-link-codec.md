# 01 — The link codec

**Depends on:** phase 0.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §2–§8, §11; [`spec/fixtures/link/link-v1.json`](../../spec/fixtures/link/link-v1.json); [`spec/tools/linkref.py`](../../spec/tools/linkref.py).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The box link as bytes, from the Walter's end: the checksum, the framing,
packets, and one encode or decode per message of protocol 1.0 — portable, with
no heap, held against the specification's fixtures. Nothing here touches a
UART; task 02 puts it on the wire.

The repeater firmware implements the same protocol from the other end, against
the same fixture file. The two codecs never see each other's source, and that
is the point: if both pass every case, they agree.

## Deliverables

All under `components/core/link/codec/`.

- **CRC-16/CCITT-FALSE** and **COBS** encode and decode for buffers up to a
  packet (244 bytes). A decoder that meets a zero, a block that runs past the
  end, or the `0xFF` code refuses the frame.
- **Packet** build and parse; **`FrameReceiver`** (bytes in any chunking →
  packets; counts what it drops; a 246-byte buffer; empty frames are not
  errors) and **`FrameWriter`** (with the leading `0x00` after 100 ms of
  idle).
- **Messages**: a plain struct per payload, and the function this end needs —
  **encode for every request, decode for every response and every event** —
  including the shared pieces (`str8`, the path, the reply record, the
  observation record). Decoders check every length and legal value and say
  *malformed* instead of reading past the end; a response whose status is not
  `OK` is the one byte and nothing more; trailing bytes they do not know are
  ignored. The numbers — types, statuses, roles, capability bits, modes,
  attention reasons — are one header that reads like the specification's
  tables.
- A `Reader` and `Writer` over a byte span for the little-endian scalars.

## Tests (host)

Reading `spec/fixtures/link/link-v1.json` at run time:

- every `crc16` and `cobs` vector, both directions;
- every case of `kind` `request`: encoding `fields` gives `payload`, and the
  frame built from `(wire_type, seq, payload)` equals `frame`;
- every case of `kind` `response` or `event`: decoding `payload` gives
  `fields`;
- every `invalid` frame refused and counted;
- the `stream`, fed whole and one byte at a time;
- every response and event payload truncated by one byte, and by all but one:
  *malformed*, never a read out of bounds (the sanitizers are the proof);
- a test that fails when the fixture file gains a case the tests do not cover.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] Every case of the fixture file is exercised; the coverage test proves it.
- [ ] No allocation under `components/core/link/codec/` (a grep the host tests run).

## Not verified here

Nothing: bytes in, bytes out.

## Out of scope

Sending anything (02).
