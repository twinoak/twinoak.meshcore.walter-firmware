# 01 — The watchdog ends with the scanner

**Depends on:** —
**Spec:** `tasks/phase-17/FOLLOWUPS.md` (*The first session on hardware*); `components/core/flasher/flasher.cpp` (`finish` ~L505–530: the exit of every flash), `components/core/nrf52/nrf52.cpp` (`systemReset` ~L291–304: AIRCR.SYSRESETREQ), `components/core/swd/dp.cpp` (`powerDown` ~L213–222: what ends debug interface mode), `components/core/include/core/board/radio_pins.h` (`kResetPulseMs`, `kResetSpacingMs` ~L62–63; `resetPulse` ~L140; `noteRadioReset` ~L211–214) and `radio_pins.cpp` (~L150–176), `components/core/update/radio_update.cpp` (the kinds ~L75–76 and ~L493–500; booting and proving ~L690–745; `onRadioRestart` ~L183), `components/core/include/core/update/radio_update.h` (`kBootWindowMs`, `kProvingPeriodMs` ~L73–83), `components/core/update/scanner_session.cpp` (the way back ~L972–1034, taking a session up again ~L719), `components/core/supervisor/supervisor.cpp` (rung 2 ~L1067); `host_test/sim/nrf52_chip.h`; `spec/LINK-PROTOCOL.md` §10 (the spacing of two resets); the repeater firmware's `src/roles/scanner/Wdt.h` and `config.h` ~L116–136 (read, never edited)
**Size:** one session.
**Model:** claude-opus-5

## Goal

The first NoiseScope session on a unit ran on 21 November (Bellevue, meshmaster.dk job 39506).
It measured everything it was asked to and ended `stuck`:

```
22:30:38  flashing-golden: the golden image is going back
22:39:52  booting: the radio was reset: waiting for it to answer
22:39:54  proving: the golden image's health check
22:40:53  stuck: the radio restarted inside the proving period
```

An hour later the radio said `uptime 2570 s, booted: 3 (watchdog)` and was healthy. The scanner
starts the nRF52840's hardware watchdog at 60 s, and that watchdog cannot be stopped and
survives a soft reset (the repeater firmware's `Wdt.h`: *survives the soft reset into the
bootloader with the same timeout*; it is set to pause while the core is halted, so it stands
still during a flash). The flasher leaves every flash by `Nrf52::systemReset()`, which is
SYSRESETREQ: a soft reset. The repeater has no watchdog and feeds none, so 59 s after its first
boot it is reset, and a restart inside the proving period is a failure by rule. The watchdog's
own reset clears it, which is why the repeater was fine afterwards and why the unit's second
flash at its next wake proved.

After this task a radio that has been a scanner starts its next image with no watchdog running,
and a session that measured ends `restored`.

## Deliverables

1. **The reset that follows a flash clears the watchdog.** After an image has been written to
   the radio, the image is started by a pulse on the reset line (`RadioPins::resetPulse`), with
   debug interface mode ended first and the spacing of §10 held, so that the pulse can never be
   the second reset of two inside half a second. Read the nRF52840 Product Specification's
   *Reset behavior* table and its section on debug interface mode, cite both in the code where
   the choice is made, and say in the report what each kind of reset does to the watchdog and
   whether a pin reset does it while the debugger still asks for power.
2. **For every flash, or for the flashes that follow a scanner: decide and say why.** The
   switch back at a session's end, the way back when the scanner did not show itself, a session
   taken up again after a restart, and the supervisor's own flash of the golden image over a
   radio whose last word was a scanner's are the ones that need it. Every flash is the simpler
   rule and costs a second and one more boot. Whichever is built, one place decides it.
3. **Booting waits for the image the pulse started.** The boot window and the proving period
   begin after the pulse; the boot the soft reset began, where there still is one, is not the
   one that is proved, and its `EV_BOOT` is not read as a restart inside the proving period.
4. **The proof stays what it is.** A restart inside the proving period is still a failure. No
   rule is added that forgives a watchdog.
5. **The simulated chip learns the fault.** `host_test/sim/nrf52_chip.h` gets a watchdog an
   image can start, which counts only while the core runs, survives SYSRESETREQ and is cleared
   by the reset line, as the Product Specification says. A session against it with today's exit
   ends `stuck` at 60 s of the golden image's first boot (the test that shows 21 November), and
   with this task's ends `restored`.
6. **The bench**, as lines in `docs/bench.md` beside the session's walk: a session on a desk
   radio, the journal's four lines above with *restored* where *stuck* was, and `rak status`
   afterwards saying the reset reason the pulse leaves. Not run by this session.
7. **The words.** `docs/ARCHITECTURE.md` where it describes the flasher's exit and a session's
   way back; a line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The test of 21 November exists and fails without the change: a scanner that started the watchdog, the golden image flashed back, `stuck` by a restart 60 s into its first boot (the test's name, and the commit it fails on).
- [ ] With the change the same session ends `restored` and the journal has no restart in the proving period (test's name).
- [ ] No two resets of the radio are ever closer than the spacing rule allows, on every path through a flash's exit (the tests that hold it, or the rail).
- [ ] The report quotes the Product Specification for: the watchdog under a soft reset, under a pin reset, and a pin reset in debug interface mode.
- [ ] An apply of a radio image and a first install end as they did before (the existing tests pass; name any that were edited and why).
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The repeater firmware: a watchdog of its own, or feeding one it inherited, is that
repository's decision. The scanner's watchdog itself. Posting a session's later ending to the
server (`tasks/phase-17/FOLLOWUPS.md`). The board: no session opens a port.
