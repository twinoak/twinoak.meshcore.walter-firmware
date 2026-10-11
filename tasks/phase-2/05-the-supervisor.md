# 05 — The supervisor

**Depends on:** 03.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §8, §2; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.1–§5.4, §8, §9; the hardware repository's `software/software-goals.md` at `b798aab043281fa4c9b17802a164fa94be12e206` (must-haves 2 and 6: an autonomous watchdog with power-cycle; boot-loop detection).
**Size:** one session.
**Model:** claude-opus-5

## Goal

When the radio goes quiet, the Walter does something about it — the least thing
that might work, then the next, each after a wait, each a limited number of
times a day — and when nothing works it stops and says so. A supervisor that
does too little leaves a repeater dead for weeks; one that does too much
power-cycles a radio every minute and drains the battery it shares. Both are
failures, and the second is the one a bug produces.

## Deliverables

- **`components/core/supervisor/Supervisor`** — one step per call, over the
  link client, `RadioPins`, the flasher's `verify`, the radio-update machine,
  the journal, a clock and the battery:
  - **Health** is a `PING` answered. A healthy radio also has its clock set:
    after any restart the supervisor sees (`EV_BOOT`, or a new `boot_id`) it
    sets the time when it has one, and re-reads `STATUS`.
  - **What silence means** is worked out before acting (LINK-PROTOCOL §9):
    the scanner is running (text lines: not a fault — report it, leave it);
    the Walter itself has just powered up (the radio was reset by our own
    power-up glitch and is still booting: wait); an update is in progress (not
    ours to disturb); or the radio really is not answering.
  - **The ladder**, each rung only after the one below failed, each followed
    by a wait before judging:
    1. ask again later (the link's own three tries, a pause, three more);
    2. a reset pulse;
    3. a power cycle through the kill line;
    4. check what is on the chip (`verify` the golden image against the flash,
       changing nothing): if it differs or cannot be read, flash the golden
       image through the update machine's rollback path; if it matches, the
       firmware is intact and flashing would change nothing — skip to 5;
    5. **stop**: journal *radio unresponsive after <what was tried>*, raise it
       for the next check-in, and do nothing more until a person or the server
       says otherwise, or until a long quiet period has passed (a build
       constant measured in days) after which the ladder may be tried once
       more.
  - **Limits**: per-rung waits that grow, and **caps per day** — resets,
    power cycles, flashes — kept in retained storage and the journal so that
    they survive the Walter's own restarts and sleep. A cap reached is a
    refusal that is itself reported.
  - **Boot loops**: several `EV_BOOT`s within a window, or a `STATUS` whose
    uptime never gets past a threshold, is a radio restarting itself; it takes
    the ladder from rung 3 (a reset will not cure what a reset is already
    doing) — and its reset reasons are kept for the report.
  - **The battery decides too**: no power cycle and no flash below their
    thresholds. A radio that browned out because the battery is empty needs
    sun, not a reflash; `POWER_GET`'s last good reading and the reset reason
    are what tell the two apart.
  - **Attention** (§8): an `EV_ATTENTION` from the radio is acknowledged by the
    next request, recorded with its reason, and handed to whoever runs the
    wake (phase 3).
- **What it reports**: a compact record per incident — when, what was seen,
  each rung tried and its outcome, how it ended — bounded in number, kept for
  the next check-in.
- **The console**: `rak watch` (the supervisor's view: healthy since, last
  incident, today's counts against the caps), `rak watch reset-caps` for the
  bench, and `rak ladder` to run the ladder now, printing each rung.
- **`docs/supervisor.md`**: the ladder, the waits and caps as a table, how the
  four kinds of silence are told apart, and what a person should do when it
  has stopped.

## Tests (host — scripted radio, fake pins, simulated chip, fake clock)

- A healthy radio for a simulated week: no action, no journal entries beyond
  the heartbeat's own.
- Silence cured at each rung in turn: only the rungs below it were used, in
  order, with the waits between them (assert the fake clock's times).
- Silence that nothing cures: the ladder runs once, stops, and stays stopped
  for the quiet period; then tries once more.
- Rung 4: the chip holds the golden image → no flash; the chip differs → the
  golden image is flashed; the chip is protected or unreadable → stop with
  that reason.
- **The caps**: a radio that fails again after every cure reaches each day's
  cap and no more — across simulated restarts and sleeps of the Walter.
- A boot loop: detected, ladder from rung 3.
- The scanner running, the Walter's own power-up, an update in progress: no
  action in each.
- A low battery: no power cycle, no flash, and the incident says why.
- The pin rules hold through every scenario (the fake pins check them), and
  no two resets are ever less than a second apart.
- A property test: for random sequences of radio behaviour over a simulated
  month, the daily counts never exceed the caps.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The caps hold in the property test and across the Walter's restarts.
- [ ] Every wait, cap, window and threshold is a named constant; `docs/supervisor.md` tabulates them and marks them "to be set on the bench".
- [ ] The report shows one incident record as it would be sent.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "9. The supervisor":

- Hold the RAK's reset to ground with a jumper: `rak ladder` asks, resets,
  power-cycles, checks the chip, and stops, with waits between; `rak watch`
  shows the incident and the counts.
- Remove the jumper during the ladder: it notices the radio is back and stops
  climbing.
- Flash the RAK over USB with something else (stock MeshCore): the supervisor
  finds the chip differs from golden and puts the golden image back.
- Power-cycle the Walter: the RAK restarts (the glitch) and the supervisor
  does **not** call that an incident.

## Out of scope

When the supervisor runs (the wake cycle, phase 3). Reporting to a server.
