# The supervisor

Task p2-05. `components/core/supervisor/supervisor.h`, `docs/ARCHITECTURE.md` §8 and §2,
`spec/LINK-PROTOCOL.md` §5.1–§5.4, §8 and §9, `spec/SYSTEM.md` §5 and §6, and the hardware
repository's `software/software-goals.md` at `b798aab` — must-have 2, *an autonomous watchdog with
a power-cycle*, and must-have 6, *boot-loop detection*.

## The idea in one paragraph

The radio is healthy when it answers `PING` (§5.2: *it proves the firmware's main loop is turning,
which a reset line cannot*). When it stops answering, the Walter works out **what the silence
means** before it does anything, and then does the **least thing that might work**, then the next,
each after a wait and each a limited number of times a day. If nothing works it **stops**, records
what it tried, and leaves the radio alone until a person, the server, or a long quiet period says
otherwise. A supervisor that does too little leaves a repeater dead for weeks; one that does too
much power-cycles a radio every minute and drains the battery the two share. Both are failures and
the second is the one a bug produces — which is why every expensive rung is counted, capped,
journalled and refusable, and why a refusal is itself reported.

**When the supervisor runs is not decided here.** Phase 3's wake cycle calls `step()` once a wake
(`docs/ARCHITECTURE.md` §10); in this build the only thing that drives it is the console's `rak
ladder`, and `begin()` at every boot so that the day's counts and a stop in force are read and
announced. Reporting to a server is phase 3's and phase 4's; what this keeps is the record the
report will be made from.

## The four kinds of silence (§9)

Worked out before anything is done, in this order:

| What | How it is told | What happens |
|---|---|---|
| **the scanner is running** | the link client is in text mode (§9: NDJSON lines, no frames) | not a fault. Reported (`rak watch` says so) and left alone |
| **this Walter has just powered up** | the boot's reason is a power-up or a brown-out and the asking began inside `kWalterPowerUpGraceMs` of it — or the rail is still settling | wait. A power-up of the ESP32-S3 glitches GPIO1–14 low for about 60 µs and that reaches nRESET (§2 rule 5): **our own** power-up reset the radio, and it is still booting |
| **an update is in progress** | the radio-update machine is active, or the journal says a run of it is unfinished, or the Walter's own update is unconfirmed (S-7) | not ours to disturb |
| **it really is not answering** | none of the above | the incident, and the ladder |

A session decides the first of these, not a moment: §9 says a Walter that has decided it is talking
to NoiseScope *treats the UART as text until the next reset of the RAK*, and the same holds the
other way — a radio that was answering frames and is now running the scanner is not recognised
until something resets it. The ladder's second rung is a reset, so the ladder finds out; the climb
then ends there, because something is running over there and it is not this unit's business to
reflash it from under a person.

## The ladder

Each rung is tried only after the one below it failed, and each is followed by a wait before the
radio is judged again. A radio that answers at any point ends the incident there and the rungs
above it are not used.

| # | Rung | What it does | Capped | Battery |
|---:|---|---|---|---|
| 1 | ask again later | the link's own three attempts have already gone (that is what found the silence); a pause, and three more | — | — |
| 2 | a reset pulse | `RadioPins::resetPulse()`: open-drain low for 100 ms, and never two inside a second (§2 rule 2) | `kResetsPerDay` | — |
| 3 | a power cycle | `RadioPins::killCycle()`: every other pin a plain input first, the kill line driven high for `kKillOffMs`, then released and the rail given its settle time (§2 rules 3 and 4) | `kPowerCyclesPerDay` | `kPowerCycleMinMv` |
| 4 | what is on the chip | `Flasher::verify()` of the golden image against the flash, **changing nothing**. It matches and the bootloader would start it → the firmware is intact, so flashing would change nothing: rung 5. It differs → the golden image goes back through the update machine's rollback path (`rak role repeater`'s machine, journalled as one run). Protected, or unreadable → rung 5 with that reason | `kFlashesPerDay` | `kFlashMinMv` |
| 5 | stop | *radio unresponsive after <what was tried>*, journalled and kept for the next check-in. Nothing more until a person, the server, or `kQuietPeriodMs` — after which the ladder may be tried once more | — | — |

**Boot loops.** Several `EV_BOOT`s inside `kBootLoopWindowMs`, or a radio whose uptime never gets
past `kBootLoopUptimeS` when asked twice that far apart, is a radio restarting itself. It takes the
ladder **from rung 3** — a reset will not cure what a reset is already doing — and rungs 1 and 2
are recorded as *skipped* so the report says why they were not used. Its reset reasons (§5.3) are
kept. A radio that answers and keeps restarting is an incident all the same.

**The battery decides too.** A radio that browned out because the battery is empty needs sun, not a
reflash. The gauge is the INA3221 on the *radio's* I2C (§5.8's `POWER_GET`), so the moment a reading
is most needed is the moment it cannot be taken — which is why the last good reading is kept
(`core/update/last_good_battery.h`) and is what rungs 3 and 4 are judged against, for as long as
`kBatteryStaleAfterMs`. A reading that has never been had does not refuse rungs 2 and 3: a reset and
a power cycle are partly how one finds out whether the radio is there to be asked at all.

**Attention** (§8). An `EV_ATTENTION` is read whenever `step()` looks at the port, recorded with its
reason and its detail, journalled, and acknowledged by the next request this object makes — §8's
own mechanism, the radio repeating it until any valid request arrives. The repeats of one attention
are one record. `takeAttention()` is how phase 3's wake hands it on.

## The caps and the day

Per day: **resets, power cycles, flashes**, each with its own cap. The day is 24 hours from the
first counted action of the window, not midnight — a Walter has no time of day until phase 3's
modem reads it from the network, and a cap that only works once the modem does is not a cap.

Everything is kept **twice**: in retained storage (RTC memory, which a reset and a deep sleep
survive and a power cut does not) and in the journal (`kind::kSupervisor`), whose every entry
carries the counts after it. At boot the retained record is read, or flash when a power cut took it.

**A cap is forgiven only when a whole day can be shown to have passed.** The three ways the showing
fails all keep the counts, which is the side to err on:

| | What is shown | What happens |
|---|---|---|
| a restart of the Walter | RTC memory is intact: the window and its counts are exactly where they were | nothing is forgiven, nothing is lost |
| a wake from deep sleep | ESP-IDF's monotonic clock restarts at zero (`interfaces/clock.h`), so the window's age cannot be measured | the window is re-anchored at the wake and the counts stay (`rak watch` says *re-anchored*) |
| a power cut | RTC memory is gone; the journal is not | the counts come back from the journal (`rak watch` says so) and the window is re-anchored |
| a time of day is known | the window is dated: the epoch governs | a day is a day, whatever the Walter's own clock did |

`rak watch reset-caps` is the bench's way out, and it is journalled like everything else.

## The numbers — to be set on the bench

Every wait, cap, window and threshold is a named constant in
`components/core/include/core/supervisor/supervisor.h`, with a sentence beside it. **Not one of
them has met a real radio**; `docs/bench.md` §10 is how they are measured. Change a number and its
sentence together.

| Constant | Today | What it is |
|---|---:|---|
| `kHeartbeatIntervalMs` | 60 000 ms | how often the heartbeat asks, at most. The wake cycle calls `step()` once a wake, so this only bounds a caller that steps in a loop |
| `kAskAgainPauseMs` | 30 000 ms | rung 1: the pause before the second three attempts |
| `kAfterResetMs` | 30 000 ms | the wait after the reset pulse before the radio is judged. A repeater boots in seconds; this is the update's own boot window with room |
| `kAfterPowerCycleMs` | 60 000 ms | the wait after the rail has been and gone: the radio's RAM is gone and it boots from cold |
| `kAfterGoldenImageMs` | 120 000 ms | the wait after the golden image went back. The update machine has already proved the image itself, so this is only the room between the two |
| `kKillOffMs` | 5 000 ms | how long the rail is held off at rung 3 (bounded by `kKillTiming`: 1 s to 30 s, itself a placeholder) |
| `kResetsPerDay` | 6 | one every four hours, which no healthy unit needs |
| `kPowerCyclesPerDay` | 3 | the rail collapsing three times, which a radio that is merely hung does not need |
| `kFlashesPerDay` | 1 | a golden image that did not take once will not take twice |
| `kCapWindowMs` | 86 400 000 ms | the day the caps are counted in |
| `kQuietPeriodMs` | 3 days | after rung 5: long enough that a mast with a hardware fault is not visited every hour, short enough that a radio which recovers on its own is found again without a person |
| `kBootLoopWindowMs` | 600 000 ms | the window the radio's own restarts are counted in |
| `kBootLoopBoots` | 3 | restarts inside that window that make it a boot loop |
| `kBootLoopUptimeS` | 120 s | an uptime that never gets past this, asked twice that far apart, is the other way a loop shows |
| `kWalterPowerUpGraceMs` | 20 000 ms | how long after **this Walter's** power-up a silent radio is still booting, not a fault |
| `kPowerCycleMinMv` | 3 500 mV | below this the rail is not cut |
| `kFlashMinMv` | 3 700 mV | below this nothing is reflashed (`kBatteryMinMv` of the update machine, which checks it again itself) |
| `kBatteryStaleAfterMs` | 6 h | how old the last good reading may be and still be used (`core/update/last_good_battery.h`) |
| `kAttentionRepeatWindowMs` | 11 000 ms | §8's ten repeats, a second apart, read as one attention |
| `kIncidentsKept` | 3 | incidents in retained storage; the journal keeps the whole history |

## What it reports

One compact record per incident — when it began (with the epoch, when there was one), what was
seen, each rung tried and its outcome, how it ended, the last good battery reading, the radio's
last known reset reason and how many times it had restarted. Three of them are kept in retained
storage, so a check-in after a sleep has them without a flash read; the journal keeps the whole
story. One as `rak watch` prints it:

```
> rak watch
rak watch: slot 0: the radio is silent; the supervisor is stopped
  healthy since 166 s ago; last answer 166 s ago; 1 pings; its uptime 3600 s; boot_id 0x01f0163e
  its last reset reason 2 (reset pin); the clock was set
