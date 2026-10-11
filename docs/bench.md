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
  state undefined (...)`, then `console on USB; nothing to do yet - idling`. `--flash-mode dio` is
  esptool's way in; the image's header says QIO and the bootloader switches to it
  (`sdkconfig.defaults`).

- [ ] **A reset runs the same image** (task p0-01; S-6). Press reset, or unplug and replug, three
  times. Every time the same banner and `running from ota_0`, and the ota state stays `undefined`:
  an image flashed over USB is in no OTA state, so the rollback the bootloader does for an updated
  image (pending-verify → aborted at the second boot) does not apply to it. If the second boot ever
  says `aborted` or falls to the bootloader's `No bootable app partitions`, the premise in
  `tasks/phase-0/reports/01.md` (rollback) is wrong and task 02/04 must mark the image valid at boot.

- [ ] **Nothing else on the pins** (task p0-01, until task 03). With a meter: GPIO1 (nRESET), 2
  (SWDCLK), 4 (SWDIO), 5 (KILL), 7 (TX) read as inputs (high-impedance, pulled by nothing of ours)
  after boot. Task 03 owns their rules; this only confirms that task 01's image drives none of them.
