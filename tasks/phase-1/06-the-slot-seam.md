# 06 — The slot seam

**Depends on:** 02, 05.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-11; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.1 (`slot`); [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/DUAL-RAK4630-FEASIBILITY.md` (§2 and §3: how one set of wires reaches two radios).
**Size:** one session — a small one.
**Model:** claude-opus-5

## Goal

A unit has one radio today. A dual adapter with two is designed and not built:
the same seven wires, and a selector on the adapter that the Walter steers by
clocking SWDCLK while nRESET is held low. This task makes "which radio" a
parameter everywhere it will need to be one — so that the second radio is an
addition later, not a rewrite — and leaves the unit exactly as it was: one
slot, selector off.

## Deliverables

- **`Slot`** — a small value type (0, 1; today only 0 exists) — and a
  **`TargetSelect`** interface in `components/core/board/`: `select(slot)`,
  `selected()`.
  - `SingleTarget`: accepts slot 0 and does nothing; refuses any other.
  - `DualTarget`: the feasibility document's selector protocol — with nRESET
    held low, shift two bits in on SWDIO with SWDCLK (which radio the debug
    lines and the reset reach; which radio the UART reaches), release — written
    from that document's §3, **behind `RadioPins`** (this is the one sanctioned
    use of "the clock toggles while reset is asserted", and `RadioPins` gains
    exactly one operation for it, with the reset spacing rule still applied to
    the reset it implies). It is compiled, tested against a simulated selector,
    and **not constructed by the firmware**: the board profile says
    `rak4630-single`, and a single-radio unit must never emit that sequence —
    a host test holds it.
- **Every radio-facing operation takes a slot**: the link client's session
  state (one per slot: `boot_id`, role, capabilities), the flasher and its
  board profile lookup, `identify`, the console's `rak …` commands (an
  optional `@1`; default `@0`). With one slot the behaviour is unchanged; with
  a slot that does not exist, a clear refusal.
- **One selection at a time**: an operation on a slot selects it first, and
  two operations on different slots cannot interleave (a guard object; the
  link client is not mid-request when the UART is steered elsewhere).
- **`host_test/sim/Selector`**: the adapter's two flip-flops as the feasibility
  document draws them, watching the same fake pins; power-on state "radio A";
  two simulated chips and two scripted radios behind it, so a test can flash
  slot 1 and see slot 0 untouched.
- **`docs/ARCHITECTURE.md` §2** gains the selector's rule and where it is
  switched off.

## Tests (host)

- Single: nothing changes — the phase's earlier tests pass with the slot
  parameter defaulted; slot 1 is refused; the selector sequence never appears
  on the pins (the fake pins fail the test if the clock moves while reset is
  low).
- Dual, against the simulated selector: select B, flash B, verify A's flash log
  is empty; select A, `hello` reaches A's scripted radio and not B's; the
  power-on default is A; a reset pulse reaches only the selected radio.
- The guard: an attempt to select another slot mid-request is refused or
  waits, and never corrupts the request.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; every earlier host test still passes unchanged in meaning.
- [ ] The firmware as built constructs `SingleTarget` only (show where), and the host test that forbids the selector sequence on a single-radio unit failed once on purpose.
- [ ] The report lists the calls that gained a slot.

## Not verified here

The dual adapter does not exist. Nothing on the single adapter changes; the
bench lines of tasks 02 to 05 are unaffected.

## Out of scope

Two image stores, two supervisors, a bridge — when there is a board.
