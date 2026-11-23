# Phase 18 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] A session's later ending is not posted.** Session 1 was `stuck` at 22:40Z and
  *restored* by the unit itself at its next timer wake; the server holds the first
  (`tasks/phase-17/FOLLOWUPS.md`). After 01 the cause is gone; a real `stuck` that the unit
  later mends would still read `stuck` on meshmaster.dk.
- [ ] **[later] A wake a person asked for did not take an open session up; the timer's did**
  (wakes 608 and 609 on 21 and 22 November). Seen once, not read in the code.
- [ ] **[later] The scanner is never given the time**: `rtc:0` in its `info` line and `ts:0` in
  every line of session 1.
- [ ] **[Thomas to decide] 32 kB of posted bodies a wake** (`kWakePostBytes`). Two reports are
  about twenty of them. With several claims a wake that is the bound a busy one meets first.
- [ ] **[later] Whole frames lit across every frequency in session 1**, most likely the unit's
  own LTE (`tasks/phase-17/FOLLOWUPS.md`). A session that wrote down when the modem sent would
  settle it.
- [ ] **[parked] The lost answer.** 02 and the library's task 70 make a lost claim harmless;
  the observations post still loses its answer at every wake. The bench's, when the next
  boards are here.
- [ ] **[03] Nine of sixteen lines of a `hist` never reached the server.**
  Session 1's fifth step: the scanner numbered its lines `seq` 245 to 260 and 261 for *done*;
  the session holds 245 to 251 and 261. The lines came 22 ms apart, about 170 bytes each, which
  115 200 baud carries; what did not keep up is on this side of the UART. The step's result
  says *done, 9 lines* and nothing says that any are missing. The scope steps send a line a
  second and are not known to have lost one (their `seq` was not checked). The scanner's own
  fault in that step is the repeater firmware's pile 3.15. Read in the code on 22 November and
  written as task 03: the port is not read while the step's progress call is out.
