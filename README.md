# TwinOak Walter firmware

The firmware for the LTE box of a [TwinOak standard repeater
v2](https://github.com/twinoak/twinoak.meshcore.standard-repeater-design-v2).

**A unit is two boxes on one battery, on one mast.** The *LoRa box* (a RAK4631) is a MeshCore
repeater: it is the unit's identity on the mesh, it is the only thing that talks to the air, and it
needs nothing of the other box — it boots, repeats and answers mesh administration with the LTE box
asleep, dead or absent. The *LTE box* (a QuickSpot Walter: an ESP32-S3 with a Sequans LTE-M modem)
is what this repository builds. It is wired to the repeater by a UART, a reset line, a power-kill
line and the debug port, and its job is to make sure **the repeater on the mast cannot be bricked
from afar** and can be looked after without climbing to it. The plan, the decisions (`S-1`…) and
the two protocols the boxes share are in [`spec/`](spec/README.md) — start with
[`spec/SYSTEM.md`](spec/SYSTEM.md).

Since phase 4 a unit is three things at once: **a repeater** on the mesh, **an exit node** —
meshmaster.dk reaches that repeater, and its neighbours on the air, through this box — and **a
remotely updatable one**, which is the whole reason the box exists. It is registered once, by a
person, at meshmaster.dk (*Exit nodes → Register an exit node*), which gives the unit a token that
is typed into its console once and read back by nothing afterwards
([`docs/provisioning.md`](docs/provisioning.md)); after that the unit checks in on its own
interval, runs the jobs the server has for it and sleeps again
([`docs/field.md`](docs/field.md), *As an exit node*).

## The state of things

> **The first unit works, it ran a night on its own battery, and the night was quiet.** On
> 23 October 2026 a Walter on `0.0.0-dev` checked in with the real meshmaster.dk — it is *LTE test
> node* there: the SIM attached, the chain verified and the server took it. On 6 November `v0.1.0`
> went onto the first unit's Walter from the published files and could not give its radio a first
> image; `v0.1.1` went on by cable on 7 November and could, and `v0.1.2` by cable on 8 November.
> **What a board has shown since:** the SWD flasher wrote a whole repeater application — 102 pages,
> read back whole, the radio reset, proved and committed as the **golden image**, with no watchdog
> reset in the middle; a whole repeater release pushed into the store over USB in seconds rather
> than hours; four HTTPS connections of one wake in a row; a telemetry job of the unit's own
> repeater answered **through the local door**, with nothing on the air; and 56 wakes of a night on
> a battery, a quarter of an hour apart. **And what it found:** meshmaster.dk heard that unit on about **every sixth wake** — a hold from one failed call
> was put back whole at every wake, because the sleep in between counted for nothing — while its
> journal said the backend had been *reached* on wakes that sent nothing at all; the **first update
> over LTE failed in two seconds**, because no download of either box has ever told the server which
> build was asking; a radio recovered by hand on 6 November came back as a **new node**, which this
> firmware read as a radio awaiting its first install; a download message promised a resume the
> Walter's own slot cannot keep; and a thirty-second stall on an observations POST turned out to be
> **reproducible**, with no way to trace it but a build of its own.
> `v0.1.3` is those six ([`tasks/phase-10/`](tasks/phase-10/README.md)), and it is the first
> release that could really be given to a unit in the field **over LTE** — the path had existed
> since `v0.1.2` and had never once worked. **And then it did, and the release could not start.**
> On 9 November the unit fetched `v0.1.3` over LTE in twelve ranges, switched to it, and was back
> on `v0.1.2` a moment later by itself: ESP-IDF could not create the main task, because 1 728
> bytes more static data had moved a 20 kB block into the heap its 16 kB stack needed. The
> rollback did exactly what phase 2 built it for, on a unit nobody was at — and `v0.1.3` is an
> image that cannot start **on any board**, by cable or over the air (*A Walter stuck on 0.1.3* in
> [`docs/flashing.md`](docs/flashing.md)).
> [`v0.1.4`](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases/latest) is
> `v0.1.3` that starts: the room back, a build that **fails when the main task would not fit**, a
> rollback that says what killed the trial, and a release that is **booted in an emulator before
> it is published** ([`tasks/phase-11/`](tasks/phase-11/README.md),
> [`docs/release.md`](docs/release.md)).
> **And a unit took it over LTE, with nobody at it, and kept it.** On 10 November the first unit
> fetched `v0.1.4` from `v0.1.2` — offer 24932, *stored* as the other slot's image at **14:52 UTC**,
> 780 528 bytes — and the apply after it came back **committed at 15:20**: *the candidate proved
> itself and is the golden image now (0.1.2 → 0.1.4)*. The heartbeat has said `0.1.4+62538fa`
> since. That is the first update over LTE that held, and 9 November's rollback is the other half
> of the same story. **It is the server's side of it and nothing else**: meshmaster.dk's pages for
> that unit and its jobs were read on 12 November, nobody was at the unit and no console was
> watched ([`tasks/phase-12/evidence/`](tasks/phase-12/evidence/README.md)), so what only a port shows —
> the six checks as they pass, the ranges, `self status`, the trial's own lines, the seconds of
> the download — is still unread. **Three offers before it expired unclaimed**, at 11:05, 11:58
> and 12:30: an offer lives thirty minutes on the server and the unit was not checking in inside
> any of them, which is what offering a release to a sleeping unit costs
> ([`docs/flashing.md`](docs/flashing.md)). **Everything else is still simulators**: a simulated nRF52 down to its flash
> controller, a flash that loses power at any operation, a scripted repeater, a fake modem that
> fails in every way a real one does, a scripted HTTPS server, a scripted meshmaster.dk and the
> agent library's own simulated stock repeater. [`docs/bench.md`](docs/bench.md) is the walk that
> turns that over one step at a time, `v0.1.4` is the release it is walked with
> ([`docs/flashing.md`](docs/flashing.md)), and this line stays here until a unit has walked it.

**Still unverified on a board** ([`docs/bench.md`](docs/bench.md), the six it marks ★): §5 the
first whole flash, with the power pulled half-way through; §9 a deliberately broken candidate that
must fail its health check and bring the golden image back; §15 a scope on the radio's reset line
across sleep and wake, where there must be no edge at all; and §18's three — the first heartbeat
recognised as *this* unit, an offer that resumes after the cable is pulled, and an apply that rolls
back when the server asked for it. Nor has a unit yet walked a whole day, timed a flash or a push
properly, run twenty pairs of HTTPS requests, **timed** a release of its own taken over LTE (one
was taken and kept on 10 November, but only the server watched it: the download is known as *at
most* 10 min 15 s, because the job's page does not say when the unit claimed it), or seen
`rak provision` give a factory radio its base.

| Phase | What | |
|---|---|---|
| 0 | the repository: toolchain, CI, the release with a signed manifest, the board's seven pins | built |
| 1 | hands on the radio: the box link, the debug port, the nRF52, the flasher | built |
| 2 | images and updates: signatures, the image store, the radio's update, the Walter's own, the supervisor | built |
| 3 | the uplink and staying alive: the modem, verified HTTPS, settings and the console, the duty cycle, the wake cycle | built |
| 4 | **the agent**: the exit-node firmware's job logic as a library at a pin, the mesh over the link, the uplink over the modem, the platform and the identity, the firmware jobs on the update machines | built; needs **meshmaster.dk 4.14 or newer** deployed to talk to |
| 5 | the radio's base (S-5a): the `provision` file in the store, the CTRL-AP and UICR behind one door, `rak provision` | built |
| 7 | **the first release**: a flashing guide, release notes a person reads, and `v0.1.0` published | released |
| 8 | **what the first unit showed** (6 November): the flasher's watchdog, the transfer in bytes, one connection one socket, the radio known before a job, link 1.1 and the band — published as `v0.1.1` | released |
| 9 | **the first unit's weekend** (7 November): a delay that yields, the socket ids' close accounting, a silent radio told from a scanner, the local door with no password and link 1.2's `LOCAL_REQUEST` — published as `v0.1.2` | released |
| 10 | **a unit that is heard** (8 November): a wait that counts the sleep, what a wake really did, a radio without its identity, the download that does not resume, a fetch that says who it is and the modem's trace from the console — published as `v0.1.3` | released |
| 11 | **room to start** (9 November, after the first update over LTE): the room back for the main task and a model of the heap that explains both releases, a build that refuses an image that would not start, a rollback that carries the reset reason and the core dump's panic line, and **a release booted in QEMU before it is published** — published as `v0.1.4` | released |
| 6, later | the bench pile, once the rig is on USB; the bridge role; the dual adapter | — |

**The release is [`v0.1.4`](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases/latest)**
(`v0.1.0` was the first, on 5 November; **`v0.1.3`, 9 November, must not be installed** — it
cannot start, and *A Walter stuck on 0.1.3* in [`docs/flashing.md`](docs/flashing.md) is how a
board that has it is brought back), signed with the release key Thomas made on 20 October
2026 — key id `73efdd383a957596`, its public half in [`keys/release-keys.txt`](keys/README.md) and
its private half a secret of the release workflow. So a build of this firmware trusts one key
(`trust` on the console says which), and a Walter flashed by cable with a release can take every
later one over LTE. **A unit at `v0.1.2` or `v0.1.1` takes this one that way** — *Offer a release*
on its page with the Walter's product, then an apply, and no cable at all; one still at `v0.1.0`
takes it by cable, because its second HTTPS connection of a wake was the one that failed.
[`docs/flashing.md`](docs/flashing.md) has both.

## What it does

1. it **watches the repeater** and brings it back — ask again, reset, power-cycle, reflash a
   known-good image — slowly, one rung at a time, and within daily limits
   ([`docs/supervisor.md`](docs/supervisor.md));
2. it **replaces the repeater's firmware over the debug port**, the application region only, checks
   that the new one is healthy, and puts the old one back if it is not
   ([`docs/update.md`](docs/update.md));
3. it **updates itself** A/B, and goes back if the new image cannot reach its main loop, the
   repeater *and* the server inside one boot;
4. it is the unit's **uplink**: LTE-M, HTTPS with a verified chain, resumable downloads
   ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §9);
5. it is the unit's **MeshMaster agent**: one wake is one check-in with meshmaster.dk — a
   heartbeat that says who this unit is and what it can do, and then the jobs the server has for
   it, run one at a time. Jobs for its **own** repeater go over the UART between the boxes with no
   packet on the air; jobs for a **neighbour** go over the repeater's radio, one packet per
   command. The job logic itself is the exit-node firmware's, taken as a library at a pinned
   commit ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §13,
   [`docs/upstream.md`](docs/upstream.md)). Among those jobs is **`twinoak.report`**, which asks
   the unit what it says about itself: the console's own lines by section — the wakes, the
   agent's state, the uplink, the journal, the settings, the log — so that a unit on a mast can
   be read without the cable, and read only;
