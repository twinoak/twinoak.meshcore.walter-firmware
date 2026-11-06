# 01 — The flasher lets the idle task run

**Depends on:** nothing in this phase.
**Spec:** `sdkconfig.defaults` (`CONFIG_ESP_TASK_WDT_*`: 30 s, panic, the idle tasks of both cores watched); `components/platform/clock.cpp` (`EspClock::delayMs`: below one tick, 10 ms at `CONFIG_FREERTOS_HZ=100`, it is `esp_rom_delay_us`, a busy wait), `components/interfaces/include/interfaces/clock.h`; `components/core/swd/wire.cpp` (the half-bit waits, `delayUs`), `components/core/swd/dp.cpp`; `components/core/nrf52/nrf52.cpp` (`waitReady`, `pollDhcsr`: `delayMs(1)` between polls); `components/core/flasher/flasher.cpp` (the page loop around `programPage`, the first page, the hash pass over every page); `components/core/nrf52/provision_door.cpp` (the base's write); `components/core/update/` (how the journal resumes *flashing-candidate* at boot); `docs/update.md`; ESP-IDF v5.5's Task Watchdog Timer documentation (what *the idle task of a core is watched* means, and what feeds it); `../phase-6/FOLLOWUPS.md`, *The SWD flasher starves the task watchdog*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 6 November the first `rak update --first-install` on a board, repeater v0.1.0 (417,284 bytes, SWD
at 125 kHz), never finished:

```
boot 19: watchdog reset
update: the journal says a run was in progress (first install); resuming at flashing-candidate
wake: 12 crash or watchdog wake(s) in a row: the next sleep is lengthened, not shortened ('wake')
> update: flashing-candidate -> stuck: no image to return to
rak update: STUCK: no image to return to - the radio has no proved application; a person's business now
```

A whole image is minutes of SWD traffic. Nothing on that path ever blocks:

- the wire's half-bit waits are `esp_rom_delay_us`;
- the NVMC and DHCSR polls wait `delayMs(1)`, which below one tick is a busy wait too.

The idle task of the core never runs, and the task watchdog resets the Walter after 30 s. The journal
then resumes the flash at every boot, exactly as designed, and the supervisor ends the run as *stuck*
after twelve. The radio was left with a half-written application. Thomas recovered it over its own
USB-C, and its key survived.

Every radio image is far longer than 30 s at 125 kHz. So is `rak provision`'s base (185 kB of flash):
it ran on the bench tool, never through this firmware. **No image can be flashed by v0.1.0.** This
task makes that possible, and makes a test say so.

## Deliverables

1. **A blocking pause, between transactions.**
   - At least one tick of real blocking (`vTaskDelay`) that lets the idle task of the core run:
     - between pages in the flasher's page loop;
     - between pages of the hash pass;
     - between pages of the base's write in `provision_door.cpp`;
     - in any other loop of this firmware that drives SWD for more than a second. Find them all:
       every loop around a memory-AP read or write, and every poll.
   - **Never inside an SWD transfer.** A pause stretches the line only while it is idle, between one
     transfer's last bit and the next one's first. The nRF52840's SWD clock has a specified minimum, so
     a transfer is never slowed (cite the product specification).
   - Through the `interfaces::Clock` the core already takes: `delayMs` of a named constant (at least
     one tick), or a new `yieldBriefly()` if that reads better. Choose the smaller change, say why in
     the report, and keep `delayMs`'s promise: *never shorter than asked*.
   - The cost is about 10–20 ms a page, so a few seconds for an image. The report gives it, with the
     arithmetic.
2. **A test that would have caught it.**
   - The host test's clock records the longest stretch of **simulated** time between two blocking
     pauses. The simulated wire, the NVMC and the polls advance that clock as the board would at
     125 kHz; read how they advance it today, and make it honest where it is not.
   - **The test:**
     - flash a full-size image through the simulated chip: 417,284 bytes, generated, with a valid
       descriptor;
     - run the hash pass;
     - run `rak provision`'s base write;
     - assert that no stretch is longer than **2 s** (fifteen times inside the watchdog).
   - It must fail against the code as it is before this task. The report shows that failure (the
     longest stretch it measured), then the pass.
3. **The watchdog's settings stay.** 30 s, panic, both idle tasks watched. A firmware that hangs
   must still be reset. Do not feed the watchdog from the flasher, which would hide a real hang; let
   the idle task run.
4. **The resume, read once more.** Read what the journal and the supervisor did on 6 November
   against `docs/update.md`. Say in the report whether each step was the designed behaviour:
   - the resume at every boot;
   - the twelve watchdog wakes;
   - *stuck* at the end;
   - the lengthened sleep.

   If one was not, it is a follow-up here, not a fix.
5. **`docs/bench.md`**, for Thomas, with the release build:
   - `rak update --first-install` of a full repeater image finishes with no watchdog reset, timed;
   - `boot` afterwards says `power-on` or `software`, never `watchdog`;
   - the same for `rak provision` on a spare radio, when there is one.

## Acceptance criteria

- [ ] The pause is in every SWD loop longer than a second, and never inside a transfer (each one cited).
- [ ] The 2 s test fails before the change (measured, in the report) and passes after; it covers an image, the hash pass and the base.
- [ ] The watchdog's configuration is unchanged.
- [ ] `docs/bench.md` has the lines; the `../phase-6/FOLLOWUPS.md` item is ticked with this task's PR.
- [ ] `tools/verify.ps1` green.

## Out of scope

The SWD clock (125 kHz stays the default; `swd` sets it). The supervisor's ladder. The board.