- [ ] **[later, meshmaster.dk's] A gap in `seq` is noticed by nobody.** The scanner numbers
  every line, so the server could say *lines 252 to 260 are missing* on the session's page.
  Offered, not written.
- [ ] **[later] A Walter tells no way back, so every reply to it is still flooded.** The library
  can teach a repeater the way home (its task p1.21-71) and the gain is real - a reply that
  takes the hops the request took, instead of being flooded at every repeater that can hear it.
  §6 of the link protocol has no request that sends a PATH packet, so `LinkMesh` takes the
  defaulted member and tells none, and §6.3's reply record cannot say how a reply arrived
  (`via_path` is a path return, not a flood), so the job's log reads *route unknown*. Taking it
  would be a request in §6 and a field in §6.3, which is the design repository's and then a task
  here. Eleven of the library's own cases are not run through the link shim for want of it
  (`host_test/CMakeLists.txt`, `LINK_SHIM_SKIPPED`), which is the cost measured.
- [ ] **[later] A claim the wake's own room refuses is asked twice.** Since the bump a claim
  whose answer *was lost* is made once more (the library's `mm::answerWasLost`, which reads
  `reachable` false with `httpStatus` 0 as lost), and a call this firmware never began - no room
  left in the bound, the 120-a-minute ceiling, no token - leaves exactly that record. So such a
  claim is asked a second time, refused at the same gate, and `budget`/`no_room` count two where
  one call was wanted. Nothing goes on the wire and the wake ends the same way, so it is a
  counter that reads high and not a fault; the fix is either a `Call` the library cannot read as
  lost, or a word from upstream for *never sent*. Read in the code of the bump (the sync pile's
  task 02) and not provoked in a test: every claim of the wake bench's cases begins.

## A reset pulse that restarted nothing (22 November)

Read in the unit's report of 22 November 01:35Z and in meshmaster.dk's status results of its
repeater. Found while reviewing 01.

- [ ] **[04] Task 01 starts every flashed image by a line nothing shows to work.** The
  supervisor at Bellevue: *the radio went quiet, cured by rung 2, a reset pulse; lasted 71 s;
  rung 2, a reset pulse: done, the radio came back*; *today: resets 2/6*; *the radio restarted
  0 times in the last 600 s (1 in all)*. The radio: `uptimeS` in every hourly status from
  20 November 18Z to 21 November 18:11Z points at one boot, 15 November 21:49Z; at 23:23:41Z
  *uptime 2570 s, booted: 3 (watchdog)*, the watchdog's reset of 22:40:51Z to the second; the
  next boot is the re-flash's, 00:30Z, *4 (software)*. So the pulse the supervisor counted
  restarted nothing and the radio came back by itself. When that evening it went out is not in
  the report: not proof. With 01 alone such a unit's radio stays halted after every flash.
- [ ] **[Thomas] What the line is on that unit.** Two candidates, neither checked:
  `UICR.PSELRESET` not set on the module, so that P0.18 is an ordinary pin; or the wire on
  the adapter. Task 04's first flash says the first; a meter on nRESET during a pulse says the
  second.
- [ ] **[Thomas to decide] Writing `UICR.PSELRESET` where it is not set.** This firmware writes
  nothing in UICR, by rule. A module without it has no pin reset whatever the wire does.
- [ ] **[later] The scanner should say its watchdog's seconds** in its `info` line. Task 04
  holds the number as a constant copied from the repeater firmware's `WDT_TIMEOUT_S`.

## The review of 01 and 03 (22 November)

01 merged 22 November (#90), 03 (#91). Read: both reports' deviations, decisions and what was
not verified; `Flasher::finish()` as 01 left it. Not read line by line: the rest. Six of six
and seven of seven criteria.

- [ ] **[04] 01 does what it was asked and the asking was short of a fact.** Every flash ends
  in a pulse and nothing else; its report names the untested line as its largest assumption.
  The section above is that assumption met.
- [ ] **[none] 01 ends debug interface mode before the pulse** and keeps the spacing rule;
  `Nrf52::systemReset()` is left without a caller, which 04 gives back.
- [ ] **[later] On a build code Fxx module whose firmware does not open its port, 01's pulse
  closes the access port** where the soft reset left it open (its report, deviation 1.3). Said
  at the next flash as *protected*.
- [ ] **[none] 03 counts the moments the port lost bytes, not the bytes:** the driver cannot
  say how many. The ring is `kRxRingBytes`, from the loop's idle plus `kReaderWorkMs`, 50 ms,
  which its report calls a guess. `kLeaseAfterStepMs` is three minutes. Rail R25 holds *no call
  in a step*.
- [ ] **[later] 03's journal entry of a step that lost bytes carries the count and not the
  command** (a slot is 48 bytes); the command is in the entry that opened the step.

## The sync's reading of the server (22 November)

- [ ] **[02] The sync's report says meshmaster.dk hands back *only a lease whose answer it never
  managed to deliver*, and it does not.** `tasks/sync/reports/02.md`, *`holding` in every
  claim*. As built (`src/MeshMaster.Core/Transport/HandedBack.cs`, `Lost`), a claim that
  carries `holding` is handed every job leased to this exit node whose lease is running, that
  has no result and that it did not name. The server keeps no record of an answer that did not
  arrive. With one claim a wake the report's conclusion mostly stands, because the leases of
  the work this firmware keeps outside the library's buffer have run out by the next wake.
  Task 02 makes a wake claim more than once, so it is given the answer: deliverable 11.
- [ ] **[none] The sync itself** (#92, main 2749afd, 104 minutes): the copy at v0.7.7, the two
  signatures, `jobsWaiting` read from a result's reply, the third fixture directory, the
  tool's numbering mended. Five criteria ticked and three bench items open. Read by its report
  only.

## The review of 02 and 04 (23 November)

02 merged 23 November (#93), 04 (#94, main 7e0fe7b). Read: both reports' deviations, decisions
and what was not verified; `Flasher::finish()` and `Flasher::startAgain()` whole. Not read line
by line: the update machine's half of 04, the supervisor, 02's code, the tests. Ten of ten
criteria in each; 02 leaves five bench items. Nothing that holds a release back.

- [ ] **[none] 04 is the task.** A module whose UICR says *no pin reset* is started through
  the debug port and never pulsed; elsewhere the pulse, and after an empty window one look:
  only a core that is still halted is started through the port, anything else is left alone.
- [ ] **[Thomas] One thing in 04 is still a simulation**, and its report says so: that a core
  the pulse did not reach is still halted when the port is opened again twenty seconds later.
  If a unit's wire is dead and its chip does not stay halted, a flash there ends as it would
  with 01 alone: `stuck`, and the supervisor's power cycle. The first flash on the unit says
  which unit Bellevue is; its journal carries UICR's two words, what started the image and what
  the look found.
- [ ] **[later] If that look ever finds a core that is not halted on a unit whose line is
  dead**, the exit to build is the soft reset through the port always, then the pulse.
- [ ] **[none] 04 raised `kRaisedBoundMax` from 60 to 65 minutes** and the session's bound to
  60 min 35 s: the second window and the watchdog's 75 s are counted, not borrowed. A session
  on a unit whose line does not work has three dark stretches where a working one has two.
- [ ] **[none] The line's record grew the pins' retained record by five bytes**, so the first
  boot of the release reads *untried* and a full second of spacing.
- [ ] **[none] 02 names what this firmware holds** (`FirmwareJobs::holdsOutsideTheBuffer`, into
  `ModemUplink::claimJobs`), with tests for a session and an apply whose owed result went
  unanswered: nothing is handed back and nothing begins twice. *No room* is decided where the
  library's bit is written, so a claim the wake cannot hold is not asked and not counted as a
  failure.
- [ ] **[later] Two of the library's seven words for why a wake stopped claiming are held by
  no test here** (*no time* is one), its report's deviation 5.

## The second session, and the first flashes by the new exit (23 November)

Read on meshmaster.dk: jobs 41846 (the radio's apply) and 41878 (session 2), and the unit's
report of 11:47Z (asked for with Thomas's yes). The unit runs 0.1.11+a2e703e and radio
0.1.7+eaa19a7.

- [ ] **[none] The reset line works on this unit.** The journal: *booting the golden image -
  started by a pulse on the reset line; nRESET is on P0.18*. The radio: *uptime 188 s, booted:
  2 (reset pin)*. The apply of 0.1.7 answered two seconds after its reset. So the section
  above (*A reset pulse that restarted nothing*) was not this: on 21 November the chip was
  most likely still in debug interface mode, left there by the exit of that day's flash. Task
  04's way out was not needed here and cost nothing.
- [ ] **[05] Session 2 ran no step: its scanner flash failed twelve seconds
  in.** Journal: *flashing-scanner* 11:37:41Z, *flashing-candidate*, **`rolling-back: the
  flasher did not finish`**, *room* 11:37:55Z, *flashing-golden* 11:37:57Z, *booting* 11:44:23Z
  (the pulse), *proving* 11:44:24Z, **`restored`** 11:47:25Z. Nine minutes off the mesh, 0
  lines, every step `notRun`. `rak update`: *the candidate failed its health check (the
  flasher did not finish) and is on the bad list*: the scanner image of 0.1.6.
- [ ] **[05] Which step of the flasher failed is stored and printed nowhere.**
  `fail(Reason::kFlashFailed, …, r.step)` puts it in the journal entry's `detail`
  (`radio_update.cpp` ~L642); the journal's print shows a detail for two other reasons only
  (`console.cpp` ~L2905-2914).
- [ ] **[05] The failed flash changed nothing on the chip, by the times.** Flashes on this
  unit, in seconds: the scanner image of session 1, 127 kB, 288; the candidate of 23 November,
  424 kB with nearly every page changed, 903; so a kilobyte scanned, programmed and read back
  is about 2.2 s. Session 1's restore (424 kB compared and read back, 127 kB programmed) was
  554 s, which makes the compare and the read-back 0.93 s a kilobyte together and the
  programming 1.27. Session 2's restore was 386 s, and 424 kB compared and read back with
  **nothing** programmed is 392. So the scanner flash of 11:37:43Z failed before it halted the
  core: while connecting, in the checks before the scan, or in the scan's first seven pages
  (twelve seconds is about 26 kB of reading). It left no pulse (`finish(…, false)`), the
  repeater ran on, and the golden image's flash two seconds later met the same chip and went
  through.
- [ ] **[05, then the unit] What is not known is whether it will happen every time.** Either it is how a
  chip that a pin reset started answers the first debug session after it (since 01 every flash
  is that), or it was a moment: the application writing its own flash while the scan read, a
  bus that stalled. One failure in one try cannot say. What is wrong either way: the machine
  answered a flash that had written nothing by flashing the whole golden image back (nine
  minutes off the mesh), and put an image on the bad list that was never on the chip.
- [ ] **[none] A wake took what was waiting.** The session, the two reports asked for during
  it, and the hour's status and telemetry all went in the one wake of 11:36Z. An offer and its
  apply went in one wake at 11:03Z (the apply's flash began a minute after the offer was
  stored).
