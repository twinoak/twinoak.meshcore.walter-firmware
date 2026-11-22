# 04 — An image is started either way

**Depends on:** 01 (merged).
**Spec:** `tasks/phase-18/reports/01.md` (*Deviations* 1 to 3; *Not verified here*); `tasks/phase-18/FOLLOWUPS.md` (*A reset pulse that restarted nothing*); `components/core/flasher/flasher.cpp` (`finish` ~L530–551: the port closed, the lines let go, the pulse) and `include/core/flasher/flasher.h` (`Result` ~L91–100); `components/core/nrf52/nrf52.cpp` (`halt` ~L220, `systemReset` ~L293, which has had no caller since 01, `coreState` ~L319), `include/core/nrf52/nrf52.h` (~L21–34: what this layer never does) and `regs.h` (UICR, read-only to this firmware ~L44–60); `components/core/include/core/board/radio_pins.h` (`kResetPulseMs`, `kResetSpacingMs` ~L62–63; `resetPulse` ~L140); `components/core/include/core/update/radio_update.h` (`kBootWindowMs` ~L73, `kProvingPeriodMs` ~L83, `kScannerWindowMs` ~L92, the kinds ~L188–189, `kNoBoot` ~L216) and `radio_update.cpp` (the boot's wait ~L592–613, the proof ~L731–744); `components/core/supervisor/supervisor.cpp` (`onRadioRestart` ~L229, the reset rung ~L1042–1060, the cure ~L1152) and `supervisor.h` (the rungs ~L30 and ~L192, `kCured` ~L233); `host_test/sim/nrf52_chip.h` (the watchdog ~L183–215); `docs/update.md`, `docs/supervisor.md`, `docs/flashing.md`, `docs/bench.md` §2 and §11; the nRF52840 Product Specification (*Reset*, *UICR*, *WDT*, *Debug and trace*); the repeater firmware's `src/roles/scanner/config.h` (`WDT_TIMEOUT_S`)
**Size:** one session.
**Model:** claude-opus-5

## Goal

Since task 01 the image a flash wrote is started by a pulse on the reset line and by nothing
else, on every flash. Task 01's report calls the line its largest untested assumption. Nothing
on record shows it restarting a radio on a unit, and one record says it did not.

The unit at Bellevue on 21 November, from its own report of 22 November 01:35Z and from
meshmaster.dk's status results of its repeater:

- **the supervisor:** *the radio went quiet, cured by rung 2, a reset pulse; lasted 71 s*;
  *today: resets 2/6*; *the radio restarted … (1 in all)*;
- **the radio:** one boot from 15 November 21:49Z until that evening's session, by the
  `uptimeS` of every hourly status; at 23:23:41Z *uptime 2570 s, booted: 3 (watchdog)*, which
  is the watchdog's reset of 22:40:51Z to the second; then the re-flash's boot at 00:30Z,
  *4 (software)*.

No restart of the radio answers a reset pulse. When that evening the pulse went out is not in
the report, so this is not proof. But on a unit whose line does not reach the chip, the
firmware as it stands leaves the radio halted after every flash: an apply rolls back and ends
`stuck`, a session's scanner never starts, and the repeater returns only by the supervisor's
power cycle. Before 01 an apply worked on that unit.

After this task an image is started on a unit whose reset line works and on one whose line
does not, the unit says which it is, and a session ends `restored` on both.

## Deliverables

1. **What the chip says about its reset pin.** While the debug port is open for a flash,
   `UICR.PSELRESET[0]` and `[1]` are read: whether the pin reset is enabled, and on which pin.
   Read and never written; `regs.h` names them as it names the rest of UICR. The answer is in
   the flash's result, its journal entry, what `rak swd id` prints and the report's `radio`
   section. Quote the Product Specification for what the two must hold.
2. **A pulse that cannot work is not tried.** Where UICR says the pin reset is not enabled,
   the image is started through the debug port: `Nrf52::systemReset()`, the exit every flash
   had before 01, as it was. Where it is enabled: the pulse, as 01 built it.
3. **A pulse that did not work is found out by asking the chip.** When nothing of the radio's
   has come inside the window after a pulse (`kBootWindowMs`, `kScannerWindowMs`), the debug
   port is opened once more and the core's state read:
   - still halted where the flash left it: the line did not reach the chip. The image is
     started through the debug port and the window begins again;
   - running: the pulse worked and the image does not speak. Nothing is sent; the ending is
     the one there is today (`kNoBoot`, or *not a scanner*);
   - the port closed or silent: the same ending as today.

   One such look a flash, never two. The journal says what the look found.
4. **A scanner's watchdog, when the debug port started the image after it.** A soft reset
   leaves the watchdog a scanner started counting (01's report quotes the Product
   Specification), and the repeater feeds none, so it is reset once inside the watchdog's
   time. After a flash **over a scanner** that the debug port started, the proving period
   therefore begins at the boot that follows the watchdog's reset, or when the watchdog's
   longest time has passed without one. The journal says which. The proof stays what it is: a
   restart inside the proving period fails it. The time is one named constant that says where
   it comes from (sixty seconds in the repeater firmware's scanner today) with a margin. A
   flash that was not over a scanner waits for nothing.
5. **The unit remembers, and says.** Whether a pulse on the reset line has been seen to
   restart the radio, seen not to, or never tried: kept over a sleep, printed by `rak watch`
   and in the report's `radio` section, changed by the next observation. It is for a person
   to read; what a flash does is decided by deliverables 2 and 3 each time.
6. **The supervisor's second rung says what happened.** *Cured by a reset pulse* is said only
   when the radio restarted at the pulse: a new boot, by its `EV_BOOT` or its boot id. A radio
   that answers again without one *came back by itself*, and the pulse is recorded as having
   restarted nothing, which is an observation for deliverable 5. The ladder's order and its
   caps do not change.
7. **The simulated chip** (`host_test/sim/nrf52_chip.h`) has `PSELRESET` in its UICR and a
   reset pin that is one of three things: enabled and wired, enabled and not reaching the
   chip, not enabled.
8. **Tests:**
   - **Bellevue as read:** enabled in UICR, the line dead. An apply: the pulse, an empty
     window, the look finds the core halted, the debug port starts the image, the proof,
     `committed`; the journal's lines in order. This test fails on the firmware as it is, with
     `stuck`; show that in the report;
   - the same unit, a session: the scanner started through the debug port, the steps, the
     golden image back, started through the debug port, the watchdog's reset, the proof begun
     after it, **`restored`**, one outage. Give the session's length beside 01's;
   - not enabled in UICR: no pulse on any flash, the same endings;
   - a line that works: 01's sequence to the frame, no look, no wait; 01's tests pass
     unedited, and any that were edited are named with the reason;
   - the pulse works and the image is silent: one look, the core running, nothing sent,
     `kNoBoot` and the rollback as today;
   - the look meets a closed port (01's Fxx case): the ending as today;
   - the power pulled between the pulse and the look, between the look and the reset, and in
     the wait for the watchdog: the next wake ends it by the rails there are;
   - the supervisor: a radio that restarts at the pulse, and one that comes back without
     restarting;
   - no two pulses closer than the spacing rule on any path; say whether a start through the
     debug port counts for the rule, and why.
9. **The bounds.** The second window and the wait for the watchdog are inside the time a pass
   and a session are given (`kOnePassMs`, the session's ceiling, the wake's raised bound).
   Give the numbers; raise nothing without saying so.
10. **The rails.** UICR is read and never written: the forbidden-register rail's sweep shows
    the two reads and no write. Every rail of `docs/update.md` and `docs/supervisor.md` named
    in the report with the test that holds it.
11. **The words.** `docs/flashing.md`, `docs/update.md`, `docs/supervisor.md`,
    `docs/console.md`; `docs/bench.md` §2 (what `rak swd id` and `rak watch` say about the
    line) and §11. A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The test of Bellevue as read exists, fails without the change with `stuck`, and ends `committed` with it (the test's name, both journals).
- [ ] A session on that unit ends `restored` with one outage, the proof begun after the watchdog's reset (the test's name).
- [ ] On a unit whose line works nothing is added: no look, no wait, 01's tests unedited (or each edit named).
- [ ] No pulse where UICR says the pin reset is not enabled; the report quotes the Product Specification for the two registers.
- [ ] One look a flash at most, and nothing is sent to a core that is running (the tests' names).
- [ ] UICR is never written: the rail's sweep, named.
- [ ] `rak watch`, `rak swd id` and the report say what UICR holds and what a pulse has been seen to do (the tests' names, the lines quoted).
- [ ] *Cured by a reset pulse* only with a restart; otherwise *came back by itself* (the tests' names).
- [ ] The second window and the watchdog's wait fit every bound that holds them; the numbers are in the report.
- [ ] `tools\verify.ps1` green. CI green.

## What only a board can show

The first flash on the unit after the release: its journal says what UICR holds, which of the
two started the image, and what the look found. Thomas's, from meshmaster.dk; I read it.

## Out of scope

Writing UICR: enabling the pin reset on a module is an act of provisioning and Thomas's
decision. The adapter and the wire. The power switch as a way to start an image. A watchdog
the repeater feeds or starts: that repository's. A change to the ladder's order or caps.
meshmaster.dk: the new words ride in the journal and the report as text. The board: no session
opens a port. A release: Thomas presses the button.
