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
