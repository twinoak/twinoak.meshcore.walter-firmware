# 03 — A step is listened to

**Depends on:** — (nothing of 01 or 02; it may run before either)
**Spec:** `tasks/phase-18/FOLLOWUPS.md` (*Nine of sixteen lines of a `hist` never reached the server*); `components/agent/firmware_jobs.cpp` (the session's loop ~L2228–2269: the lease ~L2235–2239, `postApplyProgress` right after a step began ~L2256, the idle ~L2268; `extendLease` ~L307, `postApplyProgress` ~L727, `postSomeLines` ~L1922, `onStepLines` ~L1969; the result's shape ~L1826–1849) and `include/walter_agent/firmware_jobs.h` (`kExtendEveryMs` ~L112, `kSessionIdleMs` ~L226); `components/core/update/scanner_session.cpp` (a step begun ~L850–876: the journal's entry, then `radio_.say`; `stepRunning` ~L878–902) and `include/core/update/scanner_session.h` (`kListenIdleMs` ~L120, `StepReport` ~L233); `components/platform/uart.cpp` (`kRxBuffer` ~L23, `uart_driver_install` ~L50, `read` ~L92–102); `components/core/include/core/link/client/client.h` (the counters ~L269–270) and `link/client/client.cpp` (`poll` ~L160, the read ~L319, ~L555–569); `components/core/console/console.cpp` (the counters printed ~L1575–1582, the typed session's loop ~L2615); `host_test/sim/scripted_scanner.h`, `scripted_radio.h`, `scripted_server.h`, `host_test/core/update/session_bench.h`; `docs/update.md` (*A step, and what ends it*; *A session as a job*); the repeater firmware's `docs/noisescope.md` §5 and its pile 3.15
**Size:** one session.
**Model:** claude-opus-5

## Goal

The fifth step of the first session on a unit (21 November, Bellevue, meshmaster.dk job 39506)
was `hist 869.418 869.818 25`. The scanner numbered its lines: `seq` 245 to 260 for sixteen
`hist` lines of about 170 bytes, 22 ms apart, and 261 for *done*. The server holds 245 to 251
and 261. The step's result says *done, 9 lines*. Nothing anywhere says that nine are missing.

What the code does, as read on 22 November:

- A step begins in `ScannerSession` with its journal entry and its command said to the scanner,
  and returns *changed*. The job's loop then writes its own line and calls
  `postApplyProgress`: a call to the server over LTE, a second or more. Until that call is
  back nobody reads the port, and the scanner has been speaking since its `ack`.
- The driver's receive ring is 1 024 bytes (`kRxBuffer`). Sixteen lines are 2.7 kB; seven are
  about 1.2 kB, which is the ring and what the driver and the hardware hold behind it.
- The lease's timer (`kExtendEveryMs`) is read at the top of the same loop, so its call falls
  inside a step too, once a step or a program is long enough.
- While a step runs and nothing else happens the loop reads the port every 100 ms. At 115 200
  baud 100 ms is 1 152 bytes: more than the ring. A scanner that fills the port loses bytes to
  a loop that does nothing wrong.

The three scope steps of that session sent a frame of about 450 bytes a second and are not
known to have lost one. A slower server would have cost them one.

After this task nothing is asked of the server between a step's command and its end, the ring
holds what the port can carry while the reader is away, and bytes that are lost all the same
are counted and said.

## Deliverables

1. **The step's line goes out before its command.** The server learns *step N* and its command
   before the scanner does. From the command being said until the step has ended (`done`,
   `clock`, `full`, `refused`, `silent`) the job makes no call to the server: no progress, no
   lease, no lines. The step's lines are posted after it, as today. The journal's rule stands:
   the entry before the act. Say how the machine and the job share the moment.
2. **The lease is seen to between steps.** Before a step's command the lease must cover the
   step's seconds, the posting of its lines and the calls that follow; it is extended then if
   it does not. The timer no longer fires inside a step. During the two flashes the lease is
   what it is today.
