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
  times. Every time the same banner and `running from ota_0`, and the ota state stays `undefined`:
  an image flashed over USB is in no OTA state, so the rollback the bootloader does for an updated
  image (pending-verify → aborted at the second boot) does not apply to it. If the second boot ever
  says `aborted` or falls to the bootloader's `No bootable app partitions`, the premise in
  `tasks/phase-0/reports/01.md` (rollback) is wrong and task 02/04 must mark the image valid at boot.

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

- [ ] **`rak kill 5`** (task p0-03). With the LED emulator first (`BENCH-BREADBOARD.md` §5 phase
  1), then the P-FET switch: scope on TX (GPIO7), nRESET and SWDCLK against KILL (GPIO5). TX
  drops from its pulled-up high to floating (a 10 kΩ to ground on it would show it) and nRESET,
  SWDCLK, SWDIO are undriven **before** KILL rises; KILL is high for 5 s and then floats (it never
  goes low-driven: the 100 k pull-down on the A-board's Q3 gate is what takes it down); the LoRa
  rail falls and returns by itself. While it is down nothing is back-fed: the rail sits at 0 V, not
  at a diode drop (about 0.6 V would mean something of ours is pushing current into the radio).
  After it, `status` says `radio power: on` and TX is pulled up again. `rak kill 0` and `rak kill
  31` are refused with the bounds and touch no pin.

- [ ] **Reset the Walter while a kill is in progress** (task p0-03). `rak kill 30`, then press
  the Walter's reset within those 30 s: the kill line floats the moment the S3 resets, the rail
  comes back by itself, and the new boot says `boot n: reset pin reset`; `status` says `on` after
  200 ms. Nothing of ours holds the radio off across a reset.

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

- [ ] **`rak flash-test`: passes; the RAK still boots what it had** (task p1-05). Expected: the
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
  timing or a command's output except `rak flash-test`, whose first line now begins `flash-test:
  slot 0, rak4630-single, …`.

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
  or `rak flash-test`'s region). `rak update` is refused (*there is no golden image to return
  to*); with `--first-install` the candidate is flashed, proved and becomes golden. Do not try the
  broken candidate this way unless you mean to see *stuck: no image to return to*.
- [ ] **The numbers.** Against the lines above, `docs/update.md`'s table: the boot window (how long
  a real repeater takes from reset to `HELLO`; the window must be comfortably more), the proving
  period (long enough to catch a build that restarts itself after its first advert; short enough
  that a wake cycle can afford it), the battery threshold against the real pack's curve. Each is a
  constant in `core/update/radio_update.h` with a sentence; change the number and the sentence
  together.
