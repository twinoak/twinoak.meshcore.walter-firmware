# 01 — A WAIT is waited out

**Depends on:** — (phase 18's 05 and 06 are on `main`).
**Spec:** `components/core/include/core/swd/dp.h` (the bounds, ~L98–102: `kWaitRetries` 10) and `components/core/swd/dp.cpp` (`Dp::transfer` ~L91–106); `components/core/include/core/swd/idle_pause.h`; `components/core/flasher/flasher.cpp` (`scan` ~L284, the steps' names ~L22 and ~L51); `components/core/update/radio_update.cpp` (`stepFlashing`, the second try of phase 18's 05); `tasks/phase-18/FOLLOWUPS.md` (*Sessions 4 and 5*, the items on sessions 6 to 9); `tasks/phase-18/reports/05.md` (its table of waits before the halt).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Since Walter 0.1.11 the scanner flash of every NoiseScope session has failed at its first
try, eight sessions of eight (2 to 9), the same way wherever the step was printed: *scan:
swd wait-timeout … nothing written*. The place and the moment differ: `0x036000` after 32
to 33 s (four times), `0x034000` after 30 s, `0x029000` after 9 s, `0x035000` after 31 s.
Phase 18's 05 catches it (a flash that wrote nothing is tried once more, and the second try
has carried six of seven), at the cost of half a minute, a second scan and, once, a session
refused.

Two things have never failed: **the flash back, over a running scanner**, which by its own
rule never writes flash; and **the radio's update of 23 November**, whose flash began
minutes into its wake. A session's flash begins one second after the wake has set the
radio's clock.

What is known, read in the code:

- The scan reads the application's pages through the debug port **while the application
  runs** (the core is halted after it).
- `Dp::transfer` answers a WAIT by asking again **ten times, back to back**
  (`kWaitRetries`), a few milliseconds in all, and then gives up with `kWaitTimeout`.
- The radio writes its own flash. In `twinoak.meshcore.repeater-firmware`, read and not
  changed here: the clock's file is written at the first clock set after a boot and hourly
  (`src/roles/repeater/clock/ClockKeeper.cpp` ~L174, ~L227: `!file_known_ || …`), and the
  Walter sets the clock at every wake; settings are saved after a console `set`; a session
  or an apply ends in a boot, so the wake after it is the first clock set after a boot.
- An nRF52840 stops answering memory accesses while its flash controller erases a page.
  **How long that is, is the Product Specification's to say** (the NVMC's electrical
  specification, the time of a page erase): read it and quote the figure and its section.
  My memory says 85 ms; do not build on my memory.

So the likeliest reading is that the scan meets a page erase of the radio's own and gives
up after a few milliseconds of an operation that takes tens. **It is a reading**: nothing on
the unit has shown the erase. This task makes a WAIT something the flasher waits out for as
long as the chip may take, and makes the unit say how long it waited, so that the next
flash either goes through at the first try or says why not.

## Deliverables

1. **A WAIT is retried for a time, not for a count of back-to-back tries.** Between two
   tries the line idles (`idle_pause.h`'s pause: the idle task runs, the watchdog is fed),
   and the whole is bounded by a named constant in milliseconds worked out from the Product
   Specification: the longest flash operation an application can start, with the margin you
   give it and say. Still a loop with a constant bound, still no ESP-IDF include in
   `core/swd` (the pause comes through the interface it already has, or one argument more:
   yours, said in the report).
2. **Everywhere the debug port is used**, not only in the scan: after the halt nothing but
   this firmware's own flash operations can make the chip wait, and those are polled
   already. Say in the report, from the code, which callers can now wait longer than
   before and by how much at most, and that no bound of a pass, an apply or a session
   (`tasks/phase-18/reports/05.md`'s arithmetic) is passed; move one only with the
   arithmetic shown.
3. **The unit says what it waited.** The debug port's counters gain the longest WAIT waited
   out, in milliseconds, and how many transfers waited at all. A flash's journal line says
   it when it is not nought (`scan: waited 86 ms at 0x036000, once`), a failed one says how
   long it waited before it gave up, and `rak update status` and the unit's report carry
   the counters. One line, whole (phase 18's 06 holds that journal lines are never cut).
4. **The second try stays.** 05's rule is the net for everything this does not explain
   (session 5's second try ended *no target: nothing answered on the line*, which is not a
   WAIT). Nothing of it changes.
5. **The simulated chip can make the port wait:** for a number of milliseconds of the
   test's clock, at a chosen address, while the core runs. Tests: a WAIT shorter than the
   bound is waited out, the page read, the flash goes on and the counter says how long; a
   WAIT longer than the bound ends as today (`wait-timeout`, nothing written, the second
   try) with the time waited in the line; a WAIT after the halt; no target and a FAULT
   answered as today, at once.
6. **The table of waits** of `tasks/phase-18/reports/05.md` brought up to date for this
   one, in `docs/` where that table lives or in this task's report if it lives nowhere.
7. **The words:** `docs/bench.md` and `docs/cycle.md` where they speak of the scan and of
   the second try; a line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The bound on a WAIT is a time, taken from the Product Specification with its section quoted, and the loop is constant-bounded (the constant's name; the test that holds the bound).
- [ ] A WAIT shorter than the bound costs a flash nothing but the wait; a longer one ends as today (the tests' names).
- [ ] The longest WAIT and the number of transfers that waited are in the counters, in a flash's journal line when not nought, in `rak update status` and in the report (one line of each printed in the task's report).
- [ ] No bound of a pass, an apply or a session is passed, by arithmetic in the report; the rails that hold them are green, unedited or moved with the arithmetic shown.
- [ ] 05's second try and 06's refused ending are as they were (their tests, unedited).
- [ ] `tools\verify.ps1` green (rails, host tests, the QEMU boot). CI green.

## What only a board can show

The unit at Bellevue, at its next session or radio update after the release: *a scanner is
there* about five minutes after *flashing-scanner* with no failed try between, and a journal
line that says how long the port waited. If the first try still fails, the line says how
long it waited, and that is the next thing known. Thomas's, from meshmaster.dk; I read it.

## Out of scope

The radio's firmware (another repository; it may write its flash when it likes). Halting
the core before the scan (it would stop the radio for the whole compare, and the scan is
there so that a flash that would change nothing stops nothing). The lost answer at every
wake. A release.
