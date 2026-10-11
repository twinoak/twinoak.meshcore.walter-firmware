# 03 — The SWD wire

**Depends on:** phase 0 (task 03: `RadioPins`).
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2, §4 ("The wire", "The debug port", "The simulator"); Arm, *Debug Interface Architecture Specification ADIv5.0 to ADIv5.2* (IHI 0031) — the Serial Wire Debug protocol and the DP register chapters; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/BENCH-BREADBOARD.md` §4 (what the filtered lines do to SWD).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The Walter can speak Serial Wire Debug: reset the line, select SWD, and read
and write the debug port's registers with every ACK, parity bit and turnaround
handled. It is developed against a **simulated target that works at the level
of the two wires** — it sees our clock edges and our data bits, and answers as
a debug port does — because that is the only way to find a wrong parity bit or
a missing turnaround without a board.

## Starting facts — verify each against the specification and cite it

- Two wires, SWCLK driven by us, SWDIO shared. The target samples and changes
  SWDIO on SWCLK's rising edge; we change it on the falling edge and sample
  before the next rising edge.
- **Line reset**: at least 50 clocks with SWDIO high. **JTAG-to-SWD**: the
  16-bit sequence `0xE79E`, least significant bit first. Then a line reset
  again, at least two idle clocks low, and a read of `DPIDR` — which the
  protocol requires before anything else.
- **A request** is 8 bits, LSB first: start (1), APnDP, RnW, A2, A3, parity
  (of the four before it), stop (0), park (1). Then one turnaround cycle, then
  **ACK**, 3 bits LSB first: OK `001`, WAIT `010`, FAULT `100`.
- **A read**: after ACK, 32 data bits LSB first and a parity bit from the
  target, then a turnaround. **A write**: a turnaround after ACK, then 32 data
  bits and parity from us.
- On WAIT or FAULT there is no data phase (unless overrun detection is on,
  which we leave off). Something other than the three ACKs is a protocol error:
  nothing answered, or the line is confused.
- DP registers by address: `0x0` `DPIDR` (read) / `ABORT` (write), `0x4`
  `CTRL/STAT`, `0x8` `SELECT` (write) / `RESEND` (read), `0xC` `RDBUFF` (read).
  `ABORT` clears the sticky flags; `CTRL/STAT` holds the power-up requests and
  their acknowledgements.
- Idle clocks after a transfer let the target finish it; how many are needed
  is in the specification.

## Deliverables

- **`components/core/swd/Wire`**: the bit-level protocol over two pin
  operations and a half-period delay (from `RadioPins::swdAcquire()` and the
  hal). The half-period is a parameter: **default 5 µs (100 kHz)**, bounded
  below so that nobody sets it faster than the filtered lines can carry
  without meaning to. SWDIO is an input whenever the target owns the bus.
  Where in the clock cycle a read bit is sampled matters here: the line has a
  time constant near 1 µs and the target's driver has to win a turnaround
  through it. Decide the sample point from the specification's timing and that
  number, and write the reasoning in a comment.
- **`components/core/swd/Dp`**: `connect()` (line reset, switch, line reset,
  idle, `DPIDR`), `readDp(addr)`, `writeDp(addr, value)`, `readAp(addr)` /
  `writeAp(addr, value)` with `SELECT` managed (the access port and register
  bank last selected are remembered and rewritten only when they change),
  `powerUp()` through `CTRL/STAT` with a bounded wait for the acknowledgements,
  `clearErrors()` through `ABORT`.
  - **WAIT** is retried a bounded number of times with idle clocks between;
    then it is an error.
  - **FAULT** is reported with `CTRL/STAT`'s sticky bits read and cleared.
  - **A parity error** on read data fails the transfer; it is never silently
    accepted.
  - **No ACK at all** fails `connect()` with "no target", distinct from
    everything else — the supervisor needs to tell an unpowered radio from a
    confused one.
  - AP reads are posted: the value arrives with the next AP read or with
    `RDBUFF`. The API hides that correctly — test it.
  - Every operation returns a result that says which of these happened; none
    throws, none loops without a bound.
- **`host_test/sim/SwdTarget`** — the wire-level simulator: it is driven by
  the same two pin operations (it implements the fake pins' interface for SWCLK
  and SWDIO, watching edges), decodes line resets, the switch sequence and
  requests, **checks our parity and our turnarounds and fails the test with a
  description when the host breaks the protocol**, and implements a debug port:
  `DPIDR` (the nRF52840's value, `0x2BA01477` — verify), `CTRL/STAT` with
  power-up acknowledgement, `SELECT`, `RDBUFF`, `ABORT`, the sticky flags. Its
  access ports are a plug-in point with a register-level interface, used by
  task 04; for now one stub AP that stores what is written. It can be told to:
  answer WAIT the next N times; FAULT; return bad parity once; go silent; not
  be there at all; require the line reset to be long enough.
- **The console**: `rak swd connect` — acquire the pins, connect, print `DPIDR`
  and the power-up state, release the pins. Nothing else yet.

## Tests (host)

- `connect()` against the simulator: `DPIDR` read; the simulator saw a legal
  sequence. With the line reset shortened by one clock (through a test-only
  hook): the simulator rejects it and the test expects that.
- DP read and write of each register; AP read through the posted-read
  mechanism; `SELECT` written only when needed (count the writes).
- WAIT N times then OK; WAIT forever (bounded, an error); FAULT with sticky
  flags cleared afterwards; one bad parity; silence; no target.
- The simulator's own protocol checks each fired once on purpose: a wrong
  request parity, a missing turnaround, SWDIO driven during the target's
  phase.
- `RadioPins` rules still hold through all of it (the fake pins of phase 0 are
  underneath): the pins are released on every exit path, including the error
  ones.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] Every starting fact above is either cited (document, section) in the code where it is used, or corrected here in the report with the citation.
- [ ] The simulator rejects a malformed host; the three deliberate protocol errors are shown in the report.
- [ ] No unbounded loop in `Wire` or `Dp`.
- [ ] The report states the time one transfer takes at the default half-period, from the bit count.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "4. SWD":

- `rak swd connect` prints `DPIDR 0x2BA01477`, first with short wires, then
  through 470 Ω and 2.2 nF on each line, then through the real bulkheads.
- With a scope: SWDIO's edges against SWCLK at the default speed — is the
  sample point clear of the turnaround? Raise the speed until `connect` fails
  and record the ceiling (predicted near 300 kHz).
- With the RAK unpowered: "no target", and nothing is back-fed.

## Out of scope

Memory access, the nRF52, flash (04, 05).