6. it **sleeps**, and the battery it shares with the repeater chooses for how long: a full pack
   under a sunny panel gets a wake every quarter of an hour, a sinking one in December gets one a
   day, and below a floor the modem is not used at all ([`docs/power.md`](docs/power.md)). One wake
   is one pass of ten steps and then deep sleep ([`docs/cycle.md`](docs/cycle.md)).

## What it deliberately does not do

- **It does not invent a protocol.** Nothing here names a server endpoint, a JSON field or a job
  type of its own: the agent protocol is meshmaster.dk's and is spoken by the library
  (`spec/SYSTEM.md` §8), the link to the repeater is `spec/LINK-PROTOCOL.md`, and the job shapes
  are meshmaster.dk's own contract fixtures, which the host tests read rather than transcribe. A
  unit therefore needs **meshmaster.dk 4.14 or newer** to talk to; against anything older it
  heartbeats and is told nothing to do.
- **It takes no server's word about an image.** A release the server offers is checked six ways
  against the bytes in this unit's own flash (`spec/FIRMWARE-MANIFEST.md` §5), the hash an apply
  names must be one the unit already holds, and whether a flashed image is good is decided by the
  health check in the box — never by the server. An apply the server called good and the box
  refused is reported as `rolledBack`, and the candidate is never tried again.
