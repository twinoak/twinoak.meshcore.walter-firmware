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
