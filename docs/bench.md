# The bench

**One walk, from an empty desk.** Everything four piles of tasks could not check, in the order the
rig is brought up (`spec/SYSTEM.md` §7, and the hardware repository's
`hardware/pcb/BENCH-BREADBOARD.md` §7): the Walter alone, then the UART, the reset line, SWD, the
first flash, **and only then the kill line** — then images, updates, the supervisor, the modem,
HTTPS, provisioning, sleep, and a day of it. A Walter has run a development build of this firmware
and checked in with meshmaster.dk once (23 October 2026, `0.0.0-dev`); the first unit ran `v0.1.0`
at a desk on 6 November — half an hour that found five faults — and `v0.1.1` on 7 November, which
gave its radio a golden image and found five more. **No unit has walked this file.** It is what
turns that sentence over, and **`v0.1.2` is the release it is walked with**
([`flashing.md`](flashing.md), [`release.md`](release.md)) — every item added by phases 8 and 9
needs that release or later, and says so.

Walk it in order. A step whose *Needs* are not met is a step that will waste an evening, and a
number written down in step 2 is one step 16 does not have to guess at.

## How to read a step

Every item is a box with four things in it: **what to connect**, **what to type**, **what should
be seen**, and — the one that matters when it goes wrong — **what it means if it is not**. Tick
the box when it is walked, with the date, and write the measured numbers into the item. The
constants those numbers replace are gathered in one table at the end:
[*What to bring back*](#what-to-bring-back).

Two marks:

- ⚠ **can damage something, or lose the radio's identity.** Read the item through before typing
  anything. These are the kill line, opening a protected module, a chip erase by a person, and
  anything that writes the nRF52's flash.
- ★ **the ones that matter most.** Three for the rig itself, and — since phase 4 made this unit an
  exit node — three for the agent. If there is one evening for the rig, these are it:
  1. **§5, the first flash** — `rak flash-test`, then a real image, then the power pulled
     half-way through. The unit exists so that this cannot brick a radio, and a simulator can only
     show that the documents were followed, not that the chip agrees with them.
  2. **§9, the deliberate bad image** — a signed, valid candidate that is broken on purpose: it
     must fail its health check, the golden image must come back, and the broken one must never be
     tried again. That is the whole promise of the project, on silicon.
  3. **§15, a scope on nRESET across sleep and wake** — there must be no edge at all. A Walter
     that glitches the reset line every time it wakes is a Walter that reboots the repeater every
     fifteen minutes, and nothing in a host test can see it.

  And if there is one evening for the agent, these three (`spec/SYSTEM.md` §8; a simulated month
  says all three work, and what it cannot say is whether meshmaster.dk agrees):
  4. **§18, the first heartbeat recognised** — one `agent wake` with the cable in, and the exit
     node's page showing *this* unit, by its repeater's key, asleep afterwards. Everything else in
     §§17–18 is downstream of the server and the unit agreeing about who this is.
  5. **§18, the offer that resumes after a pulled cable** — a download stopped in the middle by a
     wake's budget or by the modem going away, taken up from the stored bytes by the next wake.
     A unit on a mast on a metered SIM will meet this on its first real release; nothing but the
     rig can show that a resumed range and a whole one produce the same image.
  6. **§18, the apply that rolls back on a bad candidate** — the server asked for it, the health
     check refused it, the golden image came back and the page says `rolledBack`. This is §9's
     promise with a server in the loop, and the one place where "the server decides" would be a
     serious finding.

## The walk

| | | Needs before it |
|---|---|---|
| [1](#1-the-walter-alone-on-usb) | The Walter alone on USB | a Walter, a USB-C cable, `esptool` |
| [2](#2-the-pins-and-the-reset-line) | The pins and the reset line | §1, a meter, then a scope, then the RAK wired |
| [3](#3-the-link) | The link (the UART) | §2, the RAK with the repeater firmware |
| [4](#4-swd) | SWD | §2, the SWD lines wired, a scope |
| [4a](#4a-the-radios-base) ⚠ | The radio's base | §4, a factory RAK4630 (RUI3), a release with a `provision` file, the RAK's USB-C |
| [5](#5-flashing) ★⚠ | Flashing | §4, a repeater image, time |
| [6](#6-the-kill-line) ⚠ | The kill line | §§2–5, the kill emulator |
| [7](#7-the-slot) | The slot | §§1–6 |
| [8](#8-images) | Images | §1, a test-key build, a signed release |
| [9](#9-updating-the-radio) ★⚠ | Updating the radio | §5, §8, two releases (one broken on purpose) |
| [10](#10-updating-the-walter) | Updating the Walter | §1, §3, test-key builds as releases |
| [11](#11-the-supervisor) ⚠ | The supervisor | §3, §5, §8, a jumper to the RAK's nRESET |
| [12](#12-the-modem) | The modem | a SIM, an antenna |
| [13](#13-https) | HTTPS | §12 |
| [14](#14-provisioning-and-the-console) | Provisioning and the console | §1 (and §§12–13 for two items) |
| [15](#15-sleep) ★ | Sleep | §1, §3, a scope, an integrating meter |
| [16](#16-a-day) | A day | §15, §12, §14 |
| [17](#17-the-agent) | The agent | §3, a second repeater on the bench; §12 and a token for the uplink's items |
| [18](#18-a-day-as-an-exit-node) ★ | A day as an exit node | §§16–17, a token, a release on the server |
| [—](#what-to-bring-back) | **What to bring back** | the numbers, in one table |

The numbers above are this walk's, in walking order; the task files' own section numbers are not
(the kill line moved to §6, where the hardware's bring-up puts it, and everything after it shifted
by one). Each task's report says the same items under *Not verified here*;
`tasks/phase-3/reports/06.md` maps every one of them to a step here.

## 1. The Walter alone on USB

Needs: a Walter, a USB-C cable, `esptool` on the laptop (`pip install esptool`), the images from
`out/images/` of a local `tools/verify.ps1` or the CI artifact.

- [ ] **Flash the factory image** (task p0-01). The module's USB-C is the ESP32-S3's own USB; hold
  the module's BOOT button while plugging in if the port does not appear. Then, from the
  repository root (README.md *Building it* has the same command):

  ```
  esptool --chip esp32s3 --baud 921600 write-flash --flash-mode dio --flash-size 16MB 0x0 out/images/twinoak-walter-factory.bin
  ```

  Open the port at any baud (USB Serial/JTAG ignores it). After a reset the console prints
  `twinoak-walter 0.0.0-dev` on its own line, then `running from ota_0 at 0x010000 (3072 KB), ota
  state undefined (...)`, then `image descriptor at <address>: role manager, board walter, band
  lte-m, version 0.0.0-dev, link 1.2` (task p0-02), then `boot 1: power-on reset`, `the boot
  record starts here (a power cut, or the first boot)`, `type 'help' for the commands` and a `> `
  prompt (task p0-03; until it, the last line was `console on USB; nothing to do yet - idling`).
  `--flash-mode dio` is esptool's way in; the image's header says QIO and the bootloader
  switches to it (`sdkconfig.defaults`).

- [ ] **A reset runs the same image** (task p0-01; S-6). Press reset, or unplug and replug, three
  times. Every time the same banner and `running from ota_0`, and the ota state is `valid` from
  the first boot on: the factory image's OTA data is blank, and the bootloader's first boot writes
  `ota_0` an entry of its own in the state *valid* (`bootloader_utility.c`, `set_actual_ota_seq`:
  "Set actual ota_seq=1 in otadata[0]" in the bootloader's log) - never *new*, the only state it
  turns into a trial. (Task p0-01 predicted `undefined`; task p2-04 read the code and corrected
  this line. `undefined` would only show on a slot whose entry was erased after the first boot.)
  If the second boot ever says `aborted` or falls to the bootloader's `No bootable app
  partitions`, the premise in `tasks/phase-0/reports/01.md` (rollback) is wrong and the
  self-update (task p2-04, §10) must be re-read against what the board does.

- [ ] **Nothing else on the pins** (task p0-01, until task 03). With a meter: GPIO1 (nRESET), 2
  (SWDCLK), 4 (SWDIO), 5 (KILL), 7 (TX) read as inputs (high-impedance, pulled by nothing of ours)
  after boot. Task 03 owns their rules; this only confirms that task 01's image drives none of them.

- [ ] **A released factory image says on the console what its manifest says about it** (task
  p0-02). Take the `release-*` artifact of a *Run workflow* dry run (or `out/release/` of a local
  `tools/verify.ps1`), flash the `-factory.bin` with the `esptool` line in its `NOTES.md`, and
  hold what the console prints against the files beside it:

  ```
  python spec/tools/manifestref.py descriptor twinoak-walter-<tag>-factory.bin
  sha256sum -c SHA256SUMS
  ```

  The console's third line is `image descriptor at <address>: role manager, board walter, band
  lte-m, version <the manifest's version>, link 1.2` — the same role, board, band and version the
  descriptor and `manifest.json` give. Everything here is checked on every build except that a
  board flashed with the released file boots and prints it: that is what only a Walter on a desk
  shows.

- [ ] **A release's OTA image is not for esptool** (task p0-02; checked properly in phase 2, task
  04). `twinoak-walter-<tag>.bin` is written into the *other* application slot by a running Walter,
  not flashed at `0x0`. Nothing on the bench needs it yet; it is listed so that nobody flashes it
  to `0x0` and concludes the release is broken.

- [ ] **The console answers** (task p0-03). After the banner the port prints `boot 1: power-on
  reset`, `the boot record starts here (a power cut, or the first boot)`, `type 'help' for the
  commands` and a `> ` prompt. `help` lists seven commands; `status` says `radio power: on` and
  `next reset: allowed now` after the first second; `reboots` shows one entry. Press reset: `boot
  2: reset pin reset` and `reboots` shows two, the newer first, the older with how long the first
  run lasted. `reboot` on the console: the next boot says `boot 3: software reset, asked for on
  the console`. Unplug and replug: `boot 1` again and `the boot record starts here` — RTC memory
  does not survive a power cut.

- [ ] **`trust` names the release key** (task p2-01; the key was made 2026-10-20). On a normal
  build the console prints `ed25519 self-test (RFC 8032 test 1, and one bit changed): passed,
  <n> ms` — **write `<n>` down**: it is the first measurement of TweetNaCl's Ed25519 on the
  ESP32-S3, and a `FAILED` here means the vendored arithmetic is wrong on the board's compiler —
  then `trust: 1 key this build accepts a release signed with: 73efdd383a957596`, and the boot
  lines carry no `WARNING`. `trust: none: no update will be accepted (keys/release-keys.txt names
  no key; ...)` was the state before the key existed and would be again if that line were lost: a
  unit that accepts nothing, which is safe and useless. If a board on the bench is ever flashed
  with a test-key build (`tools/verify.ps1 -TestKey`), its banner says `+TESTKEY`, the fourth
  boot line is a `WARNING` and `trust` lists `c17969deb0026955`: never leave such a build on a
  unit that goes out.

- [ ] **The published release, flashed from the files a person downloads** (task p7-01). Not
  `out/images/` and not a workflow artifact: the five files of
  [*Releases* → *Latest*](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases/latest),
  downloaded as anybody would. Before flashing anything, check them where they landed —

  ```
  sha256sum -c SHA256SUMS
  python spec/tools/manifestref.py verify dad705e5374be34b9c42528a3b1215cc30deb1d575d2ed19e7645defcae2f1fe manifest.json
  ```

  (that hex is the public half in `keys/release-keys.txt`, key id `73efdd383a957596`; the same
  thing with the file rather than the hex is `python tools/release/verify_release.py <the
  directory>`). Then flash `twinoak-walter-v0.1.2-factory.bin` with the `esptool` line in the
  release's own body and read three lines: the banner's version is **`0.1.2+<sha7>`** with that
  commit's seven characters, `self status` says `ota_0` and **`valid`**, and `trust` says
  **`1 key … 73efdd383a957596`**. That is the whole point of the release: a board flashed by cable
  with it can take every later one over LTE.

- [ ] **[`flashing.md`](flashing.md) walked by a person, with nothing else open** (task p7-01).
  The guide is one page and claims to take a module out of its box to a unit checking in. Follow
  it top to bottom — the Walter, the RAK by cable, provisioning, `agent wake`, the page — and
  **correct it where it is wrong** rather than working around it: it is the only document here
  written for somebody who has not read the rest, and a session cannot tell whether it is enough.

- [ ] **A release taken over an older one by cable, on a unit that is already a unit** (task
  p8-06; the first unit, which is at `v0.1.1`). This is the item that says a factory flash is
  **safe on a unit in service**, which every sentence about updating by cable rests on. Before:
  `get` (how many of the keys are set), `image list` (what the store holds, with the candidate's
  hash), `rak update status` (the 6 November story and its last state), `image bad`. Then the
  `esptool` line of [*Releases* → *Latest*](https://github.com/twinoak/twinoak.meshcore.walter-firmware/releases/latest)'s
  `-factory.bin` ([`flashing.md`](flashing.md), *A unit at 0.1.0*), and afterwards:
  the banner says **`0.1.2+<sha7>`**, `self status` says `ota_0` and `valid`, and **all four
  readings above are unchanged** — the same keys set (a secret still answers `set`, never its
  value), the same images in the store with the same hashes, the same journal with the same last
  state, the same bad list. One key lost, one image gone or one journal entry missing is a finding
  against the partition table and against `tools/check_partitions.py`, which is the check that
  allows the file to be published at all. Nothing about this can be seen on a host: the promise is
  about where the flash tool stops writing.

## 2. The pins and the reset line

Needs: the Walter on USB, a meter, then a scope, then the RAK4631 on the RAK19001 with the UART,
nRESET and SWD wired through 470 Ω and 2.2 nF (`BENCH-BREADBOARD.md` §3 and §4). **The kill
line is §6**, after the first flash, because that is where the hardware's bring-up puts it. The
firmware's side is `components/core/board/radio_pins.cpp`; the rules are `ARCHITECTURE.md` §2,
and what the host tests proved is in `tasks/phase-0/reports/03.md`.

- [ ] **The safe state at boot** (task p0-03). With the meter, after boot: GPIO1 (nRESET), 2
  (SWDCLK), 4 (SWDIO), 5 (KILL), 6 (spare), 8 (RX) are high-impedance; GPIO7 (TX) reads high
  through the chip's weak pull-up (tens of kΩ to 3.3 V: a meter on it shows 3.3 V, a 10 kΩ to
  ground pulls it well below 1 V). `pins` on the console agrees: six `input`, TX `input pull-up`,
  all `released`. In the first 200 ms after boot `status` would say `unknown (the rail is
  settling)`; by the time a terminal is open it says `on`.

- [ ] **The power-up glitch is the ESP32-S3's, not ours** (task p0-03). With the scope on nRESET
  and the RAK wired: power-cycle the Walter. nRESET dips low for tens of microseconds at the
  Walter's power-up (the S3's glitch on GPIO1–14, `BENCH-BREADBOARD.md` §6 caveat 3; through the
  bench core's 10 k/100 nF it may only reach about 1 V) and the radio may restart once. After that
  nRESET, SWDCLK and SWDIO sit undriven for as long as nothing is typed. Expected; not this
  firmware's doing, and the first `rak reset` waits a full second after boot because of it.

- [ ] **`rak reset`** (task p0-03). Scope on nRESET: one low pulse of 100 ms (up to about 10 ms
  longer: a FreeRTOS tick), from the nRF52's pull-up level down to near 0 V and back to the
  pull-up level — **never above it**: the pin is open-drain and there is no code path that drives
  it high. The radio restarts once (its console prints its boot banner). Type `rak reset` twice
  quickly, or `rak reset` then `rak reset` within a second: the console prints `waiting <n> ms:
  two resets are a second apart`, the second pulse comes a second after the first ended, and the
  radio does **not** land in its UF2 bootloader (no USB drive appears on the RAK's port, Serial1
  keeps answering).

## 3. The link

Needs: the Walter on USB with §2 walked, the RAK4631 on the RAK19001 with the repeater firmware on
it, and the UART wired through both bulkhead filters — D-sub 8 and 9, 470 Ω and about 2.2 nF per
line (`BENCH-BREADBOARD.md` §3): Walter GPIO7 → RAK `Serial1` RX (P0.15), Walter GPIO8 ← RAK TX
(P0.16), the adapter's J4/J5 jumpers on 1–2. Both consoles open: the Walter's on its USB-C, the
RAK's on its own. The firmware's side is `components/core/link/client/`, the protocol is
[`spec/LINK-PROTOCOL.md`](../spec/LINK-PROTOCOL.md), what the host tests proved is in
`tasks/phase-1/reports/02.md`, and the commands are in [`console.md`](console.md).

This is the first time either firmware's codec meets the other's: until now both have only passed
the same fixtures.

- [ ] **`rak hello`** (task p1-02). Prints `protocol 1.0` from a repeater built against link 1.0
  and `protocol 1.1` from one built against 1.1 - the minor the **RAK** chose, which is the
  highest both ends know (§5.1) - then `role 1 (repeater)`, `slot 0`, `max payload 240`, a
  `boot_id`, the RAK's firmware, board and upstream versions, the band on its own line (task
  p8-05: `band 868`, or `band: not said (a 1.0 radio)`), and its capability bits by name. Hold
  them against what the RAK's own console says (`stats-core`, its banner): the same version, the
  same board. Then `rak ping` — the nonce comes back, the uptime grows between
  two of them, `1 attempt` — and `rak status`: its uptime, why it booted, its clock, its flags and
  `link errors 0`.

- [ ] **`rak cli get radio`** (task p1-02). The reply is the line the RAK's own console would have
  printed for `get radio`. Try one the mesh refuses but this link does not (§5.7: `stats-core`),
  and `rak cli reboot` — no answer, the console says that is expected, and the RAK restarts.

- [ ] **`rak power` and `rak env`** (task p1-02). Three channels with the shunts already applied:
  panel positive into the charger, battery negative while discharging, load positive. Against a
  meter on the battery: the sign and the order of magnitude. `rak env` within a degree of the room.

- [ ] **A reset of the RAK by its own button** (task p1-02). `rak events` then shows the boot —
  `boot (seq 0): reset 2 (reset pin)` with the new `boot_id` — and the RAK's clock is set straight
  afterwards: `rak time` says `time state 2 (set over this link since boot)`. There is no time to
  give yet on a Walter with no modem (phase 3), so what sets it on the bench is `rak time set
  <epoch>` from a person; after that, `rak hello` says the clock has been set and `rak status`
  agrees.

- [ ] **NoiseScope on the RAK** (task p1-02, §9). With the scanner firmware flashed instead of the
  repeater: `rak hello` takes three seconds, fails with *the link is down*, and then says *the
  radio is running the scanner: scanner (text)*. `rak text` shows its `{"t":...}` lines — at least
  the `info` line that answered the probe, and a `stat` line within ten seconds. `rak ping` is
  refused without sending a frame. `rak reset` puts it back to *unknown*, and with the repeater
  firmware on it `rak hello` answers again.

- [ ] **Through both filters, an hour of `rak ping` once a second** (task p1-02). From a laptop
  script on the Walter's console, or `rak status` before and after: **no retries counted** and no
  dropped frames (`status`'s link line: `retries 0, timeouts 0, drops 0`). A retry here would mean
  the 470 Ω and 2.2 nF are taking the edges off 115200 baud, and the baud rate is the thing to
  change (§1 is frozen).

- [ ] **An attention wakes the Walter** (task p1-02, §8). With the RAK's admin command that asks
  for the Walter (or a battery low enough): `rak events` shows `attention`, with its reason and
  `repeat` counting up if the Walter was not listening at once. Deep sleep is phase 3's, so what
  this shows now is only that the event arrives and the two zero bytes before it are not mistaken
  for a frame.

## 4. SWD

**⚠ the protected-module item: opening one erases the radio's identity, and is a person's job.**

Needs: the Walter on USB with §2 walked, the RAK4631 on the RAK19001 powered, SWDCLK and SWDIO
wired — D-sub 4 and 5, Walter GPIO2 and GPIO4 — first with short wires, then through 470 Ω and
2.2 nF on each line (`BENCH-BREADBOARD.md` §4), then through the real bulkheads; a scope with two
channels. The firmware's side is `components/core/swd/` (the wire and the debug port), the
simulator every line of it was written against is `host_test/sim/swd_target.cpp`, and what the
host tests proved is in `tasks/phase-1/reports/03.md`. Arm's ADIv5 (IHI 0031G) is cited beside
every number; the nRF52840 Product Specification ("Debug and trace") gives the clock its range,
0.125 to 8 MHz, which is why the default is 125 kHz and not the 100 kHz the plan said.

- [ ] **`rak swd connect` prints `DPIDR 0x2ba01477`** (task p1-03): `revision 2, part 0xba, DPv1,
  designer 0x23b (Arm) - the nRF52840's`, then `power-up: debug and system domains acknowledged
  (CTRL/STAT 0xf0000000)` (the four top bits: both requests and both acknowledges; `READOK` may
  add `0x40`), then `power-up request withdrawn: the radio leaves debug interface mode`, then
  `pins released` with the counters: expect `4 transfers` or so and `0 WAIT retries, 0 faults, 0
  parity errors`. First with short wires, then through 470 Ω and 2.2 nF on each line, then through
  the real bulkheads: the same lines every time. The RAK's repeater firmware keeps running
  throughout (its console still answers): nothing here halts the core. **Why this matters:** the
  host tests show the protocol is right bit for bit against a simulator written from the
  document; only a chip shows the document was read correctly.

- [ ] **The sample point and the ceiling** (task p1-03). Scope on SWDIO and SWCLK during `rak swd
  connect` at the default speed. In the acknowledge phase (right after the request's park bit)
  the RAK's edges on SWDIO should follow SWCLK's rising edges by under 2 µs through the filters,
  and be settled well before the *next* rising edge — that is where this firmware reads them (the
  end of the low half). Then raise the speed until `connect` fails and record the ceiling: the
  half-period is `kDefaultHalfPeriodUs` in `components/core/include/core/swd/wire.h` (no console
  setting yet — phase 3's settings task adds one); 3 µs is 167 kHz, 2 µs is 250 kHz, 1.5 µs is
  333 kHz, and the lower bound of 1 µs is 500 kHz. Predicted: near 300 kHz through the filters,
  the turnaround into the acknowledge failing first. Note the ceiling and the first symptom
  (`no target`, `protocol error` or `parity error`) here.

- [ ] **With the RAK unpowered: "no target", and nothing is back-fed** (task p1-03). Kill the
  rail with the emulator or pull the RAK's power, wait for `status` to say `radio power: on`
  (the firmware believes the rail is on when the kill line is not driven; here it is wrong, which
  is the point), and `rak swd connect`: `error: rak swd connect: no target: nothing answered on
  the line` within a few milliseconds, `pins released`, and the RAK's 3.3 V rail stays at 0 V
  throughout (the SWD lines are driven for 226 clocks per attempt, two attempts: about 3.6 ms of
  current through 470 Ω into a dead chip's clamps, which is what the series resistor is for).
  Then with the RAK powered but its SWD lines disconnected: the same `no target` (the Walter's
  input floats at the park bit's level; if it reads `protocol error` instead, that is a floating
  input reading low and is also acceptable — the report says why both are refusals).

- [ ] **The radio's current after `rak swd connect`** (task p1-03). The nRF52840 is in debug
  interface mode while a debugger holds `CDBGPWRUPREQ`; the command withdraws it before releasing
  the pins. With a meter on the RAK's supply: the current after the command is back to what it
  was before it. If it is not, note it here: phase 2's flasher ends with a reset pulse anyway, but
  `rak swd id` (task p1-04) must not leave the radio drawing more.

- [ ] **`rak swd id`: the chip, and — above all — its variant** (task p1-04). Expected, on a
  RAK4630 that answers:

  ```
  DPIDR 0x2ba01477 (the nRF52840's)
  control access port: IDR 0x02880000 (Nordic); protection: open
  memory access port: IDR 0x24770011 (Arm, class MEM-AP; the Cortex-M4's AHB-AP)
  chip: nRF52840, variant AAF0, flash 1024 KB = 256 pages of 4096 bytes (INFO.FLASH 1024 KB)
  device id <16 hex digits>
  core: running
  ```

  **Write the variant down here: ______.** It is `FICR.INFO.VARIANT`, the build code; `AAF0` is
  revision 3 (build code Fx0), which ships with access-port protection on unless `UICR.APPROTECT`
  holds `0x5A` — the module batch decides, not the board (`BENCH-BREADBOARD.md` caveat 6,
  `PROJECT-KNOWLEDGE.md` §6), so the first production module gets the same command. Two numbers
  the documents did not give and the simulator had to take from Arm's Cortex-M4 TRM and Nordic's
  register table: the AHB-AP's `0x24770011` and the control port's `0x02880000` — if either
  differs, note it; the firmware insists only on *class MEM-AP* and *designer Nordic* and prints
  the rest. The RAK's own console keeps answering throughout: the command halts nothing. The flash
  size says `1024 KB`; a module that says otherwise is not the one the flasher is written for.

- [ ] **If `rak swd id` says `protection: PROTECTED`: stop.** The command then reads nothing
  else and says: *opening a protected module erases it whole (bootloader, SoftDevice, identity,
  UICR): a bench procedure for a person*. That procedure is `BENCH-BREADBOARD.md` caveat 6 —
  CTRL-AP `ERASEALL` with a debug probe, nRESET **released** until `UICR.APPROTECT = 0x5A` is
  written, then a full reflash of bootloader and SoftDevice — once per module, never this
  firmware's. Note here which modules arrived protected. (And a module whose firmware opens the
  port at run time — rev 3 with `0x5A` in UICR — is protected again from a reset until that
  firmware has run: `rak swd id` after `rak reset`, within the first milliseconds, may say so.
  Try it, and note whether it does: the flasher (task p1-05) halts a running core because of
  it.)

- [ ] **`rak swd mem 0x10000100 2` prints `00052840 41414630`** (task p1-04): `INFO.PART` and
  `INFO.VARIANT`, the second being the variant as ASCII, first character in the top byte
  (`41 41 46 30` = `AAF0`). **`rak swd mem 0x00026000 4`** prints the start of the application's
  vector table — the initial stack pointer (`2000....`) and the reset handler (`000.....`, odd),
  if MeshCore's application sits at `0x26000` after the S140 SoftDevice as the flasher assumes;
  blank (`ffffffff`) means no application there, and the flasher's region table (task p1-05) needs
  the real address. **`rak swd mem 0xE000EDF0`** prints `DHCSR` with bit 17 (`S_HALT`) clear and
  bit 24 (`S_RETIRE_ST`) set on a running core: `0x0100000x`. **`rak swd mem 0x50000000`** says
  `FAULT ... at 0x50000000`: nothing lives there, and the access port's error response is read
  and cleared, after which the next command works.

## 4a. The radio's base

**⚠ erases the radio whole: RUI3, any identity it had, UICR. A person's command, once per
radio, naming the radio (`spec/SYSTEM.md` S-5a).** Phase 6, task 03 walks this with the rig; this
is the walk.

Needs: §4 walked (`rak swd id` answers through the filters); a **factory RAK4630 with RUI3** on the
production chain — the LTE box's D-sub to the LoRa box's, through the bulkheads, as in the field
(the hardware repository's `walter-swd-bench/README.md`, *What you need*); a repeater release
that carries the board's `provision` file, pushed with `walterctl.py push --as base` (§8), and its
`swd` image pushed `--as candidate`; the RAK's USB-C and a laptop for the end. The firmware's side
is `components/core/provision/` over the door of `core/nrf52/provision_door` (task p5-02); the
session, line for line, is `provisioning.md` *A factory radio*, and what the host tests proved —
every factory state, every refusal with nothing written, a power cut at every operation — is
`tasks/phase-5/reports/03.md`.

- [ ] **`rak provision check` on the factory radio.** Expected: the base's line (`bootloader
  0x000f4000, S140 6.1.1`, with the real base's byte count and *check 5p again … ok* — **write
  down the milliseconds**, which is 5p over ~450 KB of HEX out of the Walter's flash); `battery:
  unknown … accepted` (RUI3 answers no POWER_GET); `swd: DPIDR 0x2ba01477; control access port IDR
  0x02880000; protection: open` or `CLOSED`; on an open radio `chip: nRF52840, variant AAF0:
  improved APPROTECT …` and `device id <16 hex digits>`; on a closed one `link: no application
  answers HELLO`; and `next: rak provision <that> confirm`. **Write down the variant and whether
  the port was open or closed: ______ / ______.** This is `spec/SYSTEM.md` §10.2's first half,
  and `PROJECT-KNOWLEDGE.md` wants it. A variant the table does not know is said here (*the
  command would be refused at step 3*), with nothing erased: stop, and extend
  `core/provision/variant.h` with its source before anything else.
- [ ] **The typed line, and the seven steps.** Type exactly what `check` said. Expected, one line
  per step as each ends, then `rak provision: PROVISIONED slot 0: device id …, variant …, base
  sha256 …, port after the reset open; N s in all`. **Write down the time of each step** — the
  simulator predicts, at 125 kHz with a real ~180 KB base, about 21 s for the write (447 µs a
  word) and 40 s for the verify (866 µs a word), under a second for the erase (tERASEALL 169 ms
  typical, polled every 10 ms), 1.6 s for the reset and the bootloader's `kBootloaderSettleMs`,
  and the base's 98 passes over its ~450 KB of HEX out of the Walter's flash on top (invisible on
  the host; `image verify base`'s milliseconds ×98 is the estimate): **about a minute and a
  half, and a stop after step 4 is a sentence, not a hang.** On a locked radio step 3 comes after
  step 4 (the identity is readable only then). If the port was closed and reads *still closed
  after the erase and one soft reset*, note it: that is a part the Product Specification's
  model does not cover.
- [ ] **The port after the reset — §10.2's second half.** The `[7/7]` line says `the port after
  it open, NRFFW[0] 0x000f4000` or `CLOSED`. **Write it down: ______.** Closed on an `AAF0` part
  means the RAK bootloader in the release does not copy `UICR.APPROTECT` into `APPROTECT.DISABLE`
  at start-up (it was built with an MDK older than 8.45.0): the radio works, the flasher cannot
  reach it, and `rak update --first-install` will refuse the chip as *protected* until the
  repeater firmware's release carries a bootloader that does. That is a finding for the repeater
  repository, not a failure of this command.
- [ ] **The RAK's USB-C on a laptop: a UF2 drive appears.** The bootloader waits in DFU mode with
  no application. (Unplug it again before the next item: the Walter's debug port and a DFU over
  USB are two masters.)
- [ ] **`rak watch` says the slot awaits its first install**, and `status` says `radio health:
  awaiting its first install`. Leave the unit for a few heartbeats (`rak ladder` asks now): no
  rung is climbed, no reset pulse, no kill — the radio is silent by design.
- [ ] **`rak update --first-install`** with the candidate pushed: the flasher accepts the chip
  (`SoftDevice` where the profile says, the settings page erased and written), the radio boots the
  application, the health check passes, *done*. `rak watch` no longer says *awaits*. **Write down
  how long the first install took: ______.**
- [ ] **The same command again, on a radio that now has an identity.** `rak provision check`
  names it by its device id (the port is open); typing that line erases the application and its
  identity with the radio, which is what naming an open radio consents to. Only if the radio is
  disposable (`bench.local.json`, phase 6).
- [ ] **No watchdog reset anywhere in the run, and `boot` says so afterwards** (task p8-01; the
  **release** build of v0.1.1 or later, and a spare radio — there is nothing to see here on a
  radio already provisioned). Steps 5 and 6 are about a minute of unbroken SWD traffic with a
  real ~180 KB base, and before v0.1.1 nothing on that path let the idle task of the core run:
  the task watchdog (30 s, panic) would have reset the Walter in the middle, exactly as it did to
  `rak update --first-install` on 6 November. So: watch the seven step lines come out **with no
  reboot between them**, and then run `boot` — it must say `power-on` or `software`, never
  `watchdog`. **Write down the time of steps 5 and 6 again here**, beside the base's size: the
  pauses cost about 10 ms per kilobyte of base written and the same again verifying (the
  simulated figure is 0.5 s of pause on a 48 KB base), so the measured write should be a few per
  cent slower than task p5-03's prediction and no more.
- [ ] **A power cut in the middle, if a radio can be spared:** pull the Walter's USB during step 5.
  Expected afterwards: `rak provision check` says *protection: CLOSED* on an `AAF0` part (something
  is in flash, `UICR.APPROTECT` is not `0x5A` yet) and `next: rak provision locked confirm`; that
  line finishes it. On a legacy part the port reads open and the line is the device id again.

## 5. Flashing

**★ one of the three that matter most. ⚠ writes the radio's flash.**

Needs: §4 walked (`rak swd id` answers through the filters), the RAK's own USB console on the
laptop, a repeater image for the RAK (phase 2's `rak flash`, or a UF2 to load over USB), and
time: a full flash at 125 kHz is minutes (`tasks/phase-1/reports/05.md` gives the budget). The
firmware's side is `components/core/flasher/`; the simulator it was proved against is
`host_test/sim/nrf52_chip.cpp`, down to the two bootloaders' `bootloader_app_is_valid()`. **This
is the most important bench session of the project**: the unit exists so that this never bricks a
radio, and the simulator can only show the documents were followed, not that the chip agrees.

- [ ] **`rak swd id` first; note the variant; stop if *protected*** (task p1-05; §4's items). The
  flasher refuses a protected chip before touching anything, but the point of this line is the
  person, not the firmware: a protected module is a bench procedure (`BENCH-BREADBOARD.md` caveat
  6), not a flashing session.

- [ ] **Note the RAK's identity and one setting from its USB console** (task p1-05): `get
  public.key` and `get name`. These live in the file system at `0xED000`, above the application
  region's end; after everything below they must be what they were.

- [ ] **`rak swd mem 0x00003004 3` prints `51b1e5db 00026000 ....00b6`** (task p1-05): the
  SoftDevice's information structure — the magic, S140's size `0x26000` (where the application
  begins and the profile says it does), the firmware id `0x00B6` of S140 6.1.1. A size other than
  `0x26000` means the module carries another SoftDevice and the flasher refuses it (`the SoftDevice
  does not end where the profile's application region begins`); note the numbers here either way.
  Also `rak swd mem 0x000ff000 3`: the bootloader's settings record as the RAK arrived — expect
  `00000001 ffff00ff 00000000` (bank 0 valid, CRC check off, size 0: a UF2 install) or `00000001
  ffff00ff <size>`; `ffffffff ...` means the page is erased and `000000ff` that an earlier flash was
  interrupted. Write it down.

- [ ] **`rak flash-test confirm`: passes; the RAK still boots what it had** (task p1-05; the
  word is task p3-03's). Expected: the
  `swd:` session lines, then one line per page for the scan (`reading the pages the image covers:
  page 1/2`, `2/2`), the write (`erasing, writing and verifying the pages: page 1/2`, `writing
  and verifying the first page: page 2/2`), the read-back, then `rak flash-test: done in <ms> ms: 2
  pages (0 skipped, 2 written in place, 0 erased, 0 retries), 2 erases, 2048 words written, <n>
  read`, `the radio was reset through the debug port; the next reset pulse waits out the spacing`,
  `pins released`. **Write the time down** — it is the first measurement of a page write and an
  erase through the real wire. The RAK restarts (its USB console shows a boot) into the firmware it
  had; `rak hello` answers with the same version; `get name` is unchanged. If the command says
  `the last two pages of the application region are not blank`, the RAK's application reaches
  `0xEB000` — note the address it prints and do not force it; the region table is right, the image
  is just large. A `retries 1` or a failure `a page did not verify` names a page that would not
  take a write: note the address.

- [ ] **Flash a repeater image; the RAK restarts into it, not into its UF2 bootloader** (task
  p1-05; phase 2's `rak flash`, or until then a UF2 over USB plus `rak swd mem 0x000ff000 3` to
  see what the bootloader recorded). After the flash: `rak hello` shows the version flashed; the
  RAK's USB console answers `get public.key` and `get name` with what they were in the second item;
  `rak swd mem 0x000ff000 3` shows `00000001 ffff00ff <size of the image>`. Had the radio come up
  as a UF2 drive instead, the settings page is what to look at first: the flasher's record is the
  one the bootloader's own UF2 path writes, and a bootloader that refuses it is not one of the two
  read for this task (`core/flasher/bootloader_settings.h` names them) — note its version (the
  RAK's bootloader prints it on its USB drive's `INFO_UF2.TXT`).

- [ ] **Pull the Walter's power half-way through a flash; the RAK stays in its bootloader; flash
  again: it completes and boots** (task p1-05). With the stock bootloader the RAK shows a UF2
  drive on its USB (no valid application, no timeout); with OTAFIX it advertises BLE DFU instead
  and shows no drive (its README: no valid application defaults to OTA). Either way `rak hello`
  gets no answer and `rak swd mem 0x000ff000 1` reads `000000ff`: bank 0 invalid, which is the
  state the flasher put it in before the first erase. Then `rak flash` again: the scan finds the
  pages already written, writes the rest, and the counts show fewer erases than the first time.
  The RAK boots the image; identity and name unchanged. Do it twice: once with the power pulled
  during the pages, once in the last seconds (during the read-back or the settings write).

- [ ] **Double-tap the RAK's reset afterwards: the UF2 bootloader still works, and a UF2 of the
  stock firmware still installs** (task p1-05). The flasher never wrote the bootloader's pages,
  the MBR parameter page or UICR; this shows it. Then flash the repeater image back with `rak
  flash` and check `rak hello` once more.

- [ ] **Time a full flash; find the fastest SWD speed that still verifies** (task p1-05). The
  report predicts the budget at 125 kHz from the simulated clock (so many transfers a word, three
  reads of every page, 85 ms an erase); write the measured time here beside the image's size.
  Then raise the clock (`kDefaultHalfPeriodUs`, as in §4's ceiling item) and repeat until the
  read-back or a page's verify fails, and note the fastest setting at which the whole flash still
  verifies — which may be lower than §4's ceiling for `connect`, because a flash is a million
  transfers and the rarest bit error counts.

- [ ] **A build code Fxx module after an interrupted flash** (task p1-05; §4's protected item).
  Only if the variant is `AAF0` or later: with the application gone (the previous item's half-way
  state), does `rak swd id` still say `protection: open`? The flasher halts a running core instead
  of resetting it because a reset may close the port on such a module until *some* firmware has
  written `APPROTECT.DISABLE`; whether the bootloader alone does that is what this shows. If it
  says *protected*, the module cannot be re-flashed by the Walter after an interruption without a
  power cycle and a bootloader that opens the port — note it, loudly.

## 6. The kill line

**⚠ takes the radio off the air.**

Needs: §§2-5 walked, and **only now** the kill emulator (`BENCH-BREADBOARD.md` §5 - an LED and a
resistor in phase 1, the P-FET switch in phase 2), which is the order the hardware's bring-up puts
it in (`BENCH-BREADBOARD.md` §7 item 5: "Only now add the kill emulator"). A scope with two
channels and a meter on the LoRa 3.3 V rail. The firmware's side is
`components/core/board/radio_pins.cpp` rules 3 and 4; what the host tests proved is in
`tasks/phase-0/reports/03.md`.

**Why the mark.** A kill of a module that is mid-write to its own file system is the one way this
firmware could damage something it is not meant to touch, and nothing in it can know whether the
radio is mid-write. Keep the kills short, and never walk this section on a unit in service.

- [ ] **`rak kill 5`** (task p0-03; five seconds needs no `confirm`, and more does - task
  p3-03). With the LED emulator first (`BENCH-BREADBOARD.md` §5 phase
  1), then the P-FET switch: scope on TX (GPIO7), nRESET and SWDCLK against KILL (GPIO5). TX
  drops from its pulled-up high to floating (a 10 kΩ to ground on it would show it) and nRESET,
  SWDCLK, SWDIO are undriven **before** KILL rises; KILL is high for 5 s and then floats (it never
  goes low-driven: the 100 k pull-down on the A-board's Q3 gate is what takes it down); the LoRa
  rail falls and returns by itself. While it is down nothing is back-fed: the rail sits at 0 V, not
  at a diode drop (about 0.6 V would mean something of ours is pushing current into the radio).
  After it, `status` says `pins: power on` and TX is pulled up again. `rak kill 0` and
  `rak kill 31 confirm` are refused with the bounds and touch no pin, and `rak kill 31` without
  the word is refused by the parser before that (task p3-03).

- [ ] **Reset the Walter while a kill is in progress** (task p0-03). `rak kill 30 confirm`,
  then press
  the Walter's reset within those 30 s: the kill line floats the moment the S3 resets, the rail
  comes back by itself, and the new boot says `boot n: reset pin reset`; `status` says
  `pins: power on` after 200 ms. Nothing of ours holds the radio off across a reset.

- [ ] **The numbers that replace the placeholders** (task p0-03). With the scope on the LoRa rail
  during `rak kill 1`: how long after KILL rises the rail is below the nRF52's brown-out level
  (about 1.7 V) and below 0.5 V, with the radio in its normal receive load and with it idle; and
  after KILL falls, how long until the rail is steady at 3.3 V. Replace `kKillTiming` in
  `components/core/include/core/board/radio_pins.h` (min, max, settle) — the host tests and
  `help` follow — and note the measurements in this file.

## 7. The slot

Task p1-06 adds **no bench step**, and that is its result. A unit has one radio, slot 0, and every
`rak …` command means that slot without saying so; the dual adapter it makes room for does not
exist, and the firmware constructs no object that can steer one (`ARCHITECTURE.md` §2 rule 6). Two
lines are worth typing once while the rig is up, because they cost nothing:

- [ ] **`rak @0 hello` prints what `rak hello` prints, to the byte**, and `rak @1 hello` answers
  *error: slot 1: this adapter has 1 radio, so slot 0 is the only one* — with no frame on the
  UART and nothing on the seven pins. `rak @2 hello` is refused by the parser instead.

- [ ] **Every other item of §§1–6 behaves as it reads.** Nothing in task p1-06 changes a pin, a
  timing or a command's output except `rak flash-test confirm`, whose first line now begins
  `flash-test: slot 0, rak4630-single, …`.

When a dual adapter is built, this is where its bring-up goes: the selector programmed with a
scope on SWDCLK and nRESET (a window of 100–500 µs, two edges, and *no* reset at either module's
pin), the power-on default read back as module A, and `rak @1 hello` answering from the second
radio. None of it is verifiable today.

## 8. Images

From task p2-02 (`docs/ARCHITECTURE.md` §6, `docs/console.md` *Loading a radio image over USB*).
Needs: a Walter on USB, `pip install pyserial==3.5` on the laptop, and a **test-key build** of this
firmware (`tools/verify.ps1 -TestKey`, flashed as §1's first item) together with a repeater release
signed with the specification's public test key — the dry run `tools/verify.ps1` leaves in
`out/release/` is one, for the *Walter's* own image; a repeater release has to come from the
repeater repository's own dry run, or be made by hand with
`python spec/tools/manifestref.py` over a repeater image. A normal build accepts only a release
signed with the release key (`keys/release-keys.txt`), which is the point of the fourth item
below.

- [ ] **The store says what it is, on a board.** `image list` on a fresh unit prints the layout —
  `2 slots x 3 images of 815104 bytes (199 sectors)`, `plus one base of 655360 bytes (160 sectors)
  a slot`, `6242304 bytes of a 6291456-byte partition, 49152 left` — then `slot 0, rak4630-single`
  with three `empty` lines and `base       empty`, then `slot 1: no radio in this adapter`. The
  boot lines have gained `image store: rakfw at 0x630000 (6144 KB), …`. If either says something
  else, the partition table and the firmware disagree and nothing below is worth doing.

- [ ] **`walterctl.py push --as golden` on a test-key build, and `image list` shows it.** With the
  release directory on the laptop:

  ```
  python tools/walterctl.py push --port COM7 --as golden <release-dir>
  ```

  It prints the file it is sending, then `stored as the golden image`. Then on the console:
  `image list` shows `golden  repeater rak4630-single 868 <version>, <n> bytes at 0x00026000,
  sha256 …`, the commit it was built from and the key id `c17969deb0026955`; and `image verify
  golden` answers *the SHA-256 is the one in its record* — **write down how many milliseconds it
  took**, because that is the first measurement of reading 250-odd KB back out of the Walter's
  flash and hashing it on the ESP32-S3, and the radio's update (task p2-03) pays it before every
  flash.

- [ ] **A second push is refused, and a candidate is not.** `--as golden` again answers *the golden
  image is already there: it is replaced by a promotion, not by a load* (S-5), and `image verify
  golden` still passes — the refusal touched nothing. `--as candidate` with the same release
  succeeds, and `image list` then shows the same image twice in different areas.

- [ ] **A normal build refuses the same push at check 1, and `trust` says why.** Flash a build made
  without `-DTWINOAK_TRUST_TEST_KEY=1`; `trust` prints the release key's id (`trust: 1 key this
  build accepts a release signed with: 73efdd383a957596`, since 2026-10-20) and **not** the
  specification's test key, and the push ends with *check 1, the signature: the key id names no key
  this build trusts* — with nothing stored and no sector erased. A unit in service accepts a
  release signed by the release key and nothing else, which is the point.

- [ ] **Pull the Walter's power during a push.** Mid-transfer (a release of a few hundred KB gives
  a comfortable window), unplug the USB. Afterwards: the console comes up, and `image list` shows
  the slot either **empty**, or **empty with a half-finished transfer** (`a transfer of sha256 …
  stopped at N of M bytes (it can be resumed)`), or — if the slot had an image before — **nothing
  in between**: never a record over bytes that are not its own. Then run the same `walterctl.py
  push` again: it says `the Walter already has N of M bytes; sending the rest` and finishes, and
  `image verify` passes. Try it two or three times at different points.

- [ ] **The console comes back by itself.** Start a push and kill `walterctl` (Ctrl-C) in the
  middle. The port is silent for twenty seconds — that is the transfer waiting — and then prints
  `image receive: the other end went quiet: nothing stored` and a prompt. Typing `image list`
  works again.

- [ ] **`image erase`, and what it will not erase.** `image erase candidate` answers *candidate:
  forgotten in slot 0* and `image list` shows it empty; `image erase golden` is refused by the
  parser (*the golden image is not erased*). `image bad` says *no image has failed a health check
  on this unit* — task p2-03's update is what puts a hash there.

- [ ] **A push to `@1`.** `image receive candidate @1` answers *error: slot 1: this adapter has 1
  radio, so slot 0 is the only one*, and nothing on the port changes. The areas for slot 1 are in
  the partition all the same, which is what `image list`'s last lines say.

- [ ] **A whole repeater release pushed in, timed** (task p8-02; the **release** build of v0.1.1 or
  later, which is where the fix is). This is the item v0.1.0 failed on 6 November: the transfer's
  answers went out through the console's LF → CR LF, so the answer naming chunk 10 arrived
  corrupted and the push stopped at `no answer to type 0x03 seq 9`, four times. Push a real
  repeater release's `swd` file — 400 kB and more, **about 1,740 chunks**:

  ```
  python tools/walterctl.py push --port COM7 --as candidate <release-dir>
  ```

  Expected: `the console: protocol 2` on the second line, then a progress line every tenth of the
  file, then `stored as the candidate image`. **Write down the wall-clock time of the whole push**
  and the file's size: it is lock-step, one 240-byte chunk and one acknowledgement at a time, and
  the laptop's end waited out its whole 5 s reply timeout for every one of them until 6 November —
  two and a half hours for one image. **Seconds, not hours** is the answer this item is for; a push
  that takes minutes means the laptop is waiting on something and the figure is worth bringing back
  either way. Then `image verify candidate` passes.

- [ ] **The same push into a Walter still at 0.1.0, with nothing typed** (task p8-02; needs a
  second Walter, or this one re-flashed to v0.1.0 by cable and then back — only worth doing if a
  0.1.0 unit is at hand). `walterctl` reads the version off the console's ready line, which on
  0.1.0 names no protocol, and says *a Walter at 0.1.0 or older: taking its console's CR LF back
  out of its answers*. Expected: the push **finishes**, more slowly, and `image verify candidate`
  passes. `--undo-crlf` must not be needed; if it is, the ready line could not be read and that is
  worth knowing. Against a v0.1.1 Walter, `--undo-crlf` must be **refused**: *this push was set up
  for protocol 1 and the hello says 2*.

- [ ] **`walterctl.py push --as base` of a real release, over the cable** (task p5-01;
  `spec/FIRMWARE-MANIFEST.md` §4a, `spec/SYSTEM.md` S-5a). With a repeater release that carries a
  `provision` file — the real base: the MBR, SoftDevice S140 6.1.1 and the bootloader, ~450 KB of
  Intel HEX:

  ```
  python tools/walterctl.py push --port COM7 --as base <release-dir>
  ```

  It prints the file it is sending, then `stored as the base`, and then *nothing is written to a
  radio by this: that is `rak provision`, typed by a person*. **Write down how long the push took**:
  it is three times the bytes of a radio image and the first measurement of one.

- [ ] **`image list` shows the base with the right bootloader and SoftDevice.** The `base` lines
  say `the board's base, <n> bytes, sha256 …, bootloader 0x000f4000` and `S140 6.1.1, <n> bytes of
  flash, <n> UICR words, the MBR's parameter page at 0x000fe000`. If the bootloader's address is
  not `0x000F4000` or the SoftDevice is not S140 6.1.1, that release's base is not the one this
  firmware's board profile was written for and `rak provision` (§17) must not be run with it.

- [ ] **`image verify base` passes after a power cycle.** Unplug and replug the Walter, then
  `image verify base`: the SHA-256 is the one in its record, **and** `check 5p over the stored
  bytes: a base for rak4630-single (0x00026000 to 0x000ed000), bootloader 0x000f4000` with the same
  flash byte count and UICR word count as the record's. **Write down the milliseconds**: §4a's
  rules are read over ~450 KB out of flash, twice, and task p5-03 pays that before it writes
  anything to a radio.

- [ ] **`image erase base`, and the base again.** `image erase base` answers *base: forgotten in
  slot 0*; `image list` shows `base       empty`; `image verify base` says *there is no image in
  that slot*. Push it again and it is back. (The base is the one thing in the store a command may
  erase besides a candidate and a scanner image: nothing depends on its being there.)

Nothing here flashes a radio or writes a chip's UICR: putting a stored image on the RAK is §9's,
and giving a factory radio its base is `rak provision` — task p5-03's, and a person's.

## 9. Updating the radio

**★ one of the three that matter most. ⚠ writes the radio's flash.**

From task p2-03 (`docs/update.md`, `docs/console.md` *The radio's update*). Needs: §5 walked (a
real flash through the wire, the bootloader starting what was flashed), §8 walked (a repeater
release in the store), the RAK's own USB console on the laptop, and **two repeater releases
signed with the key this build trusts** - a good one and, for the second item, one that is valid
and signed but **broken on purpose**: a build whose link never comes up (the repeater repository
can make one by leaving the link task out, or by a `fw_version` in the descriptor that is not the
one the firmware reports). On a test-key build both are signed with the specification's test key.
With task p1-05's flashing session this is the one that matters most: the host tests prove the
procedure against a simulated radio that misbehaves on cue; only a RAK can show that a real
repeater boots inside the window, takes the clock, and stays up for the period.

- [ ] **The good path, watched.** With the golden image from §8 running on the RAK and a newer
  release pushed `--as candidate`: `rak update`. Watch the lines: `check -> flashing-candidate`,
  the flasher's pages, `-> booting: flashed; the radio was reset`, `-> proving: the radio
  answered` (within seconds of the reset: **write down how long**, that is `kBootWindowMs`'s
  bench number), then nothing for the proving period (three minutes today: `rak ping`'s cadence
  is inside it), `-> committing: the health check passed`, `-> done`. Afterwards `image list`
  shows the new version **golden** and the candidate slot empty, `rak hello` its version, `image
  bad` empty, and the RAK's own console `get name` / `get public.key` what they were in §5.
- [ ] **A broken candidate rolls back, and is never tried again.** Push the broken release `--as
  candidate`; `rak update`. It fails proving - `booting -> rolling-back: no HELLO inside the boot
  window` for a build whose link never comes up, or `proving -> rolling-back: HELLO's version is
  not the flashed image's` - and then `rolling-back -> rolling-back (booting the golden image)`,
  `-> rolling-back (proving the golden image)`, `-> restored`, and the last line names the reason
  and the step. `rak hello` shows the old version, `image list` the old golden image still golden
  and the broken one still the candidate, `image bad` lists its hash. **`rak update` again is
  refused at check**: *this image failed a health check once and is never tried again*, with
  nothing on the SWD lines. `rak update status` tells the whole story with the entries numbered.
- [ ] **Pull the Walter's power three times.** During the flash (the pages are going by), during
  proving (the quiet minutes), and during the rollback's flash of the golden image (use the broken
  candidate again after `image erase candidate` and a fresh push, since the bad list refuses it).
  Each time, after power returns, the boot lines include `update: the journal says a run was in
  progress (update); resuming at <state>`, the run goes on from the main loop printing its
  transitions, and ends *done* or *restored* as it would have without the cut; `rak update status`
  shows the resumed entry *(resumed after a restart)*. During proving, the whole period is run
  again - it is not credited with the minutes before the cut. A RAK cut mid-flash sits in its
  bootloader until the resumed flash finishes (§5's last items), which is right.
- [ ] **A role switch and back keeps the identity.** `image receive scanner` with a NoiseScope
  release, then `rak role scanner`: `-> done: a scanner is running`, `rak hello` says *the radio is
  running the scanner*, `rak text` shows its `info` line. Then `rak role repeater`: the golden image
  flashed back through the rolling-back states, `-> done: the golden image <version> is back and
  proved`, and `rak id` shows **the same public key and name as before** - the file system above
  the application region was never touched. `image list`: golden and candidate as they were,
  `image bad` empty.
- [ ] **`rak update --first-install` on a module whose application was wiped** (after §5's
  power-cut item, or `rak flash-test confirm`'s region). `rak update` is refused (*there is no
  golden image to return to*); with `--first-install` the candidate is flashed, proved and becomes
  golden. Do not try the broken candidate this way unless you mean to see *stuck: no image to
  return to*.
- [ ] **A whole image flashed with no watchdog reset, timed** (task p8-01; the **release** build
  of v0.1.1 or later, which is where the fix is). This is the item v0.1.0 failed on 6 November:
  a full repeater image is about seven minutes of SWD traffic at 125 kHz, and nothing on that
  path blocked, so the task watchdog (30 s, panic) reset the Walter twelve times over. Push a
  full repeater release `--as candidate` and run `rak update --first-install` (or `rak update`
  with a golden image). Expected: the flasher's page lines run through the scan, the pages and
  the read-back **without a reboot in the middle**, and the run reaches `-> booting` and then
  *done*. **Write down the wall-clock time of the whole flash and the image's size**: the
  simulated figure is 413 s for 417,284 bytes at 125 kHz, of which about 5.5 s is the pauses this
  task added — 548 of them at 10 to 11 ms each since task p9-01 made a millisecond a FreeRTOS
  tick, where at 100 Hz they were 511 at 10 to 20 ms and so 5.1 to 10.2 s. A reboot during the
  flash with the lines `boot N: watchdog reset` and `update: the
  journal says a run was in progress …; resuming at flashing-candidate` is this fault back.
- [ ] **The first install the 6 November failure left behind** (task p8-06; the unit's own radio,
  whose slot the journal says is *stuck*). That run ended on a **flash failure**
  (`fail(Reason::kFlashFailed, …)`, after the watchdog had reset the Walter twelve times over;
  report p8-01 has the reading), and a failure of any kind puts the candidate's hash on the bad
  list before the run enters *stuck* — `RadioUpdate::fail` marks the image first and only then
  decides there is nothing to return to — so **the image that failed is refused and cannot even
  be pushed back in**. Expected, in order: `image bad` names that hash;
  `rak update --first-install` on it is refused with *this image failed a health check once and is
  never tried again*, **with nothing on the SWD lines**; a push of the repeater's `v0.1.1`
  (a different file, a different hash) is accepted as the candidate; and `rak update
  --first-install` then runs to `-> done`, which makes that image **golden** and is the last word
  on a slot the journal had left *stuck* (`rak update status`, `image list`, `rak hello`). If the
  bad list is *empty* instead, the mark never reached flash on 6 November — say so, because then
  the stored `v0.1.0` candidate is usable and the refusal above will not appear.
- [ ] **`boot` afterwards says `power-on` or `software`, never `watchdog`** (task p8-01). Run
  `boot` (the reset-reason line, `docs/console.md`) right after the flash finishes, and again
  after the proving period. A `watchdog` there - even one, even with the update *done* - means
  some stretch of the run still monopolises the CPU for 30 s and is a finding against this task.
- [ ] **A radio that refuses its identity costs no rollback** (task p10-03; **needs a radio in
      repeater `v0.1.2`'s lost mode**, which is that repository's own bench line - one word
      written over `0xED000` with `"identity": "disposable"`, or a radio that has arrived there by
      itself). With a golden image in the store and a candidate pushed, `rak update`: expected
      `-> booting`, `-> proving`, and then **`rak update: the image is fine, the node is not: the
      radio refuses: its identity is lost, and no image mends that`** — *not* a rollback. Then
      check the three things the words promise: `image bad` is **empty** (or at least does not
      name this candidate), `image list` still shows the same golden image, and `rak update
      status` ends `node-fault`. `rak hello` says the **candidate's** version, because the flash
      was not undone. Afterwards `rak cli erase`, `rak cli reboot`, and the same `rak update`
      again must reach *done* — the image was never the fault. Write down whether the radio's
      `caps` in `rak hello` is `0x21`: that is what makes the check cost no frame on a healthy
      radio.
- [ ] **The numbers.** Against the lines above, `docs/update.md`'s table: the boot window (how long
  a real repeater takes from reset to `HELLO`; the window must be comfortably more), the proving
  period (long enough to catch a build that restarts itself after its first advert; short enough
  that a wake cycle can afford it), the battery threshold against the real pack's curve. Each is a
  constant in `core/update/radio_update.h` with a sentence; change the number and the sentence
  together.

## 10. Updating the Walter

From task p2-04 (`docs/update.md`, *The Walter's half*; `docs/console.md` *The Walter's own
update*). Needs: §1 walked, §3 walked (a radio answering `HELLO`, for the trial's second
condition), a **test-key build** of this firmware on the board, and **two more test-key builds of
this firmware as releases** (`tools/verify.ps1 -TestKey -Version 0.0.1+bench1`'s dry run in
`out/release/` is one: `manifest.json`, its `.sig`, the `ota` file) - a good one, and one built to
fail its trial. There is no build flag for the latter yet (the task asked for one that skips the
radio's `HELLO`); until phase 3 the simplest broken build is one whose radio UART is left closed
(comment out `link_port.open()` in `main/main.cpp` for the occasion and never release it), or
simply unplug the radio before the switch - the trial fails the same way, *the radio answered
before the update and not after it*. The host tests prove the machine against a fake of the
bootloader's rules; only a board shows that ESP-IDF's otadata and `esp_ota_*` behave as
`bootloader_utility.c` was read to say.

- [ ] **The factory image's first boot runs no trial.** After §1's flash: `self status` says
  `running: slot 0 at 0x010000 (3072 KB): valid (confirmed), version 0.0.0-dev`, `other: slot 1
  … undefined … (no image)`, `the running image is confirmed`, `no self-update has been
  journalled on this unit`; nothing of `self-update:` among the boot lines, and the same after
  three resets. This is the rule that a unit which has never been given a server does not reject
  itself (`docs/update.md`, *The uplink condition until phase 3*).
- [ ] **Push the next version and switch.** With the good release on the laptop:
  `python tools/walterctl.py push --port COM7 --as self <release-dir>`; it says `stored in the
  Walter's other application slot`. `self status` shows `received: 0.0.1 in slot 1 (N bytes),
  waiting for 'self switch'` and the story's first line. **Write down how long the push of a
  ~400 KB image took**: lock-step 240-byte frames over USB Serial/JTAG, the first measurement.
  The ready line says `(protocol 2)` and `walterctl` says `the console: protocol 2`; `self
  receive` is the same transfer as §8's and was broken in the same way until v0.1.1 (task p8-02),
  so a push that stops at `no answer to type 0x03 seq 9` is this board running 0.1.0.
  Then `self switch`: the line `self switch: making 0.0.1 (slot 1) the next to boot, from
  0.0.0-dev; the radio is told and the Walter restarts`, and the board restarts. Among the boot
  lines: `boot N: software reset, the Walter's own update, switching to the other slot`,
  `running from ota_1 … ota state pending-verify`, `self-update: this image (0.0.1, slot 1) is on
  trial: 300 s …`. Within seconds (the radio answers at once; no server is configured):
  `self-update: on trial -> confirmed: all three conditions hold: confirmed`, and `self status`
  says `running: slot 1 … valid (confirmed), version 0.0.1+bench1` and the four-line story ending
  `confirmed: all three conditions; no server configured`. **Write down how long the trial
  took** from the boot line to *confirmed*.
- [ ] **A version built to fail its trial rolls back, and says why.** Push the broken release
  `--as self` and `self switch` (or push the good one and unplug the radio's UART before the
  switch - then `self switch` refuses with *the radio does not answer HELLO and nobody has said
  why*, which is also worth seeing; plug it back, switch, unplug again during the first seconds of
  the trial). The trial image boots, `self status` shows `on trial for N s of 300: main loop met;
  radio pending (K HELLOs); uplink pending`, and after the window: `self-update: on trial ->
  rolled back: the radio answered before the update and not after it`, `restarting into the
  previous image`. The old image boots: `boot N: software reset, the Walter's own update, the
  trial image rejected itself`, `running from ota_0 … ota state valid`; `self status` shows the
  other slot `invalid (rejected by the image itself)` and the story ending `rolled back: …`;
  `image bad` lists the broken image's hash. **Pushing the same broken release again is refused**
  at the door: *this image failed its trial once and is never tried again*, with nothing erased.
- [ ] **Reset the Walter during a trial.** Push the good release, `self switch`, and press the
  module's reset while `self status` says *on trial* (use the broken build, or hold the radio in
  reset so the trial waits on `HELLO`). The board comes up in the **old** image: `running from
  ota_0`, the other slot `aborted (was on trial at a reset; the bootloader gave up on it)`, and
  the boot lines say `self-update: rolled back: the image restarted during its trial (the
  bootloader gave up on it)` and `the image 0.0.1 in slot 1 is on the bad list; this is 0.0.0-dev
  again`. This is the bootloader's rule - pending → aborted - seen on silicon.
- [ ] **Pull power while `self receive` is writing.** Mid-push (a 400 KB image gives a comfortable
  window), unplug the USB. The board comes up in the same image, `self status` shows the running
  slot as it was and no `received:` line, and the same push run again finishes. The running
  image was never touched: `esp_ota_begin` erases the *other* slot only.
- [ ] **`self confirm` and `self reject` on the bench.** During a trial that waits (the radio held
  in reset): `self confirm` ends it `confirmed: on the console`; or `self reject` ends it with a
  restart into the old image and `rolled back: rejected on the console`. Both refused with *the
  running image is not on trial* at any other time.
- [ ] **S-7 on the board.** During a trial, `rak update` is refused: *the Walter's own update is
  unconfirmed: never both at once (S-7)*. After `rak update` has begun (the flasher's pages going
  by), `self switch` is refused: *the journal says a radio update is in progress*.
- [ ] **The numbers.** The trial window against a real LTE attach when phase 3 has one (today the
  trial ends within seconds); the `SLEEP_NOTICE` time against how long the board takes from `self
  switch` to the new image's first `HELLO` - the radio must not have given the Walter up for dead
  meanwhile (`STATUS` flag bit 2 after the restart). Each is a constant in
  `core/update/self_update.h` with a sentence; change the number and the sentence together.

## 11. The supervisor

**⚠ cuts the radio's power, and may reflash its application.**

From task p2-05 (`docs/supervisor.md`: the ladder, the four kinds of silence, the waits and the
caps as a table, and what to do when it has stopped; `docs/console.md`, *The supervisor*). Needs:
§3 walked (a radio answering `HELLO`), §5 walked (a real flash through the wire), §8 walked (a
golden image in the store), and **a jumper or a flying lead** to hold the RAK's nRESET to ground,
which is how a radio is made to go quiet on a desk. The host tests prove the ladder against a radio
that misbehaves on cue; only a RAK shows how long a real one takes to come back after each rung,
and that the rail actually collapses and returns.

`rak ladder` is minutes long by design - the waits are the real ones (30, 30, 60 and 120 seconds,
plus the update machine's three-minute proving period at rung 4). Have the console open and let it
run.

- [ ] **Hold nRESET to ground and climb.** With the golden image running and `rak watch` saying
  *healthy*, put the jumper from the RAK's RESET pin to ground and run `rak ladder`. Watch the
  lines: `rung 1, ask again later` after a pause, `rung 2, a reset pulse`, `rung 3, a power cycle`
  (the rail audibly or measurably off for five seconds - put a meter on the 3.3 V rail if one is
  handy), `rung 4, what is on the chip`. With the chip still holding the golden image, rung 4
  should end *the chip holds the golden image: nothing to flash* and the climb should stop:
  `rak ladder: STOPPED: the chip holds the golden image: this is not a firmware fault`. Then
  `rak watch`: the incident, with `rungs tried: 1, 2, 3, 4, 5`, every rung's outcome, the battery
  reading and `today: resets 1/6, power cycles 1/3, golden-image flashes 0/1`. **Write down how
  long the whole climb took** and how long the RAK took to answer after the reset and after the
  power cycle: those are `kAfterResetMs` and `kAfterPowerCycleMs`'s bench numbers.
- [ ] **Remove the jumper during the climb.** Run `rak ladder` again (`rak watch reset-caps` first
  if the counts are spent) and pull the jumper while it waits after rung 2. It should notice at the
  next question and stop climbing: `rak ladder: the radio is back after rung 2, a reset pulse`, with
  no power cycle in the lines and `power cycles 0/3` in `rak watch`. Pull it during rung 1's pause
  instead and it should be cured at rung 1, with **no pin having moved at all**.
- [ ] **Flash the RAK over USB with something else** (stock MeshCore, or NoiseScope's own release
  through the RAK's UF2 bootloader) and then `rak ladder`. Rung 4 should find that the chip differs,
  flash the golden image back through the update machine (its transitions and the flasher's
  progress lines appear inside the rung), prove it, and end *the radio is back after rung 4*.
  `rak hello` then shows the golden image's version, `image list` is unchanged - golden is still
  golden, nothing was promoted, `image bad` is empty - and `rak id` shows **the same public key and
  name as before**, because the file system above the application region was never touched. Then
  `rak watch`: `golden-image flashes 1/1`, and a second such incident the same day should be
  refused at rung 4 with *today's cap is reached*.
- [ ] **Power-cycle the Walter itself and watch it not call that an incident.** Pull the Walter's
  USB (or its battery lead) and plug it back in. The RAK restarts - the ESP32-S3's power-up glitches
  GPIO1-14 low for about 60 µs and that reaches nRESET (`docs/ARCHITECTURE.md` §2 rule 5) - and
  `rak ladder` run immediately afterwards must say *this Walter has just powered up: the radio is
  still booting* and do **nothing**. `rak watch` must show no new incident. Twenty seconds later
  (`kWalterPowerUpGraceMs`) the same command asks properly again. **Write down how long the RAK
  actually takes to answer after the Walter's power-up**: that is this constant's bench number.
- [ ] **A radio running the scanner is left alone.** `rak role scanner` (§9's fourth item), then
  `rak ladder`: it must say *the radio is running the scanner (NoiseScope): not a fault, left
  alone*, move no pin, and record no incident. `rak role repeater` puts it back.
- [ ] **The caps, and that they survive.** With the jumper in, climb until the day's resets are
  spent (`rak watch` counts them); the next climb's rung 2 must be refused - *today's cap is
  reached* - and the climb go straight to rung 3. Then: **press the module's reset** and check
  `rak watch` still shows the same counts (RTC memory); **pull the power** and check the boot line
  says `supervisor: today already: … read back from the journal` and the counts are still there
  (the journal, in flash). `rak watch reset-caps` empties them.
- [ ] **A stop stays stopped.** After a climb that ended *STOPPED*, `rak ladder` is the way past it
  (it lifts the stop first and says so in the journal); without it, the boot lines announce
  `supervisor: STOPPED: <reason>; N s of the quiet period left` at every restart until three days
  have passed. Check the boot line, and check `rak watch`'s *nothing more for N s*.
- [ ] **A boot loop.** Hard to arrange honestly - it wants a repeater build that restarts itself.
  If one exists (a watchdog bite every minute, or a `rak cli` command that reboots on a timer),
  three restarts inside ten minutes must make `rak watch` say *this is a boot loop* and the next
  climb start at rung 3, with rungs 1 and 2 recorded as *skipped*. Otherwise: note it as not walked,
  and watch for `EV_BOOT`s in `rak events` as the nearest thing.
- [ ] **The numbers.** Against the lines above, `docs/supervisor.md`'s table: the three waits
  against how long a real RAK takes to come back; `kKillOffMs` against the rail's measured fall and
  rise (§6's numbers); `kWalterPowerUpGraceMs` against the glitch's real effect; the caps against
  what a mast can afford of its battery in a day; the battery thresholds against the real pack's
  curve under two MCUs awake. Each is a constant in `core/supervisor/supervisor.h` with a sentence;
  change the number and the sentence together.

## 12. The modem

From task p3-01 (`docs/ARCHITECTURE.md` §9; `docs/console.md`, *The uplink*). Needs: **a SIM with
LTE-M data on a Danish network, an LTE antenna on the module's connector**, and §1 walked (a console
on USB). Nothing of this phase can be proven in a session: the policy is tested against a fake modem
that fails in every way a real one does, and every line below is about the real one.

- [ ] **`lte up` with the SIM in and the antenna on.** Expect `lte up: up in <n> ms (up)`, a
  `signal:` line with an RSRP, an RSRQ, an RSSI, the operator's name and a band number, and a
  `time:` line saying *from the network*. Then `lte time` within a second or two of a phone's
  clock, and `lte` showing the same readings without going back to the modem. **Write down the
  attach time and the band**: the band is what the first unit tells us about Danish LTE-M
  coverage, and the time is the first bench number this task needs.
- [ ] **How long an attach takes from cold, three times.** `lte down`, wait a minute, `lte up`,
  three times over, and write down the three numbers. They set `kRegistrationDeadlineMs` (2
  minutes today, a guess) and through it `kWakeBudgetMs` (4 minutes), which is the biggest single
  claim this task makes on the battery. QuickSpot's own example waits five minutes; if a real
  attach here is a minute, both numbers come down and every wake with no coverage gets cheaper.
- [ ] **The antenna off.** Unscrew it and `lte up`. Expect `not-registered` after
  `kRegistrationDeadlineMs` and not before, `reached 'waiting for the network'`, and
  `try again: growing`. Then `lte` again: it must name the number of wakes until the next
  attempt. Do it three or four times in a row and watch the interval double - 1, 1, 2, 4 - which
  is the whole point of the backoff.
- [ ] **No SIM.** Power down, take the card out, power up, `lte up`. Expect `no-sim - no card, or
  it does not answer` **promptly** (seconds, not the registration deadline), `reached 'the SIM'`,
  and `try again: next wake; not sooner (a person has to act)`.
- [ ] **A PIN-locked SIM, if one can be borrowed.** With no PIN provisioned (which is every unit
  until task p3-03), `lte up` must say `sim-locked`, `try again: never automatically`, and **send
  nothing to the card** - the whole point. Check `lte`'s `SIM PIN: none is set` line. Do **not**
  test a wrong PIN on a card anybody needs: the firmware sends it once and the card has three
  tries, and `sdkconfig.defaults` had to set the modem library's command attempts to 1 to keep
  that promise at all.
- [ ] **The current drawn, with the Walter's own supply and a meter in series.** Three numbers,
  each written down: while searching (the antenna off, inside `lte up`), while attached and idle,
  and with the modem down after `lte down`. These are what task p3-04's policy table is built
  from, and the first of them times `kWakeBudgetMs` is what a wake with no coverage costs.
- [ ] **PSM, once there is a reason to.** The default is *off between wakes* (the modem's radio in
  its minimum state) and PSM is a setting that nothing sets yet. When task p3-04 has the interval
  table, build with `kPowerSaving = PowerSavingMode::kPsm` and watch `lte`'s
  `power saving: … granted: sleep up to <n> s, reachable <n> s` - what the **network** granted,
  which is not what was asked for. Then measure the same three currents again. Which of PSM, eDRX
  and an off modem costs least is `spec/SYSTEM.md` §10 item 5 and only this measurement answers it.
- [ ] **A modem that is really silent.** Hard to arrange honestly - the module is inside the
  package. If the reset line can be held, `lte up` must say `modem-silent`, report *the modem was
  reset (the one reset a wake is allowed)*, and not reset it twice.
- [ ] **The deadlines, against the numbers.** Every one is a named constant with a sentence in
  `components/core/uplink/policy.h` and the report lists them; change a number and its sentence
  together. The one to look at first after the attach times is `kModemCallCeilingMs` (30 s, the
  library's own command timeout): if a deaf `AT` really does sit there for 30 seconds, a wake that
  finds a dead modem spends a minute of battery on finding out.

## 13. HTTPS

From task p3-02 (`docs/ARCHITECTURE.md` §9; `docs/console.md`, *HTTPS*; `keys/tls/README.md`).
Needs §12 walked: a SIM, the antenna, `lte up` saying *up*. Nothing of this section could be run
in a session - the client, the download and the fetch are tested against a scripted server through
the firmware's own HTTP, and the one thing a session cannot script is the modem's TLS stack.
**The third item is the test that the rail is real on the modem and not only in the source.**

- [ ] **`trust tls` after the first boot, then after a reboot.** On a fresh unit, before any
  request: both roots `not written yet: written at the next request`. Then `lte up` and
  `http get https://meshmaster.dk/`: the request's trace line is preceded by the roots going in
  (the first request of a unit's life is slower by two `AT+SQNSNVW` and two `AT+SQNSPCFG`), and
  `trust tls` says `written, matches this build's root` for both and `2 root(s) written in this
  boot`. **Press the module's reset and `trust tls` again**: still written, `0 root(s) written in
  this boot` - the record is in RTC memory and the slots were not rewritten. Pull the power
  instead, and they are written once more at the next request, which is the documented cost of a
  power cut.
- [ ] **`http get https://meshmaster.dk/` → 200.** The chain is `meshmaster.dk <- YR1 <- Root YR
  <- ISRG Root X1` (read 2026-10-14; `keys/tls/isrg-root-x1.pem`), held to slot 12 by profile 2.
  Expect `http get: 200 ...`, a `Content-Type`, the first bytes as hex and the time. **Write the
  time down**: it is a connect, a handshake and a small page over LTE-M, and it is what
  `kDefaultTotalMs` (60 s) and the fetch's `kManifestRequestMs` are guesses at.
- [ ] **A host whose chain ends at a root this firmware does not carry → refused.**
  `http get https://example.com/` (its chain ends at DigiCert as of 2026-10; if it has moved to
  ISRG or Sectigo by the time this is walked, any host behind DigiCert, GlobalSign or GoDaddy
  will do - check with `openssl s_client -showcerts` first). Expect *error: http get: no
  response: the modem refused the connection: the host was unreachable or the TLS handshake
  failed*. The modem library at 1.5.0 does not say which; this walk does: the same host answers
  `curl` on the laptop, so the name resolves and the port is open, and what refused it is the
  handshake against a root it does not chain to. **This is the rail on silicon.** If it answers
  200 instead, the profile is not validating and nothing else in this section matters until it
  is: check `AT+SQNSPCFG?` by hand against `WalterModem.cpp`'s `tlsConfigProfile`.
- [ ] **`http get https://expired.badssl.com/` and `https://wrong.host.badssl.com/` → both
  refused.** badssl.com's chain ends at DigiCert, so both are refused for the root before the
  expiry or the name is even reached - which is right but proves less. To test the name check on
  its own, a host behind ISRG with a certificate for another name is needed: any Let's Encrypt
  site reached by one of its other names (a bare IP in the URL will not do; the client wants a
  host). Note which refusal was seen and why.
- [ ] **`http get https://github.com/`** → 200 (or a redirect followed): the chain
  `github.com <- Sectigo E36 <- Sectigo Root E46 <- USERTrust ECC` held to slot 13 by profile 3.
  This is the one host on the other root; a 200 here and on meshmaster.dk is both profiles working.
  If it is refused, the modem's TLS stack may not take an ECC (P-384) root - Sectigo's E46 root
  (`crt.sectigo.com`) as an alternative anchor is noted in the task's report.
- [ ] **`fetch` of the repeater's dry-run release from GitHub, on a test-key build**
  (`tools/verify.ps1 -TestKey`; never on a unit in service). The base URL is the release's
  `https://github.com/twinoak/twinoak.meshcore.repeater-firmware/releases/download/<tag>`. Expect
  the 302 to `release-assets.githubusercontent.com` followed, the six checks passing one by one,
  the ranges with their `206`s, and `fetch: stored`. **Write down the bytes and the seconds**
  (`fetch: N bytes in R range(s) ... T ms`): they are what `kRangeBytes` (64 KB), `kRangeTotalMs`
  (2 min), `kDefaultBudgetBytes` (2 MB) and `kDefaultBudgetS` (10 min) were guessed at, and the
  bytes per second over a real LTE-M link is the number task p3-05's wake budget is built from.
- [ ] **Pull power half-way through the fetch and run it again - it resumes.** `image list` after
  the power cut says `empty; a transfer of sha256 ... stopped at N of M bytes (it can be resumed)`
  with N a multiple of 65536; the second `fetch` says `resuming at N of M bytes already in flash`,
  asks for ranges from N on (`range N-...`), and ends `stored`. Then `image verify candidate`.
- [ ] **A normal build refuses the same fetch at check 1**, after the manifest and its signature
  and before a byte of the image: `fetch: refused at check 1, the signature: the key id names no
  key this build trusts; nothing stored, the slot is empty`. The state every unit is in until the
  release key exists (`keys/README.md`).
- [ ] **`http get https://meshmaster.dk/` twice in a row, twenty times** (task p8-03; the
  **release** build of v0.1.1 or later, which is where the fix is). This is the item v0.1.0 failed
  on 6 November: every connection was socket 1, so the first connection's close landed on the
  second and the second request answered *no reply: the request did not go out* or *http 200 but
  the body did not arrive whole*. Type the two lines back to back, twenty times over:

  ```
  http get https://meshmaster.dk/
  http get https://meshmaster.dk/
  ```

  Expected: **forty answers of 200**, none of them *did not go out* and none *closed before the
  body was complete*. The pairs are the point - a single request always worked, on 0.1.0 too - and
  twenty of them because the fault was a race and some wakes got through. **Write down the two
  times of each pair.** With two sockets no connection should ever wait for a close another one
  owes; if the second of each pair comes out reliably slower than the first by about a quarter of a
  second, that wait is being spent every time - `kOwedCloseWaitMs` in
  `core/net/socket_rotation.h`, and a sign that the second socket id was not granted and the
  firmware is running on one (the report of task p8-03 says how that can happen).
- [ ] **`agent wake` with config, heartbeat, claim and observations all `done`** (task p8-03).
  Four requests in one wake, four connections, alternating between two socket ids. Expected: four
  trace lines with a status each and no step reported as *no reply*; `agent status` afterwards
  showing the wake as a whole. On 6 November the second step of the wake failed every time.
- [ ] **A job claimed, run and its result posted in one wake** (task p8-03; needs a job queued on
  meshmaster.dk for this unit - §18 is where that is set up). Expected: `claim` → the job → `POST`
  of the result, all inside the one `agent wake`, and the job on the server showing a result and
  not an expired lease. This is the expensive half of the 6 November fault: the claim's answer was
  lost after the server had leased job 21026, so the job died with its one attempt.
- [ ] **The rotation's own lines, read once** (task p9-02). After the pairs above, `lte`:

  ```
    sockets: ids 1 and 6, turn and turn about; 42 connections in this boot
      the last few: 1 6 1 6 1 6 1 6
      owed closes: 0 waited for, 0 timed out, 0 written off; 41 discounted; 0 events for an id not in the rotation
  ```

  **Write the whole block down.** It settles three things no host test can:
  - **whether the second socket id was granted at all.** `ids 1 and 6` is the good case;
    `id 1 alone (the library gave out no second one)` is the degraded one, and then every pair
    above is a quarter of a second slower in its second half.
  - **whether the modem answers `+SQNSH` for a close this end asked for.** `discounted` about
    equal to the number of connections means every close arrives one connection late, which is
    what the rule is for; `written off` other than 0 means closes are being lost, and
    `owed closes: N waited for` is N quarter-seconds spent.
  - **`events for an id not in the rotation`** other than 0 means the modem is ringing on a socket
    this firmware never dialled, which would be a reading of the library worth having.
- [ ] **The sixth connection** (task p9-02; the fault of 7 November, twice). On one boot, with
  `lte` read **between each step** and its socket block written down:

  ```
  http get https://meshmaster.dk/
  http get https://meshmaster.dk/
  lte
  agent wake
  lte
  agent wake
  lte
  ```

  On 7 November the stall came after five good connections, twice: *`WalterModem: Command time-out
  (TX) Attempt 1 of 1`* and then, 30.9 s and 31.1 s in, *no reply: the connection closed before the
  body was complete*. Since task p9-02 those two sentences are told apart - a modem that stopped
  answering now says **the modem stopped answering for this socket before the body was complete**,
  and *the connection closed...* means a close really arrived - so the first thing to write down is
  **which of the two sentences it is**. Then, from the `lte` block: **which id the stalled
  connection was on** (count forward from `the last few`), **whether that id owed a close** and
  **whether anything had been written off**. If `agent wake` goes through twice, say so: five
  connections in a wake with the new rule and no stall is the result.
- [ ] **The trace build, if it stalls again.** The stall is one AT command that got no answer for
  the library's whole 30 s command timeout (`CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS`, and
  `CONFIG_WALTER_MODEM_DEFAULT_CMD_ATTEMPTS` is 1, so there is no retransmission). The only thing
  that names the command is the library's own TX/RX trace, which is `ESP_LOGD` and so needs a
  build with the log at debug - the same build as 23 October (`tasks/phase-6/FOLLOWUPS.md`, *Five
  fixes from the first hand-run*):

  ```powershell
  # not a release build, never flashed to a unit in service
  .	oolserify.ps1 -Shell
  # in the container: add to sdkconfig.defaults, build, flash by cable
  #   CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
  #   CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
  ```

  Then `agent wake` until it stalls, and keep the **whole** paste. What it must show, in order:
  1. the last `TX:` line before the 30 s gap - **the command that was not answered**. The reading
     from the code is `AT+SQNSRECV=<id>,<n>`: the request went out (a failed `send` would have
     said *the request did not go out* instead), the first byte of the response had not arrived
     (`r.status == 0`: no status line at all), and the 30 935 ms is the command timeout plus the
     connect;
  2. whether an `RX:` line came at all in those 30 s, and whether it was `+CME ERROR: 4` (which
     this firmware reads as *nothing buffered yet*), `+SQNSRECV: <id>,<n>` with a length, or
     nothing;
  3. whether a `+SQNSRECV: <id>,<n>` **before** the gap declared more bytes than followed it: the
     library's parser then counts the rest of the UART as that payload (`_receivingPayload`,
     `_receivedPayloadSize`) and swallows the next answer whole - and the command timeout is the
     only thing that clears it (`_processModemCMD` sets `_receivingPayload = false`), which would
     explain why the connection after the stall went through in 3.7 s;
  4. `+SQNSH:` lines, with their ids and their timing against our own `AT+SQNSH`: whether the
     modem reports a close for a socket this end closed, and how long after.
- [ ] **A dial refused on an id the modem may still hold** (task p9-02, the third shape of
  7 November: a result's `POST` refused at the dial after 12 532 ms, on the third connection of the
  wake, with the next connection going through in 3.7 s). Watch for
  *the modem refused the connection* on a connection whose id the `lte` block shows as having been
  used two connections earlier, and write down the seconds. If the trace shows `AT+SQNSD` answered
  `ERROR` on an id whose `+SQNSH` had not come, the dial needs that close first and the rotation's
  wait is the place for it - which is a change this task would not make without the trace.
- [ ] **The numbers.** Against the lines above: `kDefaultTotalMs`, `kDefaultPieceMs`
  (`core/net/http_over_tls.h`), `kRangeBytes`, `kRangeTotalMs`, `kRangePieceMs`, `kRangeTries`,
  `kRetryPauseMs`, `kDefaultBudgetBytes`, `kDefaultBudgetS` (`core/net/download.h`),
  `kManifestRequestMs` (`core/update/fetch.h`), `kTrustWriteDeadlineMs` (`core/net/trust.h`),
  `kOwedCloseWaitMs` (`core/net/socket_rotation.h`),
  `kHandshakeDeadlineMs` (`core/update/tls_check.h`), and the socket's `kSocketConnectTimeoutS`
  and `kReceivePollMs` (`components/modem/walter_tls.h`). Each is a constant with a sentence;
  change the number and the sentence together.

## 14. Provisioning and the console

From task p3-03 (`docs/provisioning.md`; `docs/console.md`, *Settings*). Needs §1 walked, and
§§12–13 for the last two items. Everything else here is a laptop, a cable and five minutes, and
it is the one section a person walks on **every** unit before it leaves the bench.

- [ ] **The session of `docs/provisioning.md`, typed.** Line for line, against the document. A
  host test replays the same session against the firmware's own console
  (`host_test/core/console/test_provisioning_doc.cpp`), so what is being checked here is not the
  replies - those are held by the test - but the two things a test cannot see: that **the value
  of `set lte.pin` and `set server.token` echoes as asterisks on a real terminal**, and that the
  session reads as a session when a person is typing it rather than a program.
- [ ] **A power cycle, then `get`: the same answers.** Pull the cable, put it back in, and `get`.
  Every value that was set is still set; `settings: N of 11 keys set` in the boot lines says the
  same number as before. The settings are in the `nvs` partition, which is above the application
  slots (`ARCHITECTURE.md` §11), so this is also the proof that a reflash of the application
  keeps them: flash the application again with `idf.py app-flash` (not a merged image) and `get`
  once more.
- [ ] **`status` fits a terminal window.** Twelve or thirteen lines, each inside 100 characters,
  on an 80-column terminal with no wrapping that hides a line. If one wraps, the line is too
  long and `components/core/console/console.cpp`'s `status()` is where to shorten it. Read it
  once as a stranger would: it should answer *is this unit alive, does it know its radio, has it
  a way out, and when does it next do something*.
- [ ] **`help` fits too, and `dump` says what the boot said.** `help` is long - it is every
  command - but no line of it wraps. Then `dump`: the boot's own lines, then every reply since,
  oldest first, the first line whole. Pull nothing: `dump` is the command a person runs *before*
  asking what happened.
- [ ] **A secret is nowhere.** With a PIN and a token set, run `get`, `status`, `lte`, `journal`
  and `dump` and read the lot: neither value appears. Then, with the board in hand, read the
  `nvs` partition over USB (`esptool.py read_flash 0x610000 0x10000 nvs.bin`, then
  `strings nvs.bin`) and **find both of them**: that is what *NVS is not encrypted* means, and it
  is the reason the boot says so in a line of its own. If a unit is going somewhere that matters,
  turn flash encryption on - which is a decision with its own consequences, not a flag to flip
  here.
- [ ] **`erase confirm`, and what survives it.** With images in the store, something in the
  journal and a day's counts in the supervisor: `erase confirm`, then `get` (every key unset),
  `image list` (the images as they were), `image bad` (as it was), `journal` (its entries), and
  `rak watch` (the day's counts still there - RTC memory, which a `reboot` does not clear).
  Without the word, `erase` refuses and says what the word is.
- [ ] **`set swd.khz`, with a scope on SWDCLK.** `set swd.khz 250`, then `rak swd id`: the half
  period on the scope is 2 µs and the command still identifies the chip. Then `set swd.khz 25`
  (20 µs: slower than the nRF52840 is specified for, and it should still answer through the
  filters) and `set swd.khz 1000` (the wire's floor of 1 µs, so 500 kHz, above
  `BENCH-BREADBOARD.md` §4's predicted ceiling - **this is where the real ceiling is found**).
  Write down the fastest setting at which `rak swd id` is reliable over ten runs and put it in
  §4 beside the other SWD numbers.
- [ ] **`set lte.pin` with the card's real PIN, and with a wrong one** (needs §12: a SIM). With
  the right one, `lte up` attaches. With a wrong one, `lte up` says `sim-locked`, `lte` says the
  PIN was refused, and **a second `lte up` does not send it again** - which is the one rail in
  this firmware that protects something that cannot be undone (three refusals block a card). Then
  `set lte.pin <the right one>`: the line says the refusal is forgotten, and `lte up` attaches.
  Do this with a SIM you can afford to lose, or with one whose PUK you have.
- [ ] **`set server.url` and a self-update's third condition** (needs §13). Set it to a host the
  unit can reach over TLS, then `self status`: the trial's third condition is *a TLS handshake
  with `<host>:443`*. Unset (after `erase confirm`), the same line says the condition does not
  apply. That is the difference between a unit that has a server and one that has not, and it is
  what keeps a factory image from rejecting its own first update.
- [ ] **`set log.level error`, then reboot.** The boot says only the invitation and the prompt;
  `get log.level` still answers, and so does every other command. Then `set log.level info` and
  reboot again: the boot lines are back. A unit on a mast with a noisy port is why this exists.

## 15. Sleep

**★ one of the three that matter most (the scope on nRESET, the second item).**

From task p3-04 (`docs/power.md`; `docs/console.md`, *Sleep, and what wakes it*). Needs §1 and §3
walked; the last two items need §12 (a SIM) and a meter. **This is the section that turns the
interval table's placeholders into numbers**, and until it is walked every number in
`docs/power.md` and `core/power/interval_policy.h` is a guess with a reason beside it.

Needs: a meter that can read microamps and integrate milliamp-hours (a Nordic PPK2, a Joulescope
or a bench supply with an integrating readout), a scope with two channels, a cable to the
repeater's own console, and a battery you can charge and discharge.

- [ ] **The Walter's current asleep, modem off.** `lte down`, then `sleep 300` with the meter in
      series on the Walter's supply. The Walter datasheet's deep-sleep figure is the target;
      anything a few times higher is a pin being driven or a peripheral left on. If it is high:
      `pins` before the sleep (everything should read released), then check whether the modem is
      really down — `WalterModem::begin()` sets its own reset pin's direction and enables the
      chip's deep-sleep pad hold, and that pin is the module's, not one of our seven.
- [ ] **A scope on nRESET and on KILL across sleep entry and wake: no pulse.** Two channels, 1 V
      per division, a timebase that shows the whole transition, triggered on the falling edge of
      nRESET. There should be no edge at all: the pads are configured to the safe state and then
      held, and at the wake they are configured to the same state before the holds come off
      (`docs/ARCHITECTURE.md` §2 rule 5). Then the proof a scope cannot give: `rak status` ten
      sleeps later, and the radio's **uptime still counting** from the same boot.
- [ ] **The radio's line wakes it.** From the repeater's own console, the command that raises
      attention reason 4 (a person asking for the Walter). The Walter wakes inside §8's repeat
      window, and `wake` says *the radio's line: it raised attention* with *a person asked for
      the walter*, the detail and which repeat it heard. Try it with the Walter two minutes into
      a six-hour sleep, so that the wake is unambiguous.
- [ ] **A line that chatters does not keep it awake.** Unplug the RAK's UART (or hold its TX low)
      while the Walter sleeps: it either does not wake at all, or wakes at most three times and
      then says *the radio's line is NOT armed as a wake source* at the boot and in `wake`. Plug
      it back in and wait for a timer wake: the line is armed again, and `journal` has both
      entries.
- [ ] **USB does not wake it, and a boot with a cable stays awake.** With the Walter asleep, plug
      the cable in: **nothing happens** — this chip cannot wake from deep sleep on USB
      (`docs/console.md`). Press reset: the boot says *a USB host is on the port, so this Walter
      stays awake while you are here*. Pull the cable and watch for a minute: nothing sleeps,
      because nothing in this firmware runs a wake yet (task p3-05 is what arms the bound).
- [ ] **`stay` and the bound.** `stay`, then `wake`: it says `stay` is on and how long is left.
      `stay off`: the line about the bound comes back. The bound itself cannot be walked until
      task p3-05 arms it; when it is, a wake that hangs ends in a reset and `reboots` says *the
      wake did not reach sleep inside its bound*.
- [ ] **The energy of one whole wake with an attach** (needs §12). The number that turns the
      policy's placeholders into a table. With an integrating meter: zero it, `lte up`, let the
      attach finish, `lte down`, `sleep 60`, read the milliamp-hours. Three times, from cold.
      Then the same for a wake that only talks to the radio (`rak ping`, `rak power`, `sleep 60`).
      Write both into `core/power/interval_policy.h`'s comment and into `docs/power.md`.
      The INA3221's own readings before and after a wake are the second, cheaper way to the same
      number: `rak power` reads channel 3, the total load.
- [ ] **The pack's voltage against its state of charge.** Charge it full, then run the unit on it
      with the interval forced short (`set wake.interval 15`) and write down `rak power`'s channel
      2 every hour until the repeater browns out. That curve is what the three thresholds are read
      off, and it is the one measurement nothing in these piles could even approximate.
- [ ] **The bands, once the thresholds are real.** With a bench supply standing in for the pack,
      set it to each threshold ±100 mV and read `power`: the band, the interval and what the wake
      may do. Walk it down and then up again and watch the hysteresis hold — the band should
      change once per crossing, not twice.
- [ ] **A day of it.** Leave the unit on the battery and the panel for 24 hours with
      `wake.interval auto`, then read `wake` (the last ten wakes and their durations), `power`
      (the band and the readings) and `journal` (the band changes). What a day cost the pack is
      the difference between the first and the last `rak power`.

## 16. A day

From task p3-05 (`docs/cycle.md`; `docs/console.md`, *One whole wake*). Needs §15 walked, a SIM
(§12) and a provisioned unit (§14). This is the section that turns a firmware whose every wake has
been simulated into one that has actually spent a day looking after a radio.

- [ ] **`cycle dry` with the cable in.** Before anything is left alone: `cycle dry` and read the
      ten steps. Every one should say what it would do and what it is allowed to spend; the
      preconditions should match what the unit is (a radio that answers, a band, a backoff). The
      last lines are the seam: `the backend: nothing to do` is this firmware being honest — there
      is nobody to report to (`spec/SYSTEM.md` §8).
- [ ] **`cycle now` with the cable in, once.** It runs one whole wake and **ends in deep sleep**,
      so the port goes quiet: that is the command working. Press reset to come back, then `wake`
      (the wake before this one, its reason and how long it lasted) and `journal` (the wake's
      record, if it had news). If it comes back *without* sleeping, the last line says why and
      that is a finding.
- [ ] **A day alone.** `set wake.interval 15`, pull the cable, and leave the unit on the battery
      and the panel for 24 hours. Then: `journal` shows one line per wake **that had news** and
      not 96 of them — a quiet day may be two or three lines, which is the design
      (`docs/cycle.md`, *What is journalled*); `wake` shows the last ten wakes, all of them having
      reached sleep; `rak status` shows the RAK's **uptime at a day** and its clock right, which
      together are the whole point: the Walter woke 96 times, set the clock each time, and never
      once reset the radio.
- [ ] **The letters.** Read one wake's record and check each letter against what the unit was
      doing: a day with coverage should be `yy-yyyy--y` or so, and any `x`, `r` or `t` should have a
      reason you can find in `journal` or `rak watch`.
- [ ] **Cover the panel (or lower a bench supply) until the band changes.** The interval follows
      (`power` says which band and what it allows), and in `critical` **the modem is not used at
      all** — `lte` should show no attempt in that wake and the wake record's uplink letter should
      be `s`, not `x`. Uncover it and watch the band come back up with the hysteresis holding.
- [ ] **A radio that goes quiet, for a whole wake.** Pull the RAK's nRESET to ground (§11's own
      item) and let a timer wake happen: the wake record should show the radio `silent`, the
      ladder climbed, and **the uplink still used** — that is the one thing a unit on a mast must
      do when its radio dies. `rak watch` has the incident and the day's counts; the counts must
      be inside the caps.
- [ ] **A wake that overruns.** With the cable out, make a step hang — the easiest is a SIM pulled
      half out, so the modem answers and the card does not. The wake should end in a reset inside
      five minutes; `reboots` then says *the wake did not reach sleep inside its bound*, and the
      next sleep should be **longer** and not shorter.
- [ ] **A day's energy, against the simulated figure.** The sessions' simulated month says
      **421 seconds awake a day at 18 wakes a day** (`tasks/phase-3/reports/05.md`). With an
      integrating meter in series: zero it, leave the unit for a day, and read the milliamp-hours.
      That number against §15's "the energy of one whole wake" is what finally says whether the
      interval table is right.

## 17. The agent

From tasks p4-02, p4-03 and p4-04 (`components/agent/`; `docs/ARCHITECTURE.md` §13;
`docs/console.md`, *The radio, over the box link*, *The agent's uplink* and *The agent's
identity*).

**In this order**, which is the order a person meets them: the unit registered and its token set,
the first call to the server, who this unit says it is, what a heartbeat would carry — and then
the mesh, which needs §3 walked and a **second repeater** on the bench: any MeshCore repeater on
the air, with this unit's key granted on its console (`setperm <this unit's key> 1`), because a
bare status request is answered only to a key the repeater's ACL holds. (A login is the agent's
business, task p4-05, not `rak mesh status`'s.) The uplink's items need §12 and a token;
everything in §18 needs this section first.
- [ ] **The unit registered, before its repeater is** (task p4-04, and meshmaster.dk's
      *Register an exit node*). On meshmaster.dk: *Exit nodes -> Register an exit node*
      (`/exit-nodes/new`), **with no repeater chosen** — a unit may be registered before anybody
      has registered the radio it looks after. It gives a token, once; copy it into
      `set server.token …` at the cable (`docs/provisioning.md`, *The agent's first call*) with
      `set server.url https://meshmaster.dk`. The page then shows the unit *never seen*, with no
      key beside it. Nothing is on the wire yet.
- [ ] **`agent probe` against meshmaster.dk** (task p4-03; §12 walked, a SIM, and a token from
      `/exit-nodes/new` on meshmaster.dk typed once as `set server.token …`, with `set server.url
      https://meshmaster.dk`). `lte up`, then `agent probe`: *-> http 200 ok*, the bytes and the
      milliseconds, and the config printed - the agent's id and label as the register shows them,
      `transport native`, the budget, the poll interval, the server's time within a few seconds
      of a watch. Write the milliseconds in the table below against `kLeastCallMs`: the first
      real figure for what one call costs from a cold socket. `dump` afterwards holds no byte of
      the token.
- [ ] **A wrong token** (task p4-03). `set server.token` to anything else and `agent probe`
      again: *-> http 401 not ok* and *the token was refused* - the status the agent's loop halts
      on (task p4-05) - and no body of the server's printed. Then the right token back.
- [ ] **A host whose chain this firmware does not carry** (task p4-03). `set server.url` to an
      https host whose chain ends at a root `trust tls` does not list (`docs/bench.md` §13 names
      the pair that tells a refused chain from no route), `agent probe`: one `error: agent
      probe: no reply: …` line naming the handshake, not a connection and not a status, and the
      token not on the wire - the modem refused the chain before the request head went. Then
      `server.url` back.
- [ ] **`agent id` shows the RAK's own key and version** (task p4-04). With the radio up
      (`rak hello` first, or after a wake), `agent id`: the `repeater:` line is the key the RAK's
      own console prints for `get public.key`, character for character; the `name` is its
      `get name`; `firmware` is its `ver`'s first word and `built on` is the MeshCore tag inside
      the same reply. Nothing of it was typed on this Walter — that is what the item is for. The
      `job types (13)` line and `server.token set` are the other two to glance at.
- [ ] **The radio block the RAK's console agrees with** (task p4-04). `agent id`'s `radio:` line
      against `rak cli get radio` on the same unit, and against `get radio` typed on the RAK's own
      console: the same four numbers, the frequency to three decimals. Then move one of them on
      the RAK (`set radio 869.400,62.5,8,8` there) and run `agent id` again: it still shows the
      old block, because the key did not change and the block is read once — and after
      `rak cli set radio 869.400,62.5,8,8` **through the link**, the next `agent id` shows the new
      one. That is the `config.apply` path, walked by hand.
- [ ] **`agent heartbeat` against the contract fixture** (task p4-04). Print it and read it beside
      meshmaster.dk's `tests/fixtures/contracts/AgentHeartbeat.json`: the field order, the
      `transport native`, the `board walter`, the radio block, the thirteen job types, and the two
      optional fields — `nextCheckInS` the band's interval in seconds and
      `maxTransactionsThisWake` the band's cap. Write the band `agent id` said beside it. Nothing
      is sent; this is an eye on the bytes before task p4-05 posts them.
- [ ] **A radio with no identity, and what a heartbeat says then** (task p4-04). Before the radio
      has a key — a factory RAK with no application, or `rak role scanner` — `agent id` says *the
      radio has no identity yet*, `job types (0): none`, and `agent heartbeat` prints a body with
      **no** `companionPublicKey`, no `companionFirmware`, no `radio` and `"jobTypes":[]`. That is
      what a unit registered before its repeater sends, and the page must show it **alive and
      *radio not identified*** rather than refusing it. `rak role repeater`, then `rak hello`, and
      the next `agent id` has the key again.
- [ ] **A radio that refuses, told apart from one awaiting its first install** (task p10-03;
      **needs a radio in repeater `v0.1.2`'s lost mode** — that repository's `docs/flashing.md`
      §3a, and its own bench line for making one). `agent id` must say **the radio refuses: its
      identity is lost**, name its evidence (*IDENTITY answered ERR_STATE*, and `caps 0x21` if
      the session carries it), and tell the person to read `docs/flashing.md` §3a and then type
      `rak cli erase` and `rak cli reboot` — and it must **not** say *the radio has no identity
      yet*, which is the sentence for a radio awaiting its install and which is what this
      firmware said at it until 8 November. `agent heartbeat` carries `"radioIdentityLost":1`
      among its `errors` and no `companionPublicKey`; after a wake, meshmaster.dk's exit-node
      page shows `radioIdentityLost 1` in its **Counters** row — read it there, because that row
      is the whole point of the counter. Then the way out, as a person: `rak cli erase`, `rak cli
      reboot`, `rak hello`, `agent id` — a key again, a **new** node, so confirm the key on
      meshmaster.dk and register the repeater again (§3a's *What then*).
- [ ] **The repeater registered, and the unit shown as its uplink** (meshmaster.dk's pile 4.16,
      task 132). With the radio's key in hand (`agent id`), *Register this unit's repeater* on the
      exit node's page: the repeater's own page then shows **this unit as its uplink, at once**,
      and the exit node's page shows the key it is linked by. The first heartbeat after that
      carries `companionPublicKey` — which is the link the server makes, and the first thing to
      check if the two pages disagree about who looks after what.
- [ ] **`rak mesh status <key>`** (task p4-02), `<key>` the neighbour's public key as its own
      `rak id`-equivalent prints it. **One packet on the air, one reply, `client_tx` up by one**:
      the console prints *sent as a flood* with a tag, then *reply after N ms (via a returned
      path, snr …, rssi …)* and the repeater's battery, uptime, noise floor and packet counts,
      then `client_tx A -> A+1 (+1)`. Hold the three against the neighbour's own console
      (`stats-core` on it: its packets received up by one) and against a sniffer if one is up:
      exactly one packet from this unit, flooded, two-byte path hashes. The second run goes
      *direct*: the first reply taught the route (§6.3), and `client_tx` is up by one again.
- [ ] **Silence** (task p4-02). The same command with a key nobody on the air has: sixteen
      seconds, then *no reply: the record says timeout after 16000 ms* — the RAK's own record,
      read with `REPLY_GET` after the Walter's timer ran out — and `client_tx` still up by one:
      the packet went, nothing answered. `rak events` afterwards shows nothing of it: the
      mesh client takes its own events. A third run must not be refused *busy*: the wait was
      ended with `JOB_BEGIN` again.
- [ ] **A password crosses once** (task p4-02; a laptop on the RAK's console or a logic
      analyser on D-sub 8). Not this command's — task p4-05's first `password.set` job is where
      a password first crosses this link. What to hold then: the `SEND_LOGIN` and the
      `SEND_CLI` frames carry it, the RAK's `password now:` reply comes back, and `dump` on the
      Walter afterwards holds neither; the host test of the same reads every buffer back.
- [ ] **`agent obs` and §7's drain** (task p4-04; the second repeater of the mesh items, or any
      node advertising on the air). `agent obs` first: the ring empty, and the radio's own count
      of what is waiting. Then `agent obs pull`: the records go into the ring, and the rows show
      the key, how long ago, the hops, the SNR and the RSSI, and the role — and **no name**, which
      is the thing to check by eye (§7: neither end logs the name a stranger chose). Hold a row's
      key against the advertising node's own, and its SNR against what the RAK's `stats-radio`
      says of the same packet. Then `dump`: no name there either. An advert from the unit's **own**
      repeater must not be in the ring at all — the counts line says how many were dropped as its
      own.

## 18. A day as an exit node

From task p4-05 (`components/agent/agent_backend.h`, `firmware_jobs.h`; `docs/ARCHITECTURE.md`
§13; `docs/console.md`, *The agent behind the seam*). This is where §16's day and §17's agent meet:
a unit that not only looks after its radio but is **asked** to, by meshmaster.dk, and answers.
Needs §16 walked (a provisioned unit that sleeps and wakes on the battery), §17 walked (the uplink
up, a token, `agent id` showing the radio's key), and — for the firmware items — a release of the
*Repeater* product on the server to be offered, **one more Repeater release broken on purpose**
(§9's), and the *Walter* product's own release for the self-update item. The simulated month (300 wakes) is the figure every item is held against; it
printed **185 heartbeats, 49 jobs, 2 offers (both stored), 2 applies (1 committed, 1 rolled back),
8192 bytes downloaded, every wake slept**.

- [ ] ★ **`agent wake` with the cable in, once — the first heartbeat recognised.** `lte up` first,
      then `agent wake`: it runs one pass of the loop through step 8's seam and prints each part —
      the config call (the first boot only), the heartbeat with this wake's intent, the offline
      buffer, the claim, and whatever the claim handed over — then the `WakeReport` and the sleep
      it chose. With nothing queued on the server it is one heartbeat and *nothing to do*;
      `agent jobs` then shows the last jobs empty and `agent state` shows the loop having
      heartbeated once, the poll interval, and no kept image. Nothing should print a token, a
      password or a reply body — glance at the output and then at `dump`.
      **Then the page**: the exit node shows *this* unit, by its repeater's key, last seen now, and
      asleep after the wake ends. **If it does not**, the token belongs to another unit or the
      repeater was never linked (§17's last two items) — and nothing further in this section can
      work until it does, which is why this is one of the three marked ★.
- [ ] **A job for a neighbour.** Queue a `status` job for the second repeater (§17) on the server,
      then `agent wake`. **One packet on the air, `client_tx` up by one**, and the result posted:
      the server's job page shows it `ok` with the neighbour's battery, uptime and packet counts.
      Hold `client_tx` against the runner's transaction count in the `agent wake` output — they
      must be equal, which is the one-send-per-command rail made visible.
- [ ] **A job for the unit's own repeater.** Queue a `telemetry` and a `config.read` for *this*
      unit, then `agent wake`. Both go through the local door: **`client_tx` does not move** and no
      packet is on the air (a sniffer sees nothing), yet both post a result — the telemetry's LPP
      channels, the config without the access list. This is the seam's own §13 table, walked.
- [ ] ★ **A `config.read` of the unit's own repeater, at the budget as it is** (tasks p9-01 and
      p9-05; the **release** build of v0.1.2 or later, which is where both halves of the fix are).
      This is the item v0.1.1 failed on 7 November: job 22032, the scheduled daily read of the
      unit's own repeater — the server's full key list, 34 lines through the local door — reset
      the Walter with `task_wdt: … IDLE0 (CPU 0) … CPU 0: main … Aborting` and a core dump,
      because agent-core waited out the airtime budget with `delayMs(1)` in a loop and a
      millisecond was not a tick. **Change nothing about the budget first**: on meshmaster.dk the
      unit's is *2 a minute, gap 20 s*, which is what made some of those waits 40 s, past the
      30 s watchdog. Turn the repeater's scheduled config read back on (or *Read config* on its
      page), `lte up`, then `agent wake`. **Expected now: seconds, not minutes** — the read walks
      its 34 lines with **no** `waiting … for the airtime budget` line anywhere in it, no reboot,
      the result posted `ok` with the values on the repeater's Configuration tab, and `agent jobs`
      showing the job at **0 transactions**. The simulated figure is **3.4 s**, where the same
      job was 983 s of budget waiting before task p9-05; so **write down how long it took**, and a
      read that still takes minutes means `answersLocally()` is not answering for this radio —
      check `agent id`'s `local door: open` line and the key beside it.
- [ ] **`boot` after it says `power-on` or `software`, never `watchdog`** (task p9-01). Run `boot`
      (the reset-reason line, `docs/console.md`) right after the read finishes. A `watchdog` there
      — even one, even with the job posted `ok` — means some stretch of the wake still holds CPU 0
      for 30 s and is a finding against this task. `journal` must show no `faults_in_a_row` from
      the wake either: nineteen resumed watchdog boots is what 6 November looked like from inside
      the journal.
- [ ] ★ **The radio read before the claim, with no `rak hello` typed** (task p8-04). This is the
      item v0.1.0 failed on 6 November: `agent wake` with no session ran a job for the unit's own
      repeater as an ordinary mesh job, and the association's discover went out on the air from the
      radio to itself. **Power-cycle the unit** so that nothing has asked the radio anything — the
      boot line says `agent: no identity read yet` — then, without typing `rak hello`:

      ```
      lte up
      agent wake
      ```

      Expected, in this order, before the claim: one line `agent: the radio is known (boot
      0x…): a job for this unit's own repeater goes through the local door, and every job type
      this unit serves is offered`, and then the config, the heartbeat and the claim. Queue a
      `telemetry` for *this* unit first and it must go through the local door in that same wake —
      **`client_tx` unmoved** — where on 0.1.0 it would have flooded. `agent id` afterwards shows
      the key and `job types (13)` without a `rak hello` of its own. **If the line says the radio
      was not known** while `rak hello` by hand works, the wake is not opening the session and
      this is the 6 November fault again.
- [ ] **A silent radio, and the unit still answering for itself** (task p8-04). Pull the radio's
      jumper (or leave the slot empty, as §2 does it), then `lte up` and `agent wake`. Expected:
      one line `agent: the radio did not answer a HELLO: only this unit's own job types are
      claimed (twinoak.fw.offer, twinoak.fw.apply, twinoak.fw.state, twinoak.power.cycle)`, a
      heartbeat that goes out all the same with **no `companionPublicKey` and no mesh job type**,
      and — with a `twinoak.fw.state` queued — that job claimed, run and answered. **A mesh job
      queued for this unit must not be claimed in that wake**: the server's job page still shows it
      waiting, with no lease, and it is claimed at the next wake once the radio answers again. A
      power cycle queued as `twinoak.power.cycle` is the one job meant for exactly this state.
- [ ] ★ **An offer, in resumable ranges — and the cable pulled in the middle.** Queue a
      `twinoak.fw.offer` for a Repeater release larger than one wake's download budget, then
      `agent wake` twice. The first wake downloads part of the image, posts `progress`, and
      **posts no result**; `agent jobs` shows the offer still claimed and `agent state` a
      candidate part-stored. The second wake resumes from where the bytes stopped and posts
      `stored`. Write the bytes-per-wake you set against `kWakeDownloadBytes` in the table below:
      the first real figure for what a metered plan allows a wake to pull.
      **Then do it the unkind way**, which is the one that matters (★): queue the offer again for
      a fresh release and, while the first wake is downloading, **pull the antenna or the SIM** (or
      the whole battery) so that the wake dies mid-range rather than ending politely. Power it
      back, `agent wake`: the download must take up again from the bytes already in the candidate
      slot — `agent state` says how many — and the image that finally lands must have the
      manifest's SHA-256, which `agent jobs` shows in the `stored` result. **If the hash is wrong
      after a resume**, a range boundary is off by a byte and nothing before the rig could have
      shown it; stop and report it.
- [ ] **An apply that commits.** With the offered image stored (above), queue a `twinoak.fw.apply`,
      then `agent wake`. It flashes the candidate through §9's machine, health-checks it, and the
      result is `committed`; `rak status` shows the radio on the new version. One update step this
      wake, and no second — S-7, which the wake record's letters show.
- [ ] ★ **An apply that rolls back on a bad candidate.** The same again with a release that is
      **signed and valid but broken on purpose** (§9's deliberate bad image, offered by the server
      instead of pushed over the cable): offer it, then apply it. The flash goes in, the radio does
      not answer as itself, **this firmware's** health check fails it, the golden image comes back
      and proves itself, and the result the server gets is `rolledBack` with the journal in it —
      `ok` as a job, because a rollback is the system working. The page says `rolledBack`, the
      candidate is on the bad list (`image list`), and **a second apply of the same hash is refused
      `badList` with nothing flashed**. If instead the server's verdict decided anything here — a
      `committed` for an image that does not run — that is the most serious finding this section
      can produce.
- [ ] **An apply of the Walter itself, across the trial.** Queue a `twinoak.fw.apply` naming a
      *Walter* release. The wake makes it the next to boot and sleeps; the board reboots into the
      new image and **the result is posted by the *next* boot's wake**, after the trial's third
      condition (one heartbeat `ok`) is met. If the new image never heartbeats, the next boot rolls
      back and the result is `rolledBack` — walk it both ways, with a good release and a broken one.
- [ ] **A power cycle.** Queue a `twinoak.power.cycle`, then `agent wake`: the kill line drops and
      restores the radio within the §6 bounds (a scope on the rail confirms the timing), and a
      `HELLO` proves it came back. The result carries the cycle's outcome; `rak watch` has the
      incident inside the supervisor's caps.
- [ ] **A day as an exit node.** With a release on the server and a job or two queued across the
      day, `set wake.interval 15`, pull the cable, and leave the unit a day as §16 does. Then
      `journal` and `agent jobs`: every job claimed either posted a result or left its lease to
      expire with its bytes kept (an offer mid-download), and `agent state` across the reset shows
      the loop's counters having survived every sleep. Hold the day's heartbeat and job counts
      against the simulated month's rates, and the day's energy against §16's figure — a day that
      ran real jobs costs more than a quiet one, and *how much more* is the number this item brings
      back.
- [ ] **The release a unit is running, on the server's page** (task p7-01). After any wake of a
      board flashed from `v0.1.2`, the exit node's page shows the firmware version it reported —
      `agentVersion`, this firmware's own string, **`0.1.2+<sha7>`** — beside the heartbeat. It is
      the one place a person sees which release a unit out there is actually on, and *LTE test
      node*'s page said `0.0.0-dev` until this one was flashed. Check that the seven characters
      are the tagged commit's.
- [ ] **The band a 1.0 radio does not say** (task p8-05). With **repeater v0.1.0** on the radio -
      the unit's own state this morning - `rak hello` says `band: not said (a 1.0 radio)` on the
      line after the firmware and the board, and `status`'s radio line says `band not said`. Everything else about the
      session is as before: this is the 1.1 Walter talking to a 1.0 radio, which is the pairing
      the whole phase has to keep working.
- [ ] ★ **The band a 1.1 radio does say, and the state read without a golden image** (task p8-05).
      With **repeater v0.1.1** on the radio (the repeater firmware's pile 3.8) `rak hello` says
      `band 868` and `status` says `band 868, link 1.1`. Then, **before** `rak update
      --first-install` has made anything golden, run *Read the unit's state* on meshmaster.dk: the
      slot must report `role repeater`, `board rak4630-single` **and** `band 868`, and *Mark
      canary* must offer the release instead of saying *Read the unit's state first*. This is the
      item job 20990 failed on 6 November, and the band is the only one of the three words that
      was missing. **If the slot still reports no band** while `rak hello` says one, the state
      report is not reading the session - stop and report it.
- [ ] **A scanner slot refuses an apply, and nothing is flashed under it** (task p9-03). ⚠ needs
      `"radio": "disposable"`. `rak role scanner` (§11's scanner item), then queue a
      `twinoak.fw.apply` of a stored Repeater candidate and `agent wake`. Expected: the result
      `refused` with `refusal: link` and the `check` line *the link is down and nobody has said
      why*; `image list` unchanged, the candidate still held, `image bad` still empty, and
      `rak watch` with no new incident. `rak role repeater` puts the radio back and the same apply
      then commits. **If the apply flashes a radio running NoiseScope**, this is the 7 November
      regression again (the review's S1) and the refusal has been lifted by something that is not
      the supervisor.
- [ ] **A dead radio nobody has classified refuses an apply; one the supervisor has given up on
      does not** (task p9-03). Pull the radio's jumper, power the Walter so that the boot is fresh
      (`rak watch` says `radio health: unknown`), then `lte up` and `agent wake` with a
      `twinoak.fw.apply` queued: `refused`, `refusal: link`. Then `rak ladder` until it ends
      *STOPPED* - `rak watch` says `radio health: silent` - and `agent wake` again: the same apply
      now runs the machine and ends `stuck` (there is no radio to prove anything on), which is the
      unbrick path task p2-05 built and the one case where the battery-unknown refusal is lifted
      too. **The difference between the two wakes is a decision of the supervisor's and nothing
      else**, which is what this item is for.
- [ ] ★ **The cable ends the wake an `agent wake` raised** (task p9-04). The item of 23 October
      and again of 7 November: after a bench session of several `agent wake`s, pull the cable and
      watch the current. Expected: the drop from about 41 mA to **8 mA** within a minute or so of
      the cable — the USB grace (`kUsbGraceMs`, 60 s), one reset (the next boot's `boot` line says
      `software` and `reboots` says *the wake overran its bound*), and then the ordinary wake that
      ends in deep sleep. The last lines of each `agent wake` name the figure before you pull:
      *the wake's bound is back to 300 s from the boot (awake N s)* and *pull the cable and this
      wake ends in 60 s*. **If it stays at about 41 mA for a quarter of an hour**, the bound is
      being left raised again and this is the fault back.
- [ ] **The local door, on the console** (task p9-04). After a wake that ran a job for the unit's
      own repeater: `agent id` ends with a `local door:` line — *open: a job naming this radio is
      answered here* and the four counts — and `agent jobs` ends with two blocks of its own,
      `local` (the same four, and this radio's key shortened) and `begun` (the last ten jobs,
      newest first, each saying *the local door: nothing went on the air* or *the mesh*).
      A job for a **neighbour** (§18's second item) must read *the mesh; NOT this radio's key
      …* with both keys shortened, which is the line that would have explained 7 November's
      telemetry job in one glance instead of the server's audit log.
- [ ] ★ **An admin job of the unit's own repeater with no password stored anywhere** (task p9-05).
      This is the refusal of 7 November: job 22016, a scheduled `config.read` of a repeater whose
      settings had been lost, so its admin password is a random one nobody knows (S-13) —
      meshmaster.dk refused the claim with *no admin password stored* and the job never left the
      server. On the repeater's Access tab, **clear the stored admin password** for this unit's own
      repeater; its sentence must then read *This repeater's own unit needs no admin password for
      its own jobs, but Wake it up and the exit nodes reaching it over the mesh still do.* Queue a
      `cli ver` (or *Read config*) for it, `lte up`, `agent wake`. Expected: the claim comes down
      with **no credential at all**, the job ends `ok`, and its log on the server carries
      *answered by the board, no password needed: the empty material at class admin* and **no**
      *nothing to log in with*. A refusal here with *its Walter is older* is the server's version
      gate, not this change — see the version item below.
- [ ] **And a repeater that is not this unit's own still pays for both** (task p9-05). The same
      unit, the second repeater of §17, over the air. A `status` for it must still show the
      `waiting 20 s for the airtime budget` lines, a real transaction count and `client_tx` moving;
      and a `cli` at `admin` for it **with nothing stored** must still come back `unauthorized`
      with *the claim carried no admin material; nothing to log in with* and **nothing on the
      air**. This is the item that catches a door left open — an `answersLocally()` that stays
      true after the job, or an own key that matches too much.
- [ ] **The version meshmaster.dk gates both on** (task p9-05). `agent id`'s first line must say
      `version 0.1.2+…` (or later), and the unit's page on meshmaster.dk must show the same: the
      server reads the heartbeat's `agentVersion` and hands nothing for a unit's own repeater only
      from **0.1.2** (`LocalDoor.NoMaterialFrom`), keeping the stored password and the refusal for
      anything older. A version string the server cannot parse counts as older too, so this is
      worth one glance before the two items above are read as findings.
- [ ] ★ **The link at 1.2, and the request itself** (task p9-06). **Needs the radio at repeater
      v0.1.2 too** (the repeater firmware's pile 3.9, offered over LTE like the Walter's); until
      then every item below reads as the 1.1 line and is not a finding. `rak hello`: the
      `capabilities` line must carry **`local-request`** (bit `0x80`) and the `protocol` line must
      say **1.2** — the session's minor is the highest both ends know, so a 1.2 Walter against a
      0.1.1 radio still says 1.1 and no `local-request`. Then `rak request 01`: *local request 0x01
      (status)*, `ok: 56 byte(s)`, the hex, and the fields by name — battery, uptime, transmit
      queue, noise floor, last RSSI and SNR, packets in and out, airtime, duplicates, errors.
      Compare the battery with `rak power`'s channel 2 and the uptime with `rak status`: they are
      the same radio read two ways, and a mismatch is the layout and not the link. Then
      `rak request 07` (three lines of text: version, name, owner info) and `rak request 06
      000a0000000611223344` (a page of ten neighbours, newest first, six-byte prefixes). And
      `rak request 03`, which must be refused: *the handler answered nothing, which over the mesh
      would have been silence* — telemetry's local form is `rak cli`'s `LPP_GET`, on purpose.
- [ ] ★ **The repeater leaves *stale* on meshmaster.dk** (task p9-06). With both 0.1.2s on the
      unit, `agent id` must say `from LOCAL_REQUEST: status, access list, neighbours, owner info`
      on the line under `local door:`. Then let the schedule run (or press *Read now*): the
      repeater's page must take a `status` and a `neighbours` reading **through its own unit**, at
      **0 transactions** and with nothing on the air, leave *stale*, and start filling the graphs
      a status read feeds — battery, noise floor, packets, airtime. A *Read config* of it must now
      report a real **access list** instead of the note *no ACL in this snapshot*. If the page
      still says *Its own unit answers no status over the box link until its Walter and radio are
      at 0.1.2*, read which half it names: that sentence is the server's version gate
      (`LocalDoor.OlderHalf`) and not this change.
- [ ] **And a radio at 0.1.1 is exactly what it was** (task p9-06). Worth doing **before** the
      radio is updated, which is the state the unit is in the moment v0.1.2 of the Walter lands: a
      scheduled `status` or `neighbours` of the unit's own repeater must come back **`failed`**
      with *the packet could not be built*, a *Read config* must carry the note *no ACL in this
      snapshot, and no evidence either way*, and `agent id` must say `from LOCAL_REQUEST: none -
      this radio is older than link 1.2`. Nothing on the air either way. This is the pairing the
      fleet will be in for as long as it takes to offer the radio its own update, so it has to be
      the old behaviour and not a new failure.
- [ ] **The clock check runs when the uplink has been up** (task p9-04). `rak update` typed in a
      boot where nothing has given the Walter a time prints, after the outcome, *the clock check
      was skipped: this Walter had no time of its own in this boot*. Do `lte up` first and the
      line is gone: `lte up` now says *this Walter's clock is set from it, so the radio can be
      given it too*, and the proof's step 2 sends a real `TIME_SET` (`rak watch`'s clock line says
      *was set*). The old message said *until phase 3's modem*, which has been wrong since
      phase 3.
- [ ] **The journal names the image of a run's first entry** (task p9-04). `rak update status`
      after any run: the oldest entry of it, `check`, carries the candidate's `sha256` and not
      `0000000000000000…`. On the first unit the 6 November run's `seq 28` was all zeros and the
      journal never said which image that run was about.
- [ ] **The Walter's own release over LTE, 0.1.1 to 0.1.2** (task p9-07). The item this release
      exists to make possible, and **the first one that would put a new image on a unit with no
      cable in it**. *Offer a release* on the unit's page, product `twinoak-walter-firmware`,
      release `v0.1.2`, with a reason; then `agent wake` (or wait for one). Expected, in this
      order: the six checks in the console's `fetch` lines, **twelve ranges** of 64 kB for about
      750 kB, a `progress` line on the job's page every 128 kB, and the result `stored` with the
      manifest's hash — then `self status` saying `received: 0.1.2 … waiting for 'self switch'`.
      **Write down the wall-clock seconds of the download and the wake's own elapsed time**: the
      Walter's slot does **not** resume across wakes (`SelfUpdate::beginWrite` erases it), so the
      whole file has to fit one wake's twenty minutes, and this is the first measurement of whether
      it does on a real LTE-M plan. If the wake's budget cuts it short, the job's message says *the
      next wake continues it* and for the Walter that is wrong — the next wake starts at zero;
      note the bytes that were paid for twice.
      Then *Apply the candidate*: the board restarts, the trial's three conditions hold inside the
      next boot's own wake, and that same exchange posts `committed` with `0.1.2+<sha7>` — the
      banner, `agentVersion` on the page and `self status` all saying it, and `self status` `valid`
      rather than `pending-verify`. **If the trial rolls back**, the previous image returns by
      itself and the page says `rolledBack` with the journal: read it before flashing anything by
      cable, because the rollback working is the better half of this item.

## What to bring back

Every named constant this firmware carries that **no measurement has settled** - the ones the
reports listed as *to be set on the bench*, and the ones the source marks `PLACEHOLDER`. 115 of
them, in 21 files. Each has a sentence beside it where it is declared; **change the number and the
sentence together**, and nowhere else - the host tests, the console's help and the documents all
follow the constant.

`tools/check_rails.py` holds this table to the source on every verified commit: a constant marked
for the bench with no row here, or a row naming a constant the source no longer marks, fails the
build. So the list cannot go stale, and a number that has been settled leaves the table by losing
its mark in the source - which is the same edit as writing the measurement down.

Write the measurement in the third column, with the date, and then make the edit.

**`board/radio_pins.h`** (1) - **§6** measures the rail's fall under load and its settle time, on
a scope.

| Constant | Now | Measured |
|---|---:|---|
| `kKillTiming` | `1000, 30000, 200` | |

**`provision/provision.h`** (1) - **§4a** measures how long the Adafruit bootloader takes to come
up after the pin reset before the debug port answers again.

| Constant | Now | Measured |
|---|---:|---|
| `kBootloaderSettleMs` | `1500` | |

**`update/radio_update.h`** (9) - **§9** measures a real repeater from reset to HELLO, and how
long a bad build takes to show itself.

| Constant | Now | Measured |
|---|---:|---|
| `kBootWindowMs` | `20000` | |
| `kBootHelloIntervalMs` | `2000` | |
| `kProvingPeriodMs` | `180000` | |
| `kProvingPingIntervalMs` | `3000` | |
| `kScannerWindowMs` | `20000` | |
| `kBatteryMinMv` | `3700` | |
| `kBatteryFallingMvPerHour` | `-100` | |
| `kStepIdleMs` | `100` | |
| `kEntriesPerRunMax` | `48` | |

**`update/self_update.h`** (4) - **§10** measures the trial, from the boot line to confirmed, with
a real attach in it.

| Constant | Now | Measured |
|---|---:|---|
| `kTrialWindowMs` | `300000` | |
| `kTrialHelloIntervalMs` | `2000` | |
| `kTrialUplinkRetryMs` | `30000` | |
| `kSwitchExpectBackS` | `60` | |

**`supervisor/supervisor.h`** (20) - **§11** measures how long a real RAK takes to answer after
each rung, and what a day can afford.

| Constant | Now | Measured |
|---|---:|---|
| `kHeartbeatIntervalMs` | `60000` | |
| `kAskAgainPauseMs` | `30000` | |
| `kAfterResetMs` | `30000` | |
| `kAfterPowerCycleMs` | `60000` | |
| `kAfterGoldenImageMs` | `120000` | |
| `kKillOffMs` | `5000` | |
| `kResetsPerDay` | `6` | |
| `kPowerCyclesPerDay` | `3` | |
| `kFlashesPerDay` | `1` | |
| `kCapWindowMs` | `86400000` | |
| `kCapWindowS` | `kCapWindowMs / 1000` | |
| `kQuietPeriodMs` | `3u * 24 * 60 * 60 * 1000` | |
| `kBootLoopWindowMs` | `600000` | |
| `kBootLoopBoots` | `3` | |
| `kBootLoopUptimeS` | `120` | |
| `kWalterPowerUpGraceMs` | `20000` | |
| `kPowerCycleMinMv` | `3500` | |
| `kFlashMinMv` | `update::kBatteryMinMv` | |
| `kStepIdleMs` | `update::kStepIdleMs` | |
| `kEntriesPerClimbMax` | `32` | |

**`update/last_good_battery.h`** (1) - **§11** measures the pack's curve: how old a reading may be
and still authorise a reflash.

| Constant | Now | Measured |
|---|---:|---|
| `kBatteryStaleAfterMs` | `6u * 60 * 60 * 1000` | |

**`uplink/failure.h`** (1) - **§12** measures how long a unit with no coverage should wait before
searching again.

| Constant | Now | Measured |
|---|---:|---|
| `kBackoffCapWakes` | `16` | |

**`uplink/policy.h`** (23) - **§12** measures three cold attaches, and the current drawn while
searching.

| Constant | Now | Measured |
|---|---:|---|
| `kPresentDeadlineMs` | `10000` | |
| `kModemResetDeadlineMs` | `32000` | |
| `kOpStateDeadlineMs` | `15000` | |
| `kSimDeadlineMs` | `10000` | |
| `kRatDeadlineMs` | `10000` | |
| `kPdpDeadlineMs` | `10000` | |
| `kPowerSavingDeadlineMs` | `10000` | |
| `kAttachDeadlineMs` | `15000` | |
| `kRegistrationDeadlineMs` | `120000` | |
| `kRegistrationPollMs` | `1000` | |
| `kDataDeadlineMs` | `15000` | |
| `kDataTries` | `5` | |
| `kDataRetryMs` | `2000` | |
| `kTimeDeadlineMs` | `10000` | |
| `kTimeTries` | `3` | |
| `kTimeRetryMs` | `2000` | |
| `kSignalDeadlineMs` | `10000` | |
| `kIdentityDeadlineMs` | `15000` | |
| `kTakeDownDeadlineMs` | `10000` | |
| `kWakeBudgetMs` | `240000` | |
| `kModemCallCeilingMs` | `30000` | |
| `kPsmTauSeconds` | `3600` | |
| `kPsmActiveSeconds` | `60` | |

**`net/download.h`** (9) - **§13** measures the bytes and the seconds of a real release fetched
over LTE-M.

| Constant | Now | Measured |
|---|---:|---|
| `kRangeBytes` | `64 * 1024` | |
| `kRangeTotalMs` | `120000` | |
| `kRangePieceMs` | `15000` | |
| `kRangeTries` | `3` | |
| `kRetryPauseMs` | `2000` | |
| `kMinRangeSeconds` | `10` | |
| `kDefaultBudgetBytes` | `2u * 1024u * 1024u` | |
| `kDefaultBudgetS` | `600` | |
| `kMaxValidatorLength` | `128` | |

**`net/http_over_tls.h`** (2) - **§13** measures a connect, a handshake and a small page over
LTE-M.

| Constant | Now | Measured |
|---|---:|---|
| `kDefaultTotalMs` | `60000` | |
| `kDefaultPieceMs` | `15000` | |

**`net/socket_rotation.h`** (1) - **§13** measures how long a close an id still owes takes to
arrive, which is what a connection waits for before it goes ahead without it (task p8-03).

| Constant | Now | Measured |
|---|---:|---|
| `kOwedCloseWaitMs` | `250` | |

**`net/trust.h`** (1) - **§13** measures how long the modem takes to take two roots into its
slots.

| Constant | Now | Measured |
|---|---:|---|
| `kTrustWriteDeadlineMs` | `20000` | |

**`update/fetch.h`** (1) - **§13** measures the manifest and its signature over LTE-M.

| Constant | Now | Measured |
|---|---:|---|
| `kManifestRequestMs` | `60000` | |

**`update/uplink_check.h`** (1) - **§14** measures a TLS handshake with the configured server.

| Constant | Now | Measured |
|---|---:|---|
| `kUplinkCondition` | `UplinkCondition::kTlsHandshake` | |

**`cycle/wake_reason.h`** (1) - **§15** measures a chattering line, and how often a real one is
spurious.

| Constant | Now | Measured |
|---|---:|---|
| `kSpuriousPerWindow` | `3` | |

**`power/interval_policy.h`** (14) - **§15** measures the pack's voltage against its state of
charge, and the energy of one wake.

| Constant | Now | Measured |
|---|---:|---|
| `kUpperMv` | `4000` | |
| `kLowerMv` | `3600` | |
| `kFloorMv` | `3400` | |
| `kHysteresisMv` | `60` | |
| `kChargingMa` | `50` | |
| `kPlentyMinutes` | `15` | |
| `kNormalMinutes` | `60` | |
| `kSavingMinutes` | `360` | |
| `kCriticalMinutes` | `1440` | |
| `kUnknownMinutes` | `kNormalMinutes` | |
| `kSavingModemEveryNth` | `4` | |
| `kMaxFaultDoublings` | `4` | |
| `kSamplesKept` | `5` | |
| `kMedianOf` | `3` | |

**`cycle/telemetry_ring.h`** (1) - **§16** measures how many wakes of readings a quiet day has to
hold.

| Constant | Now | Measured |
|---|---:|---|
| `kReadingsKept` | `10` | |

**`cycle/wake_cycle.h`** (17) - **§16** measures what a wake with an attach costs the pack, and
how long each step really takes.

| Constant | Now | Measured |
|---|---:|---|
| `kWakeBudgetMs` | `150000` | |
| `kLeastMs` | `1000` | |
| `kWhyBoundMs` | `kAttentionWindowMs + 1000` | |
| `kRadioBoundMs` | `40000` | |
| `kLadderBoundMs` | `360000` | |
| `kUpdateBoundMs` | `1200000` | |
| `kTrialBoundMs` | `update::kTrialWindowMs + 60000` | |
| `kStateBoundMs` | `15000` | |
| `kUplinkBoundMs` | `kWakeBudgetMs` | |
| `kUplinkLeastMs` | `20000` | |
| `kClockBoundMs` | `10000` | |
| `kBackendBoundMs` | `kUpdateBoundMs` | |
| `kBackendLeastMs` | `30000` | |
| `kSleepBoundMs` | `15000` | |
| `kClockSkewS` | `60` | |
| `kDownloadMinutes` | `5` | |
| `kSlicesMax` | `5000` | |

**`cycle/wake_guard.h`** (1) - **§16** measures the longest honest wake: an attach and a download.

| Constant | Now | Measured |
|---|---:|---|
| `kWakeBoundMs` | `5u * 60 * 1000` | |

**`components/agent/include/walter_agent/modem_uplink.h`** (1) - **§17** measures what one call to
meshmaster.dk takes over LTE-M from a cold socket (the first `agent probe`): the least of a wake's
uplink deadline a call is begun with.

| Constant | Now | Measured |
|---|---:|---|
| `kLeastCallMs` | `5000` | |

**`components/agent/include/walter_agent/agent_backend.h`** (1) - **§18** measures what an LTE-M
plan may spend on a firmware download in one wake: the most an offer fetches before it stops at a
resumable range boundary and the next wake continues. A megabyte lets a radio image in one wake;
a tight plan is given less.

| Constant | Now | Measured |
|---|---:|---|
| `kWakeDownloadBytes` | `1048576` | |

**`components/agent/include/walter_agent/identity.h`** (5) - **§17** measures what one wake with
an attach and a job actually costs the pack, which is what the per-band transaction cap promises
the server (`maxTransactionsThisWake`). Each is written in the copied library's own arithmetic
(the dearest-job figure above them is the library's own and is not a bench number), so what to
bring back is *how many transactions a wake in each band can pay for* and not the arithmetic.

| Constant | Now | Measured |
|---|---:|---|
| `kPlenty` | two claimed jobs at the dearest job's price | |
| `kNormal` | `kPlenty / 2` | |
| `kSaving` | `mm::kDefaultConfigKeyCount + mm::kConfigReadExtraCount + 1 + mm::kTransactionsForLogin + mm::kTransactionsForAclRestore` | |
| `kCritical` | `0` | |
| `kUnknown` | `kNormal` | |