- **It is no mesh chat.** The box link has a call for every mesh request the protocol defines, and
  the agent uses them only for a job it was asked to do: **one packet per command**, nothing
  relayed, nothing overheard written down. A grep and two simulated months hold it to that.
- **Nothing automatic erases a chip or writes UICR.** No supervisor rung, no update, no wake and
  no job from a server leads to the nRF52's `ERASEALL` or to a word of its UICR; a module whose
  access port is protected is detected, reported and refused by everything that runs by itself.
  The one path is `spec/SYSTEM.md` S-5a: a person's typed `rak provision <radio> confirm`, which
  erases a factory radio whole and gives it its base through one door, and rails R3 and R3b hold
  that door to its one caller ([`docs/provisioning.md`](docs/provisioning.md), *A factory radio*).
- **It does not update two things in one wake**, and it does not update anything while its own
  image is unconfirmed (`spec/SYSTEM.md` S-7).

## Building and testing it

[Docker](https://docs.docker.com/get-docker/) and PowerShell (Windows PowerShell 5.1, or `pwsh` on
Linux and macOS) — and nothing of ESP-IDF on the machine. Everything is built and tested inside
Espressif's `espressif/idf` image at one tag and digest
([`tools/idf-image.txt`](tools/idf-image.txt)), the same image CI uses. **One command is the
definition of "verified":**

