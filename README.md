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

## Building it

[Docker](https://docs.docker.com/get-docker/) and PowerShell (Windows PowerShell 5.1, or
`pwsh` on Linux and macOS) — and nothing of ESP-IDF on the machine. The firmware is built and
tested inside Espressif's `espressif/idf` image at one tag and digest
([`tools/idf-image.txt`](tools/idf-image.txt)), the same image CI uses:

```powershell
.\tools\verify.ps1                # everything: the spec copy, the host tests, the release
                                  # tooling, the firmware, the partition table, a dry-run release
.\tools\verify.ps1 -Only build    # one step (spec, spec-tools, host-tests, release-tests,
                                  # build, partitions, release)
.\tools\verify.ps1 -Clean         # a cold run: throw the build and cache volumes away first
.\tools\verify.ps1 -Shell         # a bash inside the same container, same mounts
.\tools\verify.ps1 -Version 0.3.1+1a2b3c4
                                  # everything, at the version a release would be built with
```

The first run pulls a 5 GB image and builds everything (minutes); the second is seconds, because
the build directory and `ccache` live in Docker volumes rather than on the bind-mounted checkout.
Each step logs to `out/verify/<step>.log` and the images land in `out/images/`:
`twinoak-walter.bin` (the application, what an update carries), `twinoak-walter-factory.bin`
(bootloader, table, OTA data and application as one file from `0x0`), the `.elf` and `.map`.
`out/release/` holds a **dry run of the release** over that build — the two images named after the
version, `manifest.json`, a signature by the specification's public test key and `SHA256SUMS` — so
the release path is exercised on every verified commit and is never run for the first time on a
tag ([`docs/release.md`](docs/release.md)).

**Flashing a Walter from the host**, over the module's USB-C, which is the ESP32-S3's own USB
(`pip install esptool`; hold BOOT while plugging in if the port does not appear):

```
esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 out/images/twinoak-walter-factory.bin
```

Then open the port (any baud; USB Serial/JTAG ignores it): the console prints
`twinoak-walter 0.0.0-dev`, which slot it runs from, what the descriptor inside the image says it
is, whether the radio's UART came up, why it booted, and a `>` prompt — `help`, `status`, `pins`,
`rak reset`, `rak kill <seconds>`, `reboots`, `reboot`, `trust` (the release keys this build
accepts an update signed with — none, until the key exists), and the radio over the box link:
`rak hello`, `rak ping`, `rak status`, `rak id`, `rak time`, `rak power`, `rak env`,
`rak cli <line>`, `rak events`, `rak text` ([`docs/console.md`](docs/console.md)). Before any of
that, the seven pins it shares with the radio are in their safe state (`docs/ARCHITECTURE.md` §2).

**Releasing it** is a person pushing a tag `vX.Y.Z`:
[`docs/release.md`](docs/release.md). Nothing else releases anything — not a merge, not a green
CI run — and a tag fails until the release key exists ([`keys/README.md`](keys/README.md)).

**Where things are.** `components/core/` is the logic — no ESP-IDF include, enforced by the
build — tested on the host by `host_test/` (CMake + [doctest](host_test/third_party/doctest/README.md),
under the address and undefined-behaviour sanitizers). `components/interfaces/` is what `core/`
asks of the world, `components/platform/` the ESP-IDF answers, `components/modem/` the LTE modem
(`dptechnics/walter-modem`, a managed component pinned in `dependencies.lock`), `main/` the
start-up. `sdkconfig.defaults` carries a reason per line; `partitions.csv` is checked by
`tools/check_partitions.py` against every build. `tools/release/` makes a release and refuses the
ways one can be wrong; `keys/` holds the public halves of the keys a release may be signed with.
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the map;
[`docs/release.md`](docs/release.md) is how a tag becomes a release;
[`docs/console.md`](docs/console.md) is the console;
[`docs/bench.md`](docs/bench.md) is what only a Walter on a desk can confirm.

The task piles that build the rest, one headless session per task:

```powershell
.\tasks\run-pile.ps1          # phases 0 to 3
.\tasks\run-all.ps1 -Phase 0  # one phase
```

## What it will be, in one table

| | |
|---|---|
| Module | QuickSpot Walter: ESP32-S3, 16 MB flash, Sequans GM02SP (LTE-M / NB-IoT) |
| Built with | ESP-IDF v5.5.5 in Espressif's Docker image, pinned by digest (`tools/idf-image.txt`); the `walter-modem` component at 1.5.0 |
| Verified by | `tools/verify.ps1` — host tests, the release tooling, the firmware build and a dry-run release, in that image; CI runs the same script |
| Tested | on the host: the link, the debug port, the nRF52's flash controller and the update procedure, against simulators |
| Flashed | at a bench: its own USB-C. In service: by itself, A/B, with rollback |
| Released as | a GitHub Release, made by pushing a tag: the OTA image, a factory image, `manifest.json`, its signature, `SHA256SUMS` (`docs/release.md`) |

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
