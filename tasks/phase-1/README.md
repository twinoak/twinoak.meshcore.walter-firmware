# Phase 1 — hands on the radio

Six tasks that give the Walter its two ways of reaching the radio in the other
box: the UART link, to talk to firmware that is running, and SWD, to replace
firmware that is not. At the end of this phase the Walter can — in simulation,
exhaustively, and on the bench when the rig exists — identify an nRF52840,
write an application image into its flash without touching anything else, and
talk to the repeater that image contains.

No board is needed for any of it. The link is tested against a scripted radio
built from the specification's fixtures; the debug port, the chip and the
flasher against a simulator this phase builds, down to the bit on the wire.
That simulator is the phase's second product. It is only as true as the
documents it is written from — Arm's ADIv5 for the debug port, Nordic's product
specification for the nRF52840 — so every register, bit and sequence in it
carries a citation, and where a document and a habit disagree, the document
wins.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-link-codec](01-the-link-codec.md) | phase 0 | protocol 1.0 as bytes, from this end, against the spec's fixtures |
| 02 | [the-link-client](02-the-link-client.md) | 01 | requests with retries, events, `boot_id`, telling a repeater from NoiseScope; `rak hello` on the console |
| 03 | [the-swd-wire](03-the-swd-wire.md) | phase 0 | the debug port's wire protocol and DP, against a wire-level simulated target |
| 04 | [the-nrf52-target](04-the-nrf52-target.md) | 03 | memory access, identification, access-port protection, the flash controller, core control — and the simulated chip |
| 05 | [the-flasher](05-the-flasher.md) | 04 | an image into its region and nowhere else; verify; the bootloader's settings; power loss survived |
| 06 | [the-slot-seam](06-the-slot-seam.md) | 02, 05 | every radio-facing call takes a slot; the dual adapter's selector, simulated and switched off |

01 → 02 → 03 → 04 → 05 → 06.

```powershell
.\tasks\run-all.ps1 -Phase 1
```

## Models

- **Fable** — 03, 04, 05. A debug port that misreads an ACK, a flash routine
  that erases the wrong page: these are the mistakes the unit exists to make
  impossible.
- **Opus** — 01, 02, 06.

## The exit criterion

On the bench rig: `rak hello` answers; `rak id` reads the chip's part and
variant over SWD through the filtered lines; `rak flash` writes a repeater
image that then boots, keeps its identity, and answers `rak hello` with the
version that was flashed. Thomas's — every task adds its lines to
`docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