3. **The ring by arithmetic.** `kRxBuffer` follows from the baud rate and the longest time the
   reader is away while the port is listened to (the loop's idle and the loop's own work
   between two reads), with a margin, held by a `static_assert` beside the numbers. Say the
   size, what it takes from the internal heap and why that is affordable on this board. The
   link's frames are read through the same ring: they get the room too.
4. **A loss is counted and said.** The driver knows when its ring was full and when the
   hardware's FIFO ran over; today it is installed without the queue that would say so. The
   count reaches:
   - the port's own seam, and through it the link client's counters beside `text_truncated`
     and `text_dropped`, and the console's line that prints them;
   - a session's step: a step during which bytes were lost says so, with the count, in the
     journal entry of its end, and the session's journal lines carry it to the server with the
     result.

   The result's fields (`steps[].ended`, `lines`, `bytes`, `lines`, `linesUnanswered`) are
   meshmaster.dk's contract and do not change here.
5. **The simulated port has the ring.** The port behind `sim::ScriptedRadio` holds as many
   bytes as the driver's does and loses and counts the rest. A call to the scripted server
   takes time on the fake clock, and the scripted scanner's lines arrive by that clock and not
   all at once. Say what each existing bench needed to stay as it was.
6. **Tests:**
   - **21 November:** an `ack`, sixteen lines of 170 bytes 22 ms apart and a `done`, with a
     progress call that takes two seconds. It fails on the code as it is, with about seven
     lines kept; show that in the report. With the change all sixteen and `done` are the
     step's lines, `seq` unbroken;
   - a step of 300 s with the lease's five minutes falling inside it: no call inside the step,
     and the lease never short;
   - a scanner that fills the port for ten seconds, line upon line at the baud rate: no byte
     lost with the loop as it is;
   - a ring made too small on purpose: the loss is counted, the step's journal entry says it,
     the posted result carries that journal line and its fields are unchanged;
   - a scope step, a line of 450 bytes a second for 60 s: as before;
   - the session tests that exist pass; name each that was edited and why (the order of the
     progress call and the command moves, so a test that counts calls will).
7. **The typed session** (`console.cpp`) runs the same machine and calls no server. Say what
   changes for it; it prints the count where it prints the step's ending.
8. **Held by a rail if it can be.** *No call to the server between a step's command and its
   end* as a rail in `host_test/rails/` or `tools/check_rails.py`, if there is a shape of it
   that does not cry wolf; otherwise deliverable 6's tests hold it. Say which.
9. **The words.** `docs/update.md` (*A step, and what ends it*; *A session as a job*);
   `docs/bench.md`, for a bench with a scanner image, not run by this session: a session whose
   step is `hist 869.418 869.818 25` leaves sixteen `hist` lines on the server with `seq`
   unbroken. A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The test of 21 November exists and fails without the change; with it sixteen lines and `done`, `seq` unbroken (the test's name, and the failing run quoted).
- [ ] Between a step's command and its end the scripted server sees no call, in every test that runs a step (how that is checked, in one sentence).
- [ ] The lease covers every step before it begins; a program of 600 s never runs on a lease that has ended (the test's name).
- [ ] `kRxBuffer` is tied to the baud rate and the reader's longest absence by a `static_assert`; the report gives the arithmetic and the RAM.
- [ ] Bytes lost at the port are counted; a step that lost any says so in the journal and that line is in the posted result; the result's fields are the same as before (the tests' names).
- [ ] The link's own frames are read as before: the link's tests pass unedited.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The scanner's histogram not adding up: the repeater firmware's pile 3.15. A task of its own
for the port, flow control or another baud rate. meshmaster.dk noticing a gap in `seq`. The
shape of the session's result. The lost answer (parked). The board: no session opens a port.
A release: Thomas presses the button.
