# Phase 8 — what the first unit showed (v0.1.1)

Written on the evening of 6 November. Thomas put v0.1.0 on the first unit's Walter and tried to give
its radio repeater v0.1.0 the way `docs/field.md` says: push the candidate over the cable, then
`rak update --first-install`. Neither half worked, and three more faults came out on the way. Each one
was invisible to the host tests, because every fake did what the code expected: the console host
writes bytes verbatim, the fake clock never meets a watchdog, and the fake modem never delivers an
event late. Each task below fixes one fault **and teaches the fakes the board's behaviour**, so the
test fails before the fix.

The evidence is in [`../phase-6/FOLLOWUPS.md`](../phase-6/FOLLOWUPS.md), the items dated 6 November.
Each task here takes one of them and ticks it.

| What happened on the board | Task |
|---|---|
| `rak update --first-install`: `watchdog reset` at every boot, twelve in a row, then `STUCK: no image to return to` | 01 |
| `walterctl.py push`: `no answer to type 0x03 seq 9`, four times | 02 |
| the second HTTPS request of a wake: `did not go out`, or `the connection closed before the body was complete` | 03 |
| a job for the unit's own repeater went out on the air, because no `HELLO` had been asked | 04 |
| meshmaster.dk could not pick an image for a radio with no golden image: no band | 05 |

**Prerequisites:**

- **Link protocol 1.1** in `spec/`: the reviewer's copy of 6 November, committed with this pile by
  the driver's notes commit. `HELLO`'s answer and `EV_BOOT` end with `band`, and the fixtures are
  `spec/fixtures/link/link-v1.1.json`. 1.0's file keeps its bytes.
- **The reviewer's changes to `tools/walterctl.py`** of the same evening, in the same commit:
  `--undo-crlf` (the exact inverse of 0.1.0's conversion, with its test in `tools/test_walterctl.py`),
  `SerialPort.read` returning as soon as bytes arrive (it waited out the 5 s reply timeout for every
  chunk), and a progress line every tenth of a file. Task 02 reviews them and keeps them.
- The repeater firmware's pile 3.8 answers `HELLO` in 1.1. Task 05 does not wait for it: a 1.0
  radio stays one this Walter must talk to.

**Not on the board.** This pile runs at a desk. The unit on Thomas's bench has v0.1.0 on its Walter,
v0.1.0 on its radio (recovered over the radio's own USB with the `.uf2`, and **as a new node: its key
was not kept** — corrected 8 November by task p10-03, which found the reading in the repeater
firmware's `tasks/phase-3.9/reports/01.md`: that firmware's own `InternalFS.begin()` formatted a
torn volume and drew a new key, and from its `v0.1.2` a radio in that state refuses instead) and a
radio slot marked *stuck*. **No session opens its ports**: neither the Walter's COM port nor the radio's, and
not through `tools/bench` either. Each task adds its bench lines to `docs/bench.md`, for Thomas.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-flasher-lets-the-idle-task-run](01-the-flasher-lets-the-idle-task-run.md) | — | a blocking pause between pages (never inside an SWD transfer) in the flasher, the hash pass and `rak provision`'s base; a host test that fails on a stretch of flashing without one |
| 02 | [the-transfer-in-bytes](02-the-transfer-in-bytes.md) | — | the transfer's answers written as bytes; the terminal's LF to CR LF in one helper that the console host uses too; a push of more than ten chunks in the tests; walterctl choosing `--undo-crlf` by the Walter's own word |
| 03 | [one-connection-one-socket](03-one-connection-one-socket.md) | — | consecutive connections on alternating socket ids, events for the other one ignored; a fake modem that closes the last connection late |
| 04 | [the-radio-known-before-a-job](04-the-radio-known-before-a-job.md) | — | a wake that has no `HELLO` asks for one, and the radio's key, before it claims a job |
| 05 | [link-1-1-the-band](05-link-1-1-the-band.md) | 04 | `HELLO` asked in 1.1; the band kept, printed and reported in `twinoak.fw.state` when there is no golden image |
| 06 | [the-release-0-1-1](06-the-release-0-1-1.md) | 01–05 | `v0.1.1` published as *Latest* and verified; the guide for a unit at 0.1.0 |

```powershell
.\tasks\run-all.ps1 -Phase '8'
```

Run the repeater firmware's pile 3.8 and meshmaster.dk's pile 4.25 the same night. None of them
waits for the others.

## Models

- **Opus**: all six.

## In the morning (Thomas)

1. The unit's Walter: flash the v0.1.1 **release** build by cable (`docs/flashing.md`). A factory
   flash keeps `nvs`, `journal` and `rakfw`, so the stored candidate and the *stuck* mark survive.
2. `rak update status` and `image bad` first: the failed first install of 6 November put the stored
   v0.1.0 candidate on the bad list before it entered *stuck* (`RadioUpdate::fail`), so it is never
   flashed again (corrected 7 November by the review; `docs/flashing.md` has it right). Push the
   repeater's **v0.1.1** as the candidate (no `--undo-crlf`: a 0.1.1 Walter says protocol 2), then
   `rak hello` and `rak update --first-install`. It proves the image and makes it golden, which ends
   the stuck mark.
3. `lte up`, then `http get https://meshmaster.dk/` twice in a row, then `agent wake`: config,
   heartbeat, claim and observations all `done`. Note every request's time: about 250 ms extra on the
   second request, or on every request from the third on, is review finding S2 below.
4. On meshmaster.dk: *Read the unit's state*. With a golden image, or a 1.1 radio, the slot reports
   its band and *Mark canary* works.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