```powershell
.\tools\verify.ps1                # everything: the rails, the spec copy, the host tests, the
                                  # release tooling, the firmware, the partition table, a dry-run
                                  # release
.\tools\verify.ps1 -Only build    # one step (spec, spec-tools, deps, host-tests,
                                  # release-tests, build, partitions, release)
.\tools\verify.ps1 -Clean         # a cold run: throw the build and cache volumes away first
.\tools\verify.ps1 -Shell         # a bash inside the same container, same mounts
.\tools\verify.ps1 -Version 0.3.1+1a2b3c4
                                  # everything, at the version a release would be built with
```

The first run pulls a 5 GB image and builds everything (minutes); the second is a few minutes,
because the build directory and `ccache` live in Docker volumes rather than on the bind-mounted
checkout. Each step logs to `out/verify/<step>.log`; the images land in `out/images/` and a dry run
of the release in `out/release/`. CI runs that same script and nothing else: green locally is the
bar for pushing (`spec/SYSTEM.md` S-10).

**Where the code is.** `components/core/` is everything that is logic — **no ESP-IDF include**,
enforced by the build — and it is tested on the host by `host_test/` (CMake +
[doctest](host_test/third_party/doctest/README.md), under the address and undefined-behaviour
sanitizers, with fakes and simulators that make things go wrong). `components/interfaces/` is what
`core/` asks of the world, `components/platform/` the ESP-IDF answers — kept thin enough that
reading them is the test — `components/modem/` the LTE modem (`dptechnics/walter-modem`, a managed
component pinned in `dependencies.lock`), and `main/` the start-up that constructs the real things.
`components/agent/` is this firmware's half of the agent library's seams — the mesh over the box
link, the uplink over HTTPS, the platform, the identity and the firmware jobs — and is logic too:
the same flags, the same no-ESP-IDF rule, the same host tests.
`sdkconfig.defaults` carries a reason per line; `partitions.csv` is checked against every build.
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the map.

