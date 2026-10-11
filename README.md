# TwinOak Walter firmware

The firmware for the LTE box of a
[TwinOak standard repeater v2](https://github.com/twinoak/twinoak.meshcore.standard-repeater-design-v2):
a QuickSpot Walter (ESP32-S3 with a Sequans LTE-M modem) wired to the repeater
in the box beside it by a UART, a reset line, a power-kill line and SWD.

Its job is to make sure the repeater on the mast **cannot be bricked from
afar** and can be looked after without climbing to it:

1. it **watches the repeater** and brings it back — ask again, reset,
   power-cycle, reflash a known-good image — slowly and within limits;
2. it **replaces the repeater's firmware over SWD**, application region only,
   checks that the new one is healthy, and puts the old one back if it is not;
3. it **updates itself** A/B, and goes back if the new image cannot reach both
   the repeater and the server;
4. it is the unit's **uplink**: LTE-M, HTTPS with a verified chain, a few
   minutes awake and then asleep until the battery can afford the next time;
5. it is meant to be a **MeshMaster agent** — for its own repeater over the
   UART, and as an exit node for the neighbours through the repeater's radio.
   That part waits for decisions on the MeshMaster side (`spec/SYSTEM.md` §8);
   everything up to it is in the piles here.

The plan, the decisions and the two protocols are in [`spec/`](spec/README.md)
(start with `spec/SYSTEM.md`). How this firmware is put together is
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). How it gets built is
[`tasks/`](tasks/README.md).

## State

Nothing is built yet. This repository holds the specification, the
architecture and the task piles; phase 0's first task turns it into something
that compiles.

```powershell
.\tasks\run-pile.ps1          # phases 0 to 3, one headless session per task
.\tasks\run-all.ps1 -Phase 0  # one phase
```

## What it will be, in one table

| | |
|---|---|
| Module | QuickSpot Walter: ESP32-S3, 16 MB flash, Sequans GM02SP (LTE-M / NB-IoT) |
| Built with | ESP-IDF, pinned, in Espressif's Docker image; the `walter-modem` component |
| Verified by | `tools/verify.ps1` — host tests and the firmware build, in that image; CI runs the same script |
| Tested | on the host: the link, the debug port, the nRF52's flash controller and the update procedure, against simulators |
| Flashed | at a bench: its own USB-C. In service: by itself, A/B, with rollback |
| Released as | a GitHub Release: the OTA image, a factory image, `manifest.json`, its signature, `SHA256SUMS` |

## Rails that hold in every task

- **Never a chip erase.** No automatic path erases the nRF52's whole flash or
  its UICR. A protected chip is detected and refused; opening it is a person's
  job at a bench.
- **An image goes into its region and nowhere else**, and the region is this
  firmware's own knowledge of the board — a manifest cannot widen it.
- **Nothing is flashed that is not signed**, by a key this build trusts, and
  that does not say inside itself that it is for this radio.
- **Every step of an update is journalled first.** A reset in the middle
  resumes or rolls back; it never guesses.
- **The pins have rules** (`INTERCONNECT.md`): reset is open-drain and never
  driven high; tristate before kill; two resets at least a second apart.
- **TLS is verified.** No client is ever told to accept an unverified chain.
- **Secrets stay where they are**: a token in NVS, a repeater's password in
  RAM for the length of a job; neither is printed by any command.
- **Slowly, and within limits.** Everything the supervisor does to the
  repeater has a backoff and a daily cap. A unit that power-cycles its radio
  every minute is worse than one that waits for a person.

## Licence

MIT — [`LICENSE`](LICENSE). The modem library (`dptechnics/walter-modem`) has
its own licence, which allows this use because the firmware runs on a Walter.
