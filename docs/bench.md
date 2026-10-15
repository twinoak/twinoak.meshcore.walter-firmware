# The bench

What no session could check, in the order the rig is brought up (`spec/SYSTEM.md` §7,
`hardware/pcb/BENCH-BREADBOARD.md` §7): the Walter alone, then UART, reset, SWD, flash, then the
kill line and I2C. Each task adds its lines under the step it needs; the task's report says the
same under *Not verified here*. Tick here when walked, with the date.

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
  lte-m, version 0.0.0-dev, link 1.0` (task p0-02), then `boot 1: power-on reset`, `the boot
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
  self-update (task p2-04, §9) must be re-read against what the board does.

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
  lte-m, version <the manifest's version>, link 1.0` — the same role, board, band and version the
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

- [ ] **`trust` says no update will be accepted** (task p2-01). On a normal build the console
  prints `ed25519 self-test (RFC 8032 test 1, and one bit changed): passed, <n> ms` — **write
  `<n>` down**: it is the first measurement of TweetNaCl's Ed25519 on the ESP32-S3, and a
  `FAILED` here means the vendored arithmetic is wrong on the board's compiler — then `trust:
  none: no update will be accepted (keys/release-keys.txt names no key; ...)`, and the boot
  lines carry no `WARNING`. That is the unit's state until Thomas makes the release key
  (`spec/SYSTEM.md` §10 item 1, `keys/README.md`); after the key is in `keys/release-keys.txt`
  and a build is flashed, `trust` prints `1 key this build accepts a release signed with:
  <key id>` — the id `manifestref.py keygen` printed. If a board on the bench is ever flashed
  with a test-key build (`tools/verify.ps1 -TestKey`), its banner says `+TESTKEY`, the fourth
  boot line is a `WARNING` and `trust` lists `c17969deb0026955`: never leave such a build on a
  unit that goes out.

## 2. The pins

Needs: the Walter on USB, a meter, then a scope, then the RAK4631 on the RAK19001 with the UART,
nRESET and SWD wired through 470 Ω and 2.2 nF (`BENCH-BREADBOARD.md` §3 and §4), the kill emulator
of §5 last. The firmware's side is `components/core/board/radio_pins.cpp`; the rules are
`ARCHITECTURE.md` §2; what the host tests proved is in `tasks/phase-0/reports/03.md`.

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

- [ ] **`rak hello`** (task p1-02). Prints `protocol 1.0`, `role 1 (repeater)`, `slot 0`, `max
  payload 240`, a `boot_id`, the RAK's firmware, board and upstream versions, and its capability
  bits by name. Hold them against what the RAK's own console says (`stats-core`, its banner): the
  same version, the same board. Then `rak ping` — the nonce comes back, the uptime grows between
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

## 5. Flashing

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

## 6. The slot

Task p1-06 adds **no bench step**, and that is its result. A unit has one radio, slot 0, and every
`rak …` command means that slot without saying so; the dual adapter it makes room for does not
exist, and the firmware constructs no object that can steer one (`ARCHITECTURE.md` §2 rule 6). Two
lines are worth typing once while the rig is up, because they cost nothing:

- [ ] **`rak @0 hello` prints what `rak hello` prints, to the byte**, and `rak @1 hello` answers
  *error: slot 1: this adapter has 1 radio, so slot 0 is the only one* — with no frame on the
  UART and nothing on the seven pins. `rak @2 hello` is refused by the parser instead.

- [ ] **Every other item of §§1–5 behaves as it reads.** Nothing in this task changes a pin, a
  timing or a command's output except `rak flash-test confirm`, whose first line now begins
  `flash-test: slot 0, rak4630-single, …`.

When a dual adapter is built, this is where its bring-up goes: the selector programmed with a
scope on SWDCLK and nRESET (a window of 100–500 µs, two edges, and *no* reset at either module's
pin), the power-on default read back as module A, and `rak @1 hello` answering from the second
radio. None of it is verifiable today.

## 7. Images

From task p2-02 (`docs/ARCHITECTURE.md` §6, `docs/console.md` *Loading a radio image over USB*).
Needs: a Walter on USB, `pip install pyserial==3.5` on the laptop, and a **test-key build** of this
firmware (`tools/verify.ps1 -TestKey`, flashed as §1's first item) together with a repeater release
signed with the specification's public test key — the dry run `tools/verify.ps1` leaves in
`out/release/` is one, for the *Walter's* own image; a repeater release has to come from the
repeater repository's own dry run, or be made by hand with
`python spec/tools/manifestref.py` over a repeater image. Until the release key exists
(`spec/SYSTEM.md` §10 item 1) a normal build accepts nothing, which is the point of the second
item below.

- [ ] **The store says what it is, on a board.** `image list` on a fresh unit prints the layout —
  `2 slots x 3 images of 815104 bytes (199 sectors)`, `4923392 bytes of a 6291456-byte partition,
  1368064 left` — then `slot 0, rak4630-single` with three `empty` lines, then `slot 1: no radio in
  this adapter`. The boot lines have gained `image store: rakfw at 0x630000 (6144 KB), …`. If
  either says something else, the partition table and the firmware disagree and nothing below is
  worth doing.

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
  without `-DTWINOAK_TRUST_TEST_KEY=1`; `trust` prints `trust: none: no update will be accepted
  (keys/release-keys.txt names no key; …)`, and the push ends with *check 1, the signature: the key
  id names no key this build trusts* — with nothing stored and no sector erased. This is the state
  every unit is in until the release key exists, and it is the right one.

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

Nothing here flashes a radio: putting a stored image on the RAK is §8's.

## 8. Updating the radio

From task p2-03 (`docs/update.md`, `docs/console.md` *The radio's update*). Needs: §5 walked (a
real flash through the wire, the bootloader starting what was flashed), §7 walked (a repeater
release in the store), the RAK's own USB console on the laptop, and **two repeater releases
signed with the key this build trusts** - a good one and, for the second item, one that is valid
and signed but **broken on purpose**: a build whose link never comes up (the repeater repository
can make one by leaving the link task out, or by a `fw_version` in the descriptor that is not the
one the firmware reports). On a test-key build both are signed with the specification's test key.
With task p1-05's flashing session this is the one that matters most: the host tests prove the
procedure against a simulated radio that misbehaves on cue; only a RAK can show that a real
repeater boots inside the window, takes the clock, and stays up for the period.

- [ ] **The good path, watched.** With the golden image from §7 running on the RAK and a newer
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
  bootloader until the resumed flash finishes (§5's item 6), which is right.
- [ ] **A role switch and back keeps the identity.** `image receive scanner` with a NoiseScope
  release, then `rak role scanner`: `-> done: a scanner is running`, `rak hello` says *the radio is
  running the scanner*, `rak text` shows its `info` line. Then `rak role repeater`: the golden image
  flashed back through the rolling-back states, `-> done: the golden image <version> is back and
  proved`, and `rak id` shows **the same public key and name as before** - the file system above
  the application region was never touched. `image list`: golden and candidate as they were,
  `image bad` empty.
- [ ] **`rak update --first-install` on a module whose application was wiped** (after §5's item 6,
  or `rak flash-test confirm`'s region). `rak update` is refused (*there is no golden image to return
  to*); with `--first-install` the candidate is flashed, proved and becomes golden. Do not try the
  broken candidate this way unless you mean to see *stuck: no image to return to*.
- [ ] **The numbers.** Against the lines above, `docs/update.md`'s table: the boot window (how long
  a real repeater takes from reset to `HELLO`; the window must be comfortably more), the proving
  period (long enough to catch a build that restarts itself after its first advert; short enough
  that a wake cycle can afford it), the battery threshold against the real pack's curve. Each is a
  constant in `core/update/radio_update.h` with a sentence; change the number and the sentence
  together.

## 9. Updating the Walter

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

## 10. The supervisor

From task p2-05 (`docs/supervisor.md`: the ladder, the four kinds of silence, the waits and the
caps as a table, and what to do when it has stopped; `docs/console.md`, *The supervisor*). Needs:
§3 walked (a radio answering `HELLO`), §5 walked (a real flash through the wire), §7 walked (a
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
- [ ] **A radio running the scanner is left alone.** `rak role scanner` (§8's fourth item), then
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
  rise (§2's numbers); `kWalterPowerUpGraceMs` against the glitch's real effect; the caps against
  what a mast can afford of its battery in a day; the battery thresholds against the real pack's
  curve under two MCUs awake. Each is a constant in `core/supervisor/supervisor.h` with a sentence;
  change the number and the sentence together.

## 11. The modem

From task p3-01 (`docs/ARCHITECTURE.md` §9; `docs/console.md`, *The uplink*). Needs: **a SIM with
LTE-M data on a Danish network, an LTE antenna on the module's connector**, and §1 walked (a console
on USB). Nothing of this phase can be proven in a session: the policy is tested against a fake modem
that fails in every way a real one does, and every line below is about the real one.

The task's own list names this section §10; that number was already the supervisor's, so it is §11.

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

## 12. HTTPS

From task p3-02 (`docs/ARCHITECTURE.md` §9; `docs/console.md`, *HTTPS*; `keys/tls/README.md`).
Needs §11 walked: a SIM, the antenna, `lte up` saying *up*. Nothing of this section could be run
in a session - the client, the download and the fetch are tested against a scripted server through
the firmware's own HTTP, and the one thing a session cannot script is the modem's TLS stack.
**The third item is the test that the rail is real on the modem and not only in the source.**

The task's own list names this section §11; that number is the modem's, so it is §12.

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
- [ ] **The numbers.** Against the lines above: `kDefaultTotalMs`, `kDefaultPieceMs`
  (`core/net/http_over_tls.h`), `kRangeBytes`, `kRangeTotalMs`, `kRangePieceMs`, `kRangeTries`,
  `kRetryPauseMs`, `kDefaultBudgetBytes`, `kDefaultBudgetS` (`core/net/download.h`),
  `kManifestRequestMs` (`core/update/fetch.h`), `kTrustWriteDeadlineMs` (`core/net/trust.h`),
  `kHandshakeDeadlineMs` (`core/update/tls_check.h`), and the socket's `kSocketConnectTimeoutS`
  and `kReceivePollMs` (`components/modem/walter_tls.h`). Each is a constant with a sentence;
  change the number and the sentence together.

## 13. Provisioning and the console

From task p3-03 (`docs/provisioning.md`; `docs/console.md`, *Settings*). Needs §1 walked, and
§§11–12 for the last two items. Everything else here is a laptop, a cable and five minutes, and
it is the one section a person walks on **every** unit before it leaves the bench.

The task's own list names this section §12; that number is HTTPS's, so it is §13.

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
  `docs/bench.md` §4 beside the other SWD numbers.
- [ ] **`set lte.pin` with the card's real PIN, and with a wrong one** (needs §11: a SIM). With
  the right one, `lte up` attaches. With a wrong one, `lte up` says `sim-locked`, `lte` says the
  PIN was refused, and **a second `lte up` does not send it again** - which is the one rail in
  this firmware that protects something that cannot be undone (three refusals block a card). Then
  `set lte.pin <the right one>`: the line says the refusal is forgotten, and `lte up` attaches.
  Do this with a SIM you can afford to lose, or with one whose PUK you have.
- [ ] **`set server.url` and a self-update's third condition** (needs §12). Set it to a host the
  unit can reach over TLS, then `self status`: the trial's third condition is *a TLS handshake
  with `<host>:443`*. Unset (after `erase confirm`), the same line says the condition does not
  apply. That is the difference between a unit that has a server and one that has not, and it is
  what keeps a factory image from rejecting its own first update.
- [ ] **`set log.level error`, then reboot.** The boot says only the invitation and the prompt;
  `get log.level` still answers, and so does every other command. Then `set log.level info` and
  reboot again: the boot lines are back. A unit on a mast with a noisy port is why this exists.

## 14. Sleep

From task p3-04 (`docs/power.md`; `docs/console.md`, *Sleep, and what wakes it*). Needs §1 and §3
walked; the last two items need §11 (a SIM) and a meter. **This is the section that turns the
interval table's placeholders into numbers**, and until it is walked every number in
`docs/power.md` and `core/power/interval_policy.h` is a guess with a reason beside it.

The task's own list names this section §13; that number is the console's (p3-03), so it is §14.

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
- [ ] **The energy of one whole wake with an attach** (needs §11). The number that turns the
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