`components/agent-core/` is the one thing under `components/` that is **not ours**: the MeshMaster
agent's job logic, taken from `twinoak.meshmaster.exitnode-firmware` at one commit as a checked
copy with a manifest, never edited here, together with its fakes, its fixtures and its own two
suites under `host_test/agent-core/` — which run in this repository's host tests.
`tools/verify.ps1` holds both copied trees against their manifests before it starts the container.
[`docs/upstream.md`](docs/upstream.md) is how it got here and how it is kept up to date;
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §13 is what it does.

**How it gets built.** One task, one branch, one PR, one headless session each
([`tasks/`](tasks/README.md); [`tasks/RUNBOOK.md`](tasks/RUNBOOK.md) is the procedure). Review
findings go into each phase's `FOLLOWUPS.md`, tagged with the task that must pick them up.

```powershell
.\tasks\run-pile.ps1          # phase after phase, stopping where a phase stops
.\tasks\run-all.ps1 -Phase 3  # one phase, task by task
```

| | |
|---|---|
| Module | QuickSpot Walter: ESP32-S3, 16 MB flash, Sequans GM02SP (LTE-M / NB-IoT) |
| Built with | ESP-IDF v5.5.5 in Espressif's Docker image, pinned by digest (`tools/idf-image.txt`); the `walter-modem` component at 1.5.0 |
| Verified by | `tools/verify.ps1` — the rails, the host tests, the release tooling, the firmware build and a dry-run release, in that image; CI runs the same script |
| Flashed | at a bench: its own USB-C. In service: by itself, A/B, with rollback |
| Released as | a GitHub Release, published by pressing *Run workflow* |

## Flashing a Walter

**[`docs/flashing.md`](docs/flashing.md) is the guide** — one page, from a module out of its box to
a unit checking in with meshmaster.dk, with the release's files, the three lines to look at on the
console and the first three things to try when it does not come up. The short of it, over the
module's USB-C, which is the ESP32-S3's own USB (`pip install esptool`; hold BOOT while plugging in
if the port does not appear):

```
esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 out/images/twinoak-walter-factory.bin
```

That is the image of a local build; a release's is `twinoak-walter-vX.Y.Z-factory.bin`, from
[*Releases*](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases/latest).
Then open the port (any baud; USB Serial/JTAG ignores it) and press reset. The console prints the
banner, which slot it is running from, what the descriptor inside the image says it is, why it
booted, whether the radio's UART came up, and a `>` prompt. **A boot that finds a USB host stays
awake** — this chip cannot be woken from deep sleep by a cable, so a person who plugged one in is
here on purpose; without one, the board runs one wake and sleeps.
[`docs/console.md`](docs/console.md) has every command;
[`docs/provisioning.md`](docs/provisioning.md) is the session a unit gets once, at a bench, before
it goes out.

**The bench walk is [`docs/bench.md`](docs/bench.md)**: everything no session could check, in the
order the rig is brought up, from an empty desk to a day on a mast — what to connect, what to type,
what should be seen, and what it means if it is not. It ends with *What to bring back*: every
constant in this firmware that a measurement has yet to settle, in one table.
[`docs/field.md`](docs/field.md) is the other half of that: what a unit on a mast does that a person
should know.

## Making a release

**A release is a person pressing *Run workflow*** on `release`, from `main`, with *dry run*
unticked — not a merge, not a green CI run, and not a pushed tag, which starts nothing
([`docs/release.md`](docs/release.md)). The run reads the version from [`VERSION`](VERSION),
takes the body from the lines the tasks left in
[`release-notes/unreleased/`](release-notes/README.md), builds the images, writes a
`manifest.json` that says what each file is, boots the factory image in QEMU, signs the manifest
with Ed25519 where it was built, publishes the lot as a GitHub Release with its tag — and then
files the lines as `release-notes/vX.Y.Z.md` and moves `VERSION` on, in a small pull request of
its own. A unit **checks it where it is used**: six checks
(`spec/FIRMWARE-MANIFEST.md` §5) — the signature by a key compiled into that build, the manifest,
the right image for that radio, the file's hash, the one descriptor inside it, and the region —
over the bytes as they lie in the unit's own flash, never over what a sender said it sent. The
server delivers; it is not trusted.

