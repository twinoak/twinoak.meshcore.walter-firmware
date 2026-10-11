# 01 — The modem

**Depends on:** phase 2.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §9, §1; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-9, §10 items 5 and 6; the `walter-modem` component at the version in `dependencies.lock` — its `WalterModem.h`, its `Kconfig`, and its `examples/` (`http`, `https`, `positioning` for the clock, and whatever shows power saving) in `managed_components/`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The Walter can get onto the LTE-M network and say what happened when it could
not. Everything above this task sees an **uplink**: *bring it up*, *what time
is it*, *how good is the signal*, *take it down* — and one of a small number of
named failures, each with its own idea of when to try again. The modem library
is behind that interface and nowhere else, so that the question of whether the
Walter stays (`spec/SYSTEM.md` §10 item 6) costs a port and not a rewrite.

## Read first

`WalterModem.h` for the calls this task uses and what each returns — `begin`,
`checkComm`, `getSIMState`, `unlockSIM`, `setOpState`, `getOpState`,
`definePDPContext`, `setNetworkAttachmentState`, the network event handler and
its registration states, `getRAT`/`setRAT`, `getSignalQuality`, `getRSSI`,
`getClock`, `configPSM`, `configEDRX`, `getIdentity`, `reset`, `softReset`,
`sleep` — and which of them block, for how long, and what the response
structure carries. The library is all static methods and owns a UART and a
task: note what it starts at `begin`, what it keeps in RTC memory across deep
sleep, and what its own `sleep()` does to the ESP32, because task 04 has to
decide whether to use it or to sleep the board itself.

Write down, in the report, anything the header says is buggy or deprecated
that this task touches.

## Deliverables

- **`components/hal/Uplink`** — the interface, in terms of what the firmware
  wants, not of AT commands:
  - `bringUp(deadline)` → up, or a failure from the taxonomy below;
  - `takeDown()`; `state()`;
  - `networkTime()` → a UTC time and how it was obtained, or *none*;
  - `signal()` → RSRP, RSRQ, RSSI where the modem gives them, and the
    operator and band;
  - `identity()` → IMEI and ICCID (for the label on the box; see *Privacy*);
  - `powerSaving(request)` → what the network granted;
  - every call takes a deadline and returns by it.
- **The failure taxonomy** (`components/core/uplink/`), an enum and a table —
  the cause, what the console says, and the backoff:

  | Failure | Means | Try again |
  |---|---|---|
  | `modem-silent` | no answer to the first command | after a modem reset, once per wake |
  | `no-sim` | no card, or it does not answer | next wake; not sooner (a person has to act) |
  | `sim-locked` | a PIN is wanted and none is set, or the one set was refused | **never automatically** — a wrong PIN three times locks the card |
  | `not-registered` | searching, denied, or no network within the deadline | growing: next wake, then every second, fourth… up to a cap |
  | `no-data` | registered, no packet data | as `not-registered` |
  | `no-time` | up, but the network gave no time | not a failure of the uplink; reported beside it |

  The backoff state lives in retained storage so that a unit with no coverage
  does not spend its battery searching at every wake. `sim-locked` deserves
  its own test: this firmware sends a PIN **at most once per PIN value** and
  remembers that it was refused.
- **`components/core/uplink/UplinkPolicy`** — portable, over the interface: the
  sequence (is the modem there → SIM → radio on → attach → wait for
  registration → data → time), its deadlines as named constants, the backoff,
  and *how long this wake may spend on the modem at all* — one budget the wake
  cycle hands in, never exceeded.
- **`components/modem/`** — the interface on `walter-modem`. Thin: each method
  is the library calls and the mapping of their results into the taxonomy.
  The APN and the PIN come from settings (task 03; until then, two build
  constants that are empty). LTE-M only unless a setting says otherwise.
- **The network's time**: `getClock` after registration; reject a time before
  this firmware's build date and one absurdly far after it (the modem answers
  with 1970 or its own default until the network has told it); say in the
  result whether it is the network's. This is the time that sets the radio's
  clock (`TIME_SET`), so a wrong one is worse than none.
- **Power saving**: request PSM with timers from named constants, read back
  what was granted, and report both. Whether PSM, eDRX or switching the modem
  off between wakes costs least is a bench question (`spec/SYSTEM.md` §10
  item 5): implement *off between wakes* as the default and PSM as a setting,
  and say so.
- **`host_test/sim/FakeModem`** — an `Uplink` that follows a script: attach
  after N seconds, never attach, drop after attaching, give no time, give a
  time from 1970, a locked SIM, silence. Reused by every later task.
- **The console**: `lte` (state, operator, band, signal, what the last attempt
  did and when the next is allowed), `lte up`, `lte down`, `lte time`,
  `lte id`. `docs/console.md` gains them.

## Privacy

IMEI, ICCID and IMSI identify a subscription. They are printed by `lte id`
when a person asks and are **not** written to the log, the journal, or
anything the firmware sends by itself. A test greps the log sink.

## Tests (host — fake modem, fake clock)

- The good path, and each failure of the table reached by its script, with the
  right console line.
- The backoff: a month of wakes with no coverage — count the attempts and the
  seconds spent searching; they follow the table and stop growing at the cap.
  Coverage returns: the next allowed attempt succeeds and the backoff resets.
- A PIN refused: never sent again until the setting changes.
- Every call returns by its deadline against a fake that never answers.
- The wake's budget: a bring-up that would overrun is abandoned, cleanly, with
  the modem taken down.
- Time: 1970 refused; before the build date refused; a sane one accepted and
  marked as the network's.
- Nothing identifying in the log.

## Acceptance criteria

- [ ] `tools/verify.ps1` green — the firmware builds with the modem component linked and `lte` on the console.
- [ ] `components/core/` still has no ESP-IDF include and no `WalterModem` symbol (a check in `verify.ps1`, if phase 0 did not already make one).
- [ ] Every deadline, backoff step and cap is a named constant with a sentence; the report lists them as "to be set on the bench".
- [ ] The report lists every library call used, whether it blocks, and anything the header marks buggy or deprecated.
- [ ] The report states which claims of this task were tested and which only compile.

## Not verified here (Thomas, with a SIM)

Add to `docs/bench.md`, "10. The modem":

- `lte up` with the SIM in and the antenna on: registered, the operator and
  band named, a signal reading, and `lte time` within a second or two of a
  phone's clock.
- The same with the antenna off, and with no SIM: the right failure, promptly,
  and `lte` says when it will try again.
- How long an attach takes from cold, three times — it sets the deadlines.
- The current drawn while searching and while attached (the Walter's own
  supply, a meter in series): the numbers the policy table of task 04 needs.

## Out of scope

HTTP (02). The APN and PIN as settings (03). Sleep (04). GNSS, MQTT, CoAP,
BlueCherry, sockets: switch them off in the component's Kconfig where that is
possible and say what it saved.
