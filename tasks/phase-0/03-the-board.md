# 03 — The board

**Depends on:** 01.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2 (every rule); the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/INTERCONNECT.md` (the D-sub table, "Kill line behaviour", "UART link"), `hardware/pcb/BENCH-BREADBOARD.md` §3 and §6 (the caveats, all twelve), `PROJECT-KNOWLEDGE.md` §6.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

This firmware shares seven pins with a radio on a mast. Before any code that
*uses* those pins exists, the rules for them do — in one place, enforced, and
tested against pins that are not real: reset is open-drain and never driven
high; everything is released before the rail is killed; two resets are a second
apart; nothing is left driven when there is nothing to say. Then a console to
exercise them by hand, and a record of why the last boot happened.

It is Fable's because a mistake here is a radio held in reset, or back-fed
through its protection diodes, until somebody climbs up.

## Deliverables

- **`components/hal/`**: the pin interface the core is written against —
  set a pin's mode (input, output, open-drain), write, read, a delay, a
  monotonic clock, "hold these pads through deep sleep" — and nothing
  ESP-specific in its signatures.
- **`components/core/board/`**:
  - **the pin map** as named constants with the net, the D-sub pin and the
    GPIO, from `INTERCONNECT.md` — and a host test that reads the table of
    ARCHITECTURE §2 in the document and fails if the constants disagree with
    it (the document and the code must not drift);
  - **`RadioPins`**, the only object allowed to touch those pins, with
    exactly these operations: `safe()`, `resetPulse()`, `killCycle(off_ms)`,
    `uartEnable()` / `uartRelease()`, and — for phase 1 — `swdAcquire()` /
    `swdRelease()` handing out the clock and data pins and taking them back.
    Every rule of ARCHITECTURE §2 is enforced inside it:
    - `safe()` is the state at construction and after every operation that
      fails part-way;
    - `resetPulse()` is output-low for 100 ms then input; it **waits** rather
      than violates the one-second spacing; the time of the last reset
      survives deep sleep (through the hal's retained storage);
    - `killCycle()` releases the UART and SWD pins and the reset pin first,
      refuses a duration outside its bounds, drives the kill line high, waits,
      **stops driving it** (it does not drive it low), waits for the rail to
      settle, and only then lets the UART be enabled again;
    - nothing can acquire SWD while the rail is believed off; nothing can
      toggle the SWD clock while reset is asserted (that combination is
      reserved for the dual adapter's selector, phase 1 task 06);
    - there is **no** call that sets the reset pin to output-high, and the
      type system makes that hard to add by accident.
  - what it believes about the radio's power — on, off, unknown — as state the
    supervisor will read.
- **`components/platform/`**: the hal on ESP-IDF's GPIO driver: the modes, the
  pad holds for deep sleep, the retained storage (RTC memory, with a checksum).
  Thin. In `app_main`, `safe()` is the first thing that happens.
- **Why the last boot happened** (`components/core/health/` + platform): the
  ESP32's reset reason and wake cause mapped to a small enum (a portable
  mapping with a test), a boot counter, and the last five reasons with their
  uptimes, kept in retained storage — the agent firmware's `Health` is the
  model.
- **A console** on the module's USB (`components/core/console/` for the line
  parser and the command table, portable; platform for the bytes): one command
  per line, a `>` prompt, as the agent firmware's console behaves. Commands in
  this task: `help`, `status` (version, uptime, why it booted, the pins' state
  and what is believed about the radio's power), `reboots`, `rak reset`,
  `rak kill <seconds>`, `pins`, `reboot`. Unknown commands and bad arguments
  get one line saying what was wrong. `docs/console.md` starts here.

## Tests (host)

Against **fake pins that record every transition with its time** and check the
rules themselves:

- at no point in any test is the reset pin output-high — the fake fails the
  test the moment it sees it;
- `resetPulse()` twice in a row: the second starts at least a second after the
  first ended; across a simulated deep sleep too;
- `killCycle()`: the recorded order is release-everything → kill high → wait →
  kill released → settle; too short and too long are refused; a failure
  injected in the middle ends in `safe()`;
- SWD cannot be acquired with the rail off; the clock cannot be toggled with
  reset asserted;
- the pin map against the document;
- the console parser: every command, a blank line, an over-long line, a bad
  number, an unknown command; and a property test — a few thousand random
  command sequences against the fake pins, none of which breaks a rule.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The fake pins' rule checks exist and each fired once on purpose (show the change, then revert).
- [ ] `grep` finds no direct GPIO call outside `components/platform/`, and no use of the seven pins outside `RadioPins`.
- [ ] The report states the bounds chosen for the kill duration and the settle time, and what in the hardware documents they come from — or that they are placeholders for the bench.

## Not verified here (Thomas, with the rig and a scope)

Add to `docs/bench.md`, "2. The pins":

- At power-up of the Walter, the reset line dips for tens of microseconds (the
  ESP32-S3's own glitch) and the radio may restart: expected, not this
  firmware's doing. After that nRESET, SWDCLK, SWDIO sit undriven.
- `rak reset`: one 100 ms low pulse on nRESET, never above the radio's own
  pull-up level; the radio restarts once. Twice quickly: the second pulse comes
  a second later and the radio does **not** land in its UF2 bootloader.
- `rak kill 5`: TX, nRESET and the SWD pins go quiet before the kill line
  rises; the LoRa rail falls and returns; nothing is back-fed while it is down
  (the rail sits at 0 V, not at a diode drop).
- Reset the Walter while a kill is in progress: the rail comes back by itself.
- The numbers that replace the placeholders: how long the rail takes to fall
  and to settle.

## Out of scope

Using the UART (phase 1, task 02) or SWD (03). Deep sleep itself (phase 3) —
this task only provides the holds.