`tools/verify.ps1` rehearses the whole release path on every verified commit, signed with the
specification's *public* test key, so the path is never run for the first time when the button is
pressed. Three things a release needs and a rehearsal does not: the release key, which exists
([`keys/README.md`](keys/README.md)); a version in `VERSION` whose tag is not already there; and
at least one line waiting — a release with none of them is refused before anything is built.

## The rails

The promises this firmware makes are each a test or a grep that fails the build, and they are
indexed in one place: **[`tools/check_rails.py`](tools/check_rails.py)** (run by `verify.ps1`,
twenty-four entries, one per rail, `--list` prints them) and **`host_test/rails/`** (the
assertions that are about every test in the suite rather than one, and the two simulated months).

| | |
|---|---|
| never a chip erase by anything automatic | no `ERASEALL`, no `ERASEUICR`, no write to UICR anywhere but one door — a grep over every source that allows exactly two files, `regs.h` and `provision_door.cpp` (R3), and a tripwire in the simulated chip that only a test about the door may disarm. Since `spec/SYSTEM.md` S-5a (2026-10-20) a person's typed `rak provision <radio> confirm` is the one exception: it goes through that door, which checks the typed grant against the chip itself, and R3b holds the door to its one caller — `core/provision/`, and never the supervisor, an update, the flasher, the wake cycle or an agent |
| the pins have rules | reset is open-drain and **never driven high**; everything radio-facing is an input before the kill line goes high; two resets at least a second apart — checked by the fake pins at every transition of every test |
| an image goes into its region and nowhere else | the flasher's region is this firmware's own table per board; a manifest cannot widen it, and the simulated chip fails any test that writes outside the window it declared |
| nothing is flashed that is not signed | the store's two doors take only the verdict's tokens, which have no public constructor: no signed manifest, no erased sector |
| every step of an update is journalled first | a reset in the middle resumes or rolls back; it never guesses |
| never both updates in one wake | and never one at all while this firmware's own image is unconfirmed |
| TLS is verified | the modem's other validation levels are named nowhere in this firmware, the one call that configures a profile is one call, and there is no `http://` literal |
| secrets stay where they are | a token and a PIN in NVS, a repeater's password in RAM for the length of an exchange; no command, log line, journal entry or wake record carries either |
| nothing the mesh said is logged as text | not a neighbour's reply to a job, not a stranger's name in an advert, not a scanner's line: it goes into the result the server asked for and into no log, journal or retained record — searched for over a month of wakes as an exit node |
| a password crosses once, and is nowhere afterwards | a repeater's password is in RAM for the exchange, wiped from the link's send *and* receive buffers, on the wire exactly once, and read back as zeros out of every buffer it crossed |
| one packet per command | the radio's own `client_tx` moved by exactly the number of sends the agent's runner was told went out, every wake of a month |
| the token goes to one host, in one header | read in four places and printed in none; `Authorization: Bearer` to the configured host and dropped at a redirect to another; in no URL, no body, no error text and no log |
| the server chooses nothing | it offers; this firmware checks all six and decides. An apply's hash must be one the unit already holds, and a health check in the box — not a verdict from the server — says whether a flashed image stays |
| an update starts in one of three ways | a claimed job, a typed console command, or the supervisor's own golden-image rung — and nothing else, by a grep of the call sites |
| no job a finished wake claimed is left leased | a result the server would not take is kept in flash across the sleep and posted by the next wake that reaches one |
| slowly, and within limits | everything the supervisor does to the repeater has a wait, a backoff and a daily cap |
| the agent core is a copy and keeps its boundary | every file of `components/agent-core/` and `host_test/agent-core/` byte for byte what its manifest says, the pin in two places held equal, every `#include` in the library its own or ArduinoJson or the standard library, and no store, no file system and no way of skipping certificate verification in either tree |

## Licence

MIT — [`LICENSE`](LICENSE). The modem library (`dptechnics/walter-modem`) has its own licence,
which allows this use because the firmware runs on a Walter.
