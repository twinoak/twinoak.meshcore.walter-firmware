# 05 — A flash that wrote nothing

**Depends on:** 04 (merged).
**Spec:** `tasks/phase-18/FOLLOWUPS.md` (*The second session, and the first flashes by the new exit*); `spec/SYSTEM.md` S-5 (~L95–99) and the update's drawing (~L218–224: *the candidate's hash is remembered and never tried again* stands under the health check's *no*); `components/core/flasher/flasher.cpp` (`openChip` ~L155–204, `begin` ~L206, `scan` ~L249, `programPages` ~L365, `settingsInvalidate` ~L505, `finish` ~L575–620, `flash` ~L663–704: the halt comes after the whole scan) and `components/core/include/core/flasher/flasher.h` (`Step` ~L47–68, `Outcome` ~L71–75: *refused* is nothing on the chip changed, `Result` ~L103–122); `components/core/nrf52/nrf52.cpp` (`open` ~L97–135, `close` ~L137, `halt` ~L220) and `components/core/include/core/nrf52/nrf52.h` (the timeouts ~L61–66); `components/core/include/core/swd/dp.h` (the bounds ~L4, ~L100–112); `components/core/update/radio_update.cpp` (`fail` ~L302–320, which puts the hash on the bad list before anything else; `nodeFault` ~L329–336, the ending that puts nothing there; the bad list in the preconditions ~L556–562; `stepFlashing` ~L611–645, `kFlashFailed` with the flasher's step as its detail ~L642) and `radio_update.h` (the states ~L32 and ~L211, the reasons ~L235–248, `detail` ~L323); `components/core/update/scanner_session.cpp` (the scanner image's checks ~L637–648, the endings ~L182–192, the scanner run ~L804); `components/core/console/console.cpp` (the journal's line ~L2896–2918, a typed flash's own words ~L1955–1963); `components/agent/firmware_jobs.cpp` (the outcomes' words ~L128–155, the notes it gives the server's lines); `components/core/include/core/image/store.h` (the bad list ~L241, ~L437); `host_test/core/flasher/`, `host_test/core/update/`, `host_test/sim/nrf52_chip.h`; `docs/update.md` (*The machine*, *What starts a flashed image*, *The journal*, *A session*), `docs/flashing.md`, `docs/console.md`, `docs/bench.md`; the nRF52840 Product Specification (*Debug and trace*, *NVMC*, *Reset*)
**Size:** one session.
**Model:** claude-opus-5

## Goal

The second NoiseScope session on the unit at Bellevue (23 November, meshmaster.dk job 41878)
flashed no scanner. Its journal: *flashing-scanner* at 11:37:41Z, **`rolling-back: the flasher
did not finish`** twelve seconds later, the golden image flashed back in 6 min 26 s, *restored*.
Nine minutes off the mesh, no line measured, and `rak update` says of the scanner image that
it *failed its health check (the flasher did not finish) and is on the bad list*.

By the times of every flash this unit has made, that flash **changed nothing on the chip**:
the restore is exactly a compare and a read-back of the whole image with no page programmed
(the follow-ups have the arithmetic). It ended before it halted the core: at the connect, in
the checks before the scan, or in the scan's first pages. The golden image's flash began two
seconds later on the same chip, with the repeater still running, and went through.

Three things are wrong whatever made that flash fail:

1. **Nobody can say what failed.** The flasher's step is in the journal entry's `detail` and
   is printed nowhere; the layer's status under it is not kept at all.
2. **A flash that wrote nothing was answered by flashing the whole golden image back.**
3. **An image that never reached the chip is on the bad list**, in words that say it ran.

And one thing is not known: whether this is every flash since task 01 (each now begins on a
chip that a pin reset started and whose application is running; before 01 the chip stayed in
debug interface mode from one flash to the next), or was a moment.

After this task a failed flash says its step and its status; a flash that ended before its
first write is made once more; if that fails too the job ends with the radio as it was; and
the bad list holds images that are wrong or that ran and failed, and no others.

## Deliverables

1. **The flasher says whether it changed the chip.** `Result` carries it, worked out from
   what the flasher did and not from a step's name: nothing written (every way out before the
   first write of the settings page or of a page), and whether the core was halted and so had
   to be started again.
