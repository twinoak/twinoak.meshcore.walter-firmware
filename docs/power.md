# What the battery allows

The Walter and the repeater it looks after share one 1S LiPo — "a single 1S LiPo feeds both
boxes", the hardware repository's `hardware/pcb/INTERCONNECT.md` at `b798aab`, *Power
architecture* — and an LTE attach costs more than the repeater spends in an hour. So the Walter
sleeps, and **the battery chooses how long**: a full pack under a sunny panel can afford a
check-in every quarter of an hour; a sinking one in December gets one a day; and below a floor the
Walter stops using the modem altogether, because a repeater that keeps repeating is worth more
than a report that it is about to stop.

This is that table (task p3-04, `core/power/interval_policy.h`; `docs/ARCHITECTURE.md` §10 is the
wake it belongs to). `power` on the console prints the same numbers off the board.

> **Every number here is a placeholder.** No cell's discharge curve has been measured, nothing has
> drawn current from this pack, and the three figures that would settle them are bench
> measurements: the Walter's draw asleep, the energy of one whole wake with an attach, and this
> pack's voltage against its state of charge. `spec/SYSTEM.md` §10 item 5 is the open question and
> `docs/bench.md` §15 is how it is closed. Change a number and the sentence beside it in
> `interval_policy.h` together.

## The reading

A reading is `POWER_GET`'s channel 2 over the box link: the battery's bus voltage in millivolts
and its current in milliamps, **positive = charging** (`spec/LINK-PROTOCOL.md` §5.8; the shunt is
already applied by the radio, and the standard B-board's channel 1 is the panel and 3 the total
load). The Walter has no gauge of its own — the INA3221 is on the radio's I²C.

One sample is not a reading:

- the **voltage** used is the **median of the last three**, because a sample taken while the modem
  is transmitting sags by whatever the pack's internal resistance and the burst's current make it;
- the **current** used is the **mean over the samples we have**, because what matters is whether
  the panel is actually putting charge in, and one instant of sun is not that;
- five samples are kept, and a reading taken with fewer says how few (`power` prints them).

**No reading at all is its own case.** A silent radio is a Walter with no gauge: the band is
`no-reading`, the interval is the medium one — explicitly *not* `plenty` — nothing that spends
charge on the strength of a figure nobody has is allowed, and whether the radio's rail may be
taken away is left to the supervisor, which has a battery floor of its own over the last good
reading (`core/supervisor/supervisor.h`).

## The bands

| Band | Battery | Interval | A wake may |
|---|---|---|---|
| `plenty` | at or above **4000 mV**, or charging steadily (≥ **50 mA** mean) from `normal` | **15 min** | everything, an update included |
| `normal` | between | **60 min** | everything but an update or a flash of either MCU — unless it is charging |
| `saving` | below **3600 mV** | **360 min** (6 h) | check the radio, read power; the **modem only every 4th wake** |
| `critical` | below **3400 mV** | **1440 min** (a day) | check the radio. **No modem, no flash, no power cycle** |
| `no-reading` | the radio did not answer `POWER_GET` | **60 min** | the modem and the supervisor's own rules; no update, no flash |

### Hysteresis

Leaving a band needs **60 mV beyond the threshold that entered it**, in the direction of travel:
rising out of `saving` into `normal` needs 3660 mV, falling out of `plenty` into `normal` needs
3940 mV, and in between the band stays what it was. A pack sitting on a threshold therefore does
not change band every wake — each change writes a journal entry and changes the interval, and a
unit that oscillates between a quarter of an hour and six hours is a unit nobody can predict.

The band the last wake ended in crosses the sleep in RTC memory
(`interfaces::RetainedSlot::kDutyCycle`). A power cut clears it, and then the thresholds decide
outright — which is the right answer for a Walter that has just come back from nothing.

A host test wanders ±20 mV around each of the three thresholds, twenty times over, and the band
changes **at most once** (`host_test/core/power/test_interval_policy.cpp`).

### Charging

"Above the upper threshold, **or charging steadily**" is read narrowly, and this is the place it is
said out loud: charging steadily lifts `normal` to `plenty`, and lifts nothing else. A cell below
the lower threshold was recently nearly empty, and one sunny quarter of an hour is not a reason to
spend an attach on it — the band rises on its own as soon as the voltage does, which is what the
panel is for. In `normal`, charging is also what permits an update or a flash.

### The radio's own opinion

`EV_ATTENTION` reasons 2 (*battery low*) and 3 (*battery critical*) move the band down **at once**,
without waiting for a reading of our own: the radio has the gauge and it has already looked
(§8; `detail` carries the millivolts). They only ever move it *down* — a radio saying "low" to a
Walter whose own reading is below the floor leaves it below the floor — and when we have no
reading at all, the millivolts the event carried are used as one.

### `wake.interval`

The setting (`docs/console.md`, *Settings*) overrides the first three bands' interval and
**never** `critical`. `auto` — the default — is the battery deciding.

### Faults

A wake that ended in a crash or a watchdog **lengthens** the next sleep rather than shortening it:
each consecutive fault doubles the interval, up to four doublings and never past a day. A Walter
that panics on every wake must not panic sixty times an hour, and a panic caused by something the
next wake would do again is best met with time. One good wake ends the run.

## What a day costs

From the simulated December in `host_test/core/power/test_interval_december.cpp` — a crude model of
a pack and a Danish panel, run through the real policy, and worth only its shape:

| Band | Wakes a day | Modem uses a day |
|---|---|---|
| `plenty` | 96 | up to 96 |
| `normal` | 24 | up to 24 |
| `saving` | 4 | 1 |
| `critical` | 1 | **0** |

The numbers to the right of `plenty` and `normal` are *allowances*, not costs: what a wake
actually does with the modem is the wake cycle's business (task p3-05) and the uplink's own backoff
thins the attempts out when there is no coverage (`core/uplink/failure.h`). `tasks/phase-3/reports/
04.md` has the simulated month's own counts.

## Where the numbers are

| | |
|---|---|
| the thresholds, the margin, the charging current | `components/core/include/core/power/interval_policy.h` |
| the four intervals, `saving`'s Nth, the fault doublings | the same file |
| the bound on a wake, `stay`'s bound, the USB grace | `components/core/include/core/cycle/wake_guard.h` |
| the spurious-wake limit, the attention window | `components/core/include/core/cycle/wake_reason.h` |
| what has to be measured, and how | `docs/bench.md` §15 |