today: resets 1/6, power cycles 1/3, golden-image flashes 0/1
  the day began 118 s ago of 86400 (dated: a whole day can be shown to have passed)
battery: 4000 mV, read 77 s ago (a power cycle needs 3500 mV, a flash 3700 mV)
the radio restarted 0 times in the last 600 s (1 in all); a boot loop is 3 of them
STOPPED: the chip holds the golden image: this is not a firmware fault
  rungs tried: 1, 2, 3, 4; nothing more for 259200 s, or until 'rak watch reset-caps', 'rak ladder' or the server
the last 1 incidents, newest first:
  1. 159 s ago (epoch 1760000000): the radio went quiet; stopped: the chip holds the golden image: this is not a firmware fault
     lasted 159 s; rungs tried: 1, 2, 3, 4, 5; battery 4000 mV; the radio's last reset reason 2 (reset pin)
     1 restarts of the radio, its last uptime 3600 s
     rung 1, ask again later: done; the radio did not come back
     rung 2, a reset pulse: done; the radio did not come back
     rung 3, a power cycle: done; the radio did not come back
     rung 4, what is on the chip: the chip holds the golden image: nothing to flash
     rung 5, stop: done; the radio did not come back
```

That one is from the host tests (`host_test/core/supervisor/test_console_watch.cpp`, printed into
`out/experiments/p2-05-counts-and-messages.log`), which is why the times are small and the uptime
is a fixture's. Note what rung 4 did **not** do: the chip held the golden image, so nothing was
written and the day's one flash was not spent.

## The console

`docs/console.md`, *The supervisor*.

| Line | What it does |
|---|---|
| `rak watch` | the view above. **It only reads** — looking at the supervisor can never reset a radio |
| `rak watch reset-caps` | the day's counts to zero and a stop lifted, journalled. For the bench |
| `rak ladder` | climb now: ask the radio, and if it is silent work out why and climb, printing each rung as it is tried. Minutes — the waits are real. A healthy radio is said to be healthy and nothing is climbed; a stop is lifted first, because a person is the other way out of rung 5 |

## What a person should do when it has stopped

`rak watch` says which rungs were tried and why it stopped. Then:

- **the chip holds the golden image** — the firmware is intact and the radio still will not answer.
  This is the hardware fault `docs/ARCHITECTURE.md` §8 and `spec/SYSTEM.md` §6 leave alone: a
  module, a connector, an antenna or the rail. Nothing the Walter can do will mend it.
- **the chip is protected** — the debug port is shut (an Fxx module whose firmware has not opened
  it, or a chip whose APPROTECT is engaged). Opening one is a bench procedure for a person
  (`docs/bench.md` §4); nothing in this firmware may erase a whole chip to get past it.
- **the chip could not be read at all** — the SWD lines, the bulkhead connector, or the rail.
  `rak swd connect` and `rak swd id` on the console are the next two things to try.
- **the golden image could not be put back** — the flasher failed; `rak update status` has the
  journal's account of it.
- **the battery is too low** — the unit needs sun, not a reflash. `rak power` when the radio is
  back, and the panel and the pack are the question.
- **today's cap is reached** — the supervisor has done this enough times today. `rak watch` lists
  the incidents; `rak watch reset-caps` and `rak ladder` are how a person overrides it.
- **the ladder was climbed and nothing worked** — the radio is unresponsive with its own firmware
  freshly on it. The same hardware fault as the first case, and the one the bench's §10 walks.

## What is not here

When the supervisor runs (phase 3's wake cycle), reporting to a server (phase 3's backend
interface, phase 4's agent), and the second radio of a dual adapter — the slot is carried through
every call, and a unit has one radio today (S-11).
