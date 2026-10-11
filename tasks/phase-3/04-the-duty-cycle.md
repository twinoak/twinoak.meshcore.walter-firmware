# 04 — The duty cycle

**Depends on:** 03.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2 (rules 1 and 5), §10; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §8 (`SLEEP_NOTICE`, `EV_ATTENTION` and why its first bytes are lost), §5.8 (`POWER_GET`: channel 2 is the battery, positive = charging); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) §10 item 5; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/INTERCONNECT.md` (power architecture: one 1S LiPo feeds both boxes), `hardware/pcb/BENCH-BREADBOARD.md` (the power-up glitch on GPIO1–14); ESP-IDF's documentation of deep sleep, wake sources, RTC GPIO and pad holds at the version this repository builds with; the Walter's datasheet for what it draws asleep and what can wake it.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The Walter shares one battery with the repeater it looks after, and an LTE
attach costs more than the repeater spends in an hour. So it sleeps — deeply,
with its seven pins held exactly as they should be — and wakes for three
reasons only: its timer, the radio asking for it, a person plugging in a
cable. How long it sleeps is the battery's decision: a full battery under a
sunny panel can afford a check-in every quarter of an hour; a sinking one in
December gets one a day; and below a floor the Walter stops using the modem
altogether, because a repeater that keeps repeating is worth more than a
report that it is about to stop.

## Read first

What deep sleep does on the ESP32-S3: which memory survives, which pins can
wake it (GPIO8 is an RTC pin — confirm it), the difference between `ext0` and
`ext1`, what a pad hold is and the order of the calls that make a level
survive the transition into and out of sleep **without a glitch**, and what
happens to the USB serial port. And what the modem library does about sleep
(task 01's report): whether this firmware calls the library's `sleep()` or
sleeps the board itself after taking the modem down. Decide, and say why.

**USB as a wake source** is a question to answer, not to assume: find out
whether this board can wake from deep sleep when a cable is plugged in. If it
cannot, the rule becomes: *a person presses reset; a boot that finds a USB
host stays awake* — and the documents say so.

## Deliverables

- **`components/hal/Sleeper`** — sleep until a time or a wake source,
  whichever is first; which source woke us; is a USB host present. And
  retained storage (phase 0's) for what must cross a sleep.
- **`components/platform/`** — on ESP-IDF, thin:
  - before sleeping: `RadioPins::safe()`, then the pads held in that state
    (rule 5), UART TX included — high while the radio is powered, so that the
    radio's receiver does not see a break for hours;
  - the wake pin: GPIO8, **low level** (a start bit), with its pull-up kept
    in sleep so that a radio that is off, resetting or being flashed does not
    hold the Walter awake;
  - after waking: the holds released only once the pins have been configured
    to the same state; the wake cause mapped to the enum of phase 0;
  - the modem down and the task watchdog fed or stopped as the platform
    wants.
- **`components/core/cycle/WakeReason`** — what the wake means, worked out
  before anything is done about it:
  - *timer*: the check-in;
  - *the radio's line*: listen for up to the attention's whole repeat window
    (§8: ten repeats, a second apart) for a valid `EV_ATTENTION`; a frame →
    its reason; **nothing valid → a spurious wake**, counted. Spurious wakes
    are rate-limited: after a few in an hour the line is disarmed as a wake
    source until the next timer wake, and that is reported — a chattering line
    must not be able to keep the Walter awake;
  - *USB*: stay awake while the host is there, plus a grace period; the
    console says so at the prompt;
  - *power-on or reset*: the radio was reset with us (the glitch); give it
    time to boot before judging it (the supervisor already knows this);
  - *a crash or a watchdog*: note it; too many in a row lengthen the sleep
    rather than shorten it.
- **`components/core/power/IntervalPolicy`** — portable, a pure function of
  (battery millivolts, battery current averaged over the readings we have,
  the previous band, the settings) → the next interval and what the wake may
  do:

  | Band | Battery | Interval | The wake may |
  |---|---|---|---|
  | `plenty` | above the upper threshold, or charging steadily | short | everything, an update included |
  | `normal` | between | medium | everything but an update of either MCU unless charging |
  | `saving` | below the lower threshold | long | check the radio, read power; modem only every Nth wake |
  | `critical` | below the floor | longest | check the radio; **no modem, no flash, no power cycle** |

  Thresholds in millivolts for one LiPo cell, **with hysteresis** between
  bands (leaving a band needs a margin beyond the threshold that entered it,
  and a reading is a median of a few, never a single sample taken while the
  modem was transmitting); every number a named constant marked
  *placeholder — to be set on the bench*. `wake.interval` set to minutes
  overrides the first three bands' interval and **never** `critical`. No
  reading at all (the radio is silent) is its own case: a medium interval and
  the supervisor's rules, not `plenty`.
  The radio's own attention reasons 2 and 3 (battery low, critical) move the
  band down at once without waiting for our own reading.
- **Going to sleep** (`components/core/cycle/Sleep`): the one way out of a
  wake. Journal *sleeping until <time>, because <band>*; tell the radio
  (`SLEEP_NOTICE` with the seconds, as the last request); take the modem
  down; pins safe; sleep. If an update of either MCU is mid-step, the step is
  finished or left at a journalled point first.
- **Staying alive while awake**: a watchdog over the whole wake — a wake that
  has not reached sleep within a hard bound (a named constant, minutes) is
  ended by reset; the next wake sees a watchdog reason. Without a USB host,
  the Walter is **never** awake indefinitely.
- **The console**: `sleep now`, `sleep <seconds>`, `wake` (why this wake
  happened, the last ten with their reasons and durations, the spurious
  count), `power` (the band, the readings behind it, the next interval), and
  `stay` / `stay off` (keep awake without a cable, for the bench, bounded by a
  timeout).

## Tests (host — fake sleeper, fake clock, scripted radio, fake pins)

- The pins at the moment of sleeping are `safe()` in every path that sleeps,
  errors included — the fake pins assert it when `sleep` is called.
- Each wake reason reaches its meaning; a radio-line wake with a valid
  attention on the 1st, the 4th and the 10th repeat; one with noise only; one
  with a scanner's text.
- Spurious wakes: disarmed after the limit, re-armed at the next timer wake,
  reported.
- The policy as a table test over a grid of voltages and currents: the band
  and interval for each, and **no oscillation** — a voltage wandering ±20 mV
  around each threshold changes band at most once.
- A simulated December: a discharge curve with short charging bumps, a wake
  per interval — count the wakes and the modem uses per day in each band.
- A simulated week with the radio silent: the supervisor's rules and a medium
  interval, never `plenty`.
- `wake.interval` overrides three bands and not the fourth.
- The hard bound: a wake that hangs is ended; the next wake knows.
- USB present: no sleep; USB gone: sleep after the grace period.
- `SLEEP_NOTICE` is the last request of every wake that had a link.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report answers the USB-wake question with its source, and says which of the library's `sleep()` and the board's own this firmware uses, and why.
- [ ] The order of calls around sleep entry and exit that keeps nRESET from glitching is in the code as a comment citing ESP-IDF's documentation, and in the report.
- [ ] The policy table, with every threshold and interval, is in `docs/power.md`, each number marked placeholder.
- [ ] The report gives the simulated December's wakes and modem uses per day.

## Not verified here (Thomas, with the rig and a meter)

Add to `docs/bench.md`, "13. Sleep":

- The Walter's current asleep, modem off — the datasheet's figure is the
  target; anything a few times higher is a pin being driven or a peripheral
  left on.
- **A scope on nRESET and on KILL across sleep entry and wake**: no pulse.
  The RAK's uptime (`rak status`) keeps counting across ten sleeps.
- From the repeater's console, the command that raises attention reason 4:
  the Walter wakes, and `wake` says *attention: a person asked*.
- Unplug the RAK's UART while the Walter sleeps: it does not wake, or wakes
  once and disarms.
- The energy of one whole wake with an attach (a meter that integrates, or
  the INA3221's own readings before and after): the number that turns the
  policy's placeholders into a table.

## Out of scope

What a wake does (05). eDRX and PSM tuning (a bench pile). GNSS.