2. **A failed flash says what failed.** The step, the status of the layer under it, the
   address when there is one, and whether the chip was changed: kept in the journal's entry
   (the `detail`'s thirty-two bits; say the layout, and that an older entry reads as it did)
   and printed in the journal's line at the console and in a report, in `rak update status`,
   and in the note of the line that goes to the server with the job, so that the job's page
   says it without a report.
3. **Once more.** A flash that ended having written nothing is made a second time, after a
   pause that is one named constant, and after the radio's `HELLO` when the first try had
   halted it and started it again. The journal says so before the second try (*the flash did
   not begin (the step, the status): once more*). One second try and never a third, for every
   flash this firmware makes: a candidate's, a scanner's, the golden image going back, a first
   install, the supervisor's.
4. **Twice, and the radio is as it was.** For a candidate and for a scanner image: no flash
   back, nothing on the bad list, and the radio serving throughout. An apply ends `refused`
   and a session ends `refused` (*nothing was touched*, the ending there is), each with the
   step and the status in its journal and its note. No new word in the contract. For the
   golden image's own flash the ending is what it is today, with the step said.
5. **The bad list is for an image that is wrong or that ran and failed.** The specification's
   *never tried again* stands under the health check. So: an image whose flash failed after
   its first write is still followed by the golden image, and goes on the bad list only when
   the fault is its own (its bytes, its digest, its descriptor) or its proof's. A debug port
   that gave up, a page that would not take, a core that would not halt are the unit's, and
   their image may be flashed again. `rak update`'s sentence says which it was: *the flash
   failed* is not *failed its health check*. If S-5 as written forbids any of this, stop and
   say which sentence.
6. **The path before the halt, read against the Product Specification** for a chip that a pin
   reset started and whose application is running or asleep when the port is opened: the
   connect, the power-up and its hundred polls, the control port, the protection, the memory
   port, the identity, the wait for the flash controller, the SoftDevice's word, and the
   scan's reads while the core runs (an application that erases its own flash while the scan
   reads stalls the bus). For each wait and each retry: what bounds it, and whether that bound
   has only ever met a chip already in debug interface mode. Say in the report which of them
   you would suspect, and why. **Change a bound only with the specification quoted; mend
   nothing by guess.**
7. **The simulated chip** can fail a flash at each step before the halt, once or every time,
   one of them a read that stalls while the application runs; and the benches count the
   radio's outages.
8. **Tests:**
   - **23 November:** a scanner flash that fails once before its halt. With the change: the
     line in the journal, the second try, the scanner there, the steps run, `restored`. It
     fails on the firmware as it is, with no step run and a golden flash; show both journals;
   - the same failing twice: the session ends `refused`, no flash of any kind, **no outage**,
     the scanner image not on the bad list, and the next session is taken;
   - an apply the same two ways: `committed` after one failure; `refused` after two, the
     candidate still offerable, no outage;
   - a flash that fails after its first write, by the unit's fault: the golden image goes
     back as today, the image is **not** on the bad list, and `rak update` says *the flash
     failed*; by the image's own fault (a digest that is not the one given): on the bad list
     as today;
   - the golden image going back, failing once before its halt: tried once more, `restored`;
   - a first try that halted the core and wrote nothing: the radio is started again, answers,
     and the second try follows;
   - the power pulled between the two tries: the next wake ends it by the rails there are;
   - the journal's line, `rak update status` and the server's note for a failed flash, quoted.
9. **The bounds.** The second try and its pause are inside the time a pass, a session and a
   wake are given; give the numbers and raise nothing without saying so.
10. **The words.** `docs/update.md` (the machine, the journal, a session), `docs/flashing.md`,
    `docs/console.md`; `docs/bench.md`, for a bench with a radio: a flash made to fail before
    its halt (the debug lines pulled for the first seconds), and what the journal then says.
    A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The test of 23 November exists, fails without the change with a golden flash and no step run, and with it ends `restored` with its steps run after one second try (the test's name, both journals).
- [ ] Two failures before the first write end `refused` with no flash, no outage and nothing on the bad list, for a session and for an apply (the tests' names).
- [ ] A failed flash's step, status and *nothing was written* / *the chip was changed* are in the journal's line, `rak update status` and the server's note (quoted in the report); an entry from before this task prints as it did.
- [ ] The bad list takes an image only for its own fault or its proof's (the tests' names), and the report quotes S-5 and the drawing for it.
- [ ] One second try at most, on every kind of flash (the tests' names).
- [ ] The report has deliverable 6's table: every wait and retry before the halt, its bound, and whether it has met a chip outside debug interface mode; any bound changed has its sentence of the Product Specification.
- [ ] A flash that succeeds at its first try is what it was: 01's and 04's tests pass unedited, or each edit is named.
- [ ] `tools\verify.ps1` green. CI green.

## What only a board can show

The next session on the unit. Either it runs at its first try, and 23 November was a moment;
or its journal says *the flash did not begin (the step, the status): once more* and goes on,
and that step is the answer to what happens on a chip a pin reset started. Thomas's, from
meshmaster.dk; I read it.

## Out of scope

A cure for the cause beyond deliverable 6: resetting and halting the chip before its
application runs is a candidate once the step is known, and a task of its own. Taking the
0.1.6 scanner image off the unit's bad list: a newer scanner image has another hash.
meshmaster.dk: the new words are text in a journal and a note. The repeater firmware. The
board: no session opens a port. A release: Thomas presses the button.
