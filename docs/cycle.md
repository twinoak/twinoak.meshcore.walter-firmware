# One wake

Task p3-05; `docs/ARCHITECTURE.md` §10 for where it sits, `docs/power.md` for the band and the
interval, `docs/supervisor.md` for the ladder, `docs/update.md` for the two update machines.
`spec/SYSTEM.md` S-7 and §8 are the two decisions this page rests on.

Everything built in phases 0 to 3 exists and is tested alone. This is **the order those things run
in**, what happens when one of them fails, and the two guarantees that make a unit on a mast
predictable: *nothing is skipped silently* and *nothing runs twice*.

**A unit on a mast runs one pass and sleeps.** A boot that finds no USB host runs the wake cycle;
a boot that finds one stays awake with the console, because this chip cannot wake from deep sleep
on a cable and a person who has plugged one in is here on purpose. `cycle now` and `cycle dry` at
the console are how that person sees what the mast does by itself (`docs/console.md`).

## The ten steps

| | Step | What it does | Needs | Up to |
|---:|---|---|---|---:|
| 1 | `why` | the wake's reason: the timer, the radio's line with §8's attention behind it, a spurious wake, a person, a power-up, a crash, a watchdog (`core/cycle/wake_reason.h`) | — | 11.3 s |
| 2 | `radio` | the supervisor's check, and its **ladder** when the radio really is silent (`core/supervisor/supervisor.h`) | — | 40 s, or 6 min for a climb |
| 3 | `resume` | what the journal says was in progress: the radio's update continues here; this firmware's own trial is noted and runs its conditions through the steps below | `radio` | 20 min |
| 4 | `state` | `STATUS`, `POWER_GET`, `ENV_GET`; the pending attention acknowledged and its reason kept | `radio` | 15 s |
| 5 | `band` | the interval policy with the fresh reading: what the rest of this wake may do (`core/power/interval_policy.h`) | — | 1 s |
| 6 | `uplink` | the modem, inside this wake's budget; the network's time (`core/uplink/policy.h`) | `band` | the budget |
| 7 | `clock` | the radio's clock from the network's time, when it is unset, restored, or out by more than a minute (`spec/LINK-PROTOCOL.md` §5.4) | `state`, `uplink` | 10 s |
| 8 | `backend` | one exchange over the seam, and the settings it asked for (`interfaces/backend.h`) | `uplink` | 67 min 25 s + 1 min |
| 9 | `update` | **at most one update step**, of one MCU (S-7); a fetch; the ladder when one was asked for | `band` | 20 min |
| 10 | `sleep` | task p3-04's one way out: the journal, `SLEEP_NOTICE`, the modem down, the pads held, deep sleep | — | 15 s |

### The dependency table is data

`core/cycle/wake_cycle.h`'s `kSteps` is the table above, as C++, and a host test walks it
(`host_test/core/cycle/test_wake_cycle.cpp`). So "the clock was not set because the uplink did not
come up" is a row and not a comment, and a step whose dependency failed is **skipped** — a
different word from *failed*, and a different letter.

Three rows carry the design and a change has to come through them:

- **step 10 needs nothing and has no least**: every wake ends in sleep. A wake that cannot sleep
  is a battery the repeater needed.
- **step 5 needs nothing**: "no reading at all" is its own band (`no-reading`), because a Walter
  whose radio has died is exactly the one that has to reach its uplink and say so.
- **step 2 succeeds whatever it finds.** A scanner, a silent radio and a radio mid-update are
  *named states of the wake*, not failures of the step that found them out (§9's four silences).

## What a wake record looks like

One compact line per wake, in the console's log and — when the wake has news — in the journal:

```
wake 1204 timer: radio healthy, band normal, steps yy-yyyy-ys, 9 s awake, next 60 min
```

`steps` is one letter per step, in the order of the table:

| | Means |
|---|---|
| `y` | **done**: it ran and did what it was for |
| `-` | **nothing to do**: it ran and there was nothing (no update journalled, no ask, no build) |
| `s` | **skipped: a precondition** — a step it needs did not succeed, or the band said no |
| `t` | **no time: the wake's budget** — it was not begun, deliberately, and never half-run |
| `r` | **refused**: it was asked for and this firmware would not (the band, S-7, a secret key) |
| `x` | **failed**: it was attempted and did not work |
| `.` | **not reached**: the pass ended before it |

### The Walter's own attention

A wake that has something of its **own** to report - not the radio's §8 attention, which prints as
`, attention <reason>` - ends the line with `, agent <letters>`:

```
wake 1205 timer: radio healthy, band normal, steps yy-yyyy-ys, 11 s awake, next 60 min, agent u
```

| | Means |
|---|---|
| `u` | **the server refused this unit's token (http 401): set server.token.** The agent library halts on it and nothing more is sent until a person types a new one, so without this the only trace would be a log line the next deep sleep forgets |
| `h` | **the agent asked for a restart for its health and did not get one.** `mm::IPlatform::rebootForHealth`, which this firmware answers by journalling and not restarting: the supervisor and the wake cycle own restarts. In one-wake mode the library never asks, so this letter is a door that should stay shut |

The letters are task p4-04's (`core/cycle/agent_attention.h`). The byte they come from is one of
the two the duty cycle's retained record kept spare, so a raise **survives a sleep**: it is read
at step 10, which is why a 401 at step 8 is in this wake's own record, and the record that carried
it clears it - one raise is reported once, and a condition that is still there raises it again.
Neither letter is one a step's outcome uses, because one line carries both fields. `status` says
the same thing in a sentence, and a restart the agent asked for is a journal entry
of its own (`journal`: *agent, the agent asked for a restart for its health and did not get one*).

**Nothing of it touches the radio.** No frame is sent, no ladder is climbed and the radio is not
woken: it is a byte, a letter and a line.

### How long the board was away (task p10-01)

The same retained record carries one more thing: **the seconds the last sleep asked its timer
for**, written by `endingInSleep()` as the last thing before the chip goes down and read by the
boot on the other side. It is there because of what `interfaces/clock.h` says — `esp_timer`
restarts at zero after a deep sleep — and because the agent's library has to be told how long a
board was away, or a wait the server asked for counts for nothing across it.

`core::cycle::WakeReason::timeAwayMs()` is the answer, and the rule is **never more than really
passed**:

| This wake | What it was away |
|---|---|
| the timer | the seconds the sleep asked for, **less ten per cent** — the deep-sleep timer runs on the internal RC oscillator the ESP32-S3's `RTC_SLOW_CLK` defaults to, which ESP-IDF calibrates against the crystal at every boot and for which Espressif publishes no tolerance ("the frequency stability of this clock source is affected by temperature fluctuations", `docs/en/api-reference/system/system_time.rst`). `core/cycle/wake_reason.h` cites all of it beside the figure |
| anything else | **the radio's** (task p14-06, below). Until that task: **0**, because the sleep had been cut short and nothing here knew how much of it had passed |

A figure too small owes a sliver of a wait for one more wake; a figure too large calls a server
back before the wait it asked for. So the discount is deliberate and the 0 was deliberate.

**The record grew by four bytes for it**, and a retained record of another size in the same slot is
not the same record (`interfaces/retained.h`). So the first boot of 0.1.3 reads none: for that one
wake the wake log, the band's hysteresis, the spurious count and the faults in a row all start
again, exactly as after a power cut, and the interval is the `unknown` band's hour. `wake` says
*the record did not survive*, and the wake after it is normal.

### The radio's uptime measures the other wakes (task p14-06)

The 0 above cost a unit an evening. On 13 November 2026 Thomas pressed *Wake it up* on
meshmaster.dk (job 28399); the unit's radio answered `ok` at 18:09:02 UTC and raised §8's
attention, reason 4; the board woke on the line — and sent nothing, because the wake before it had
ended in a failed call and a failed call leaves the agent's library a wait. The wait was owed
whole, because the time away was 0 for every wake but the timer's. Nothing came from the unit
until its own timer woke it.

**The radio never sleeps and counts its own seconds.** So its `uptime_s` (§5.2's `PING`) is the
one clock that runs through a Walter's sleep whatever ends it:

- the board reads it **after the agent's state has been saved and before `SLEEP_NOTICE`**, and
  writes it down with the radio's `boot_id` in the duty-cycle retained record;
- at a boot that is not a timer wake and has a reading, `core/cycle/duty_cycle.h`'s `begin()`
  sends one `HELLO` and one `PING` — **before the agent's state is restored**, which is why it is
  there and not at step 4 — and the two `uptime_s` are the two ends of the sleep;
- the span is believed **less one second** for the whole seconds at either end and **less
  500 ppm** for the radio's own clock, rounded up. 500 ppm is the nRF52840's LFRC in Nordic's
  product specification (*Clock control*: "32.768 kHz +/-500 ppm RC oscillator"), and the
  crystal is what a design fits "when better than +/- 500 ppm accuracy is required" — so it
  bounds whichever source the RAK4631 has, and this end does not have to know which.

**It is still 0** when there is no reading (a power cut, a first boot, a wake that never reached
its sleep, a radio that did not answer the sleep), when the radio answers nothing at the boot,
when its `boot_id` is not the one written down — it restarted too, so its seconds are not the
same seconds — or when its `uptime_s` is below the one written down. And **the reading belongs to
the state it was taken after**: the agent's `saveState()` voids it, because a wait is counted from
the moment the state was taken and a reading from before that would measure too much.

A timer wake keeps the timer's figure and asks the radio nothing, which is nearly every wake this
unit has. `agent state` says which of the three measured this boot, and `wake`'s list says it for
each wake that sent nothing for a wait.

**The record grew by eight bytes for it** — the `boot_id`, the `uptime_s` and the byte that says
whether there is a reading — so it is **128 bytes, which is `kRetainedRecordMax` exactly**. The
first boot of the release that carries it reads no record, for the reason the paragraph above
gives; and the next thing that has to cross a sleep needs a slot of its own.

### A wake a person asked for owes no wait (task p14-06)

The other half of that evening. §8's attention reason 4 is *a person asked for the Walter*, and a
wake that carries it **does not put the saved wait back at all** — whether the event woke the
board or step 4 found the reason still pending in `STATUS` (§8: "a pending attention stays visible
in `STATUS` until the Walter has made a request"). Somebody is at a screen waiting to hear from
the unit; a failed call of the wake before is not an answer to them.

It is the line task p14-05 drew for a trial boot, drawn again. Nothing else of the library's state
moves: the backoff, the run of failures, the halt on a 401, the counters and the accepted interval
are handed back as they were saved — so a unit that cannot reach its server still backs off, it
simply answers the person who asked. **The bound on how often is the radio's** (§8: at most one
new attention per reason per hour); this board adds none, because a second limit here would only
make a person press the button twice.

### What a wake did, and who was reached (task p10-02)

The seam's `Exchanged` has three values and they answered two questions at once, so *nothing to
do* meant both "it talked and there was nothing waiting" and "it never opened its mouth". The
journal read the first of those into the second and said **the backend was reached** about wakes
that sent nothing, which is what the morning of 8 November 2026 was read wrong from. The three
answers this firmware's words now tell apart are:

| | |
|---|---|
| **answered** | a heartbeat went out and the backend answered it. The only thing the journal says *the backend was reached* about, and the only thing `interfaces::wasAnswered()` is true for |
| **sent nothing** | the exchange worked and nothing went over it, **with the reason beside it** |
| **failed** | the exchange did not work. *the exchange failed* is the step's own outcome and the whole of the news; nothing is claimed about who was reached |

The reason is `interfaces::Reached`, a field of the answer rather than a fourth `Exchanged`: one
value more could not carry *why*, and the why is the point — and `Exchanged`'s three values are
what the wake cycle's rails turn on (S-7's one update step, the budget, a sleep a wait lengthens:
all of them test for *failed* and nothing else).

| | Means |
|---|---|
| `the backend answered` | a heartbeat came back |
| `a wait still owed` | a backoff or a `Retry-After` the agent's loop is still inside. The seconds left go with it |
| `no uplink` | there was no uplink to send it over |
| `the uplink's backoff` | the modem was not brought up at all this wake, because the uplink's own backoff said not to (`core/uplink/policy.h`, `mayTry()`). **Written by the wake cycle, never by the seam** — step 8 needs step 6, so a wake the backoff skipped reaches no backend to answer for it (task p13-02) |
| `not configured` | this unit has no server to send it to — **what the null backend of this repository answers, every wake** |
| `halted on a 401` | the server refused this unit's token and a person has to act (`server.token`) |
| `nothing to send` | nothing was sent for a reason this seam has no word for |
| `it did not say` | the implementation said nothing, and **nothing may be claimed**: it is the zero of the field, so a backend that is silent about it is never credited with having reached anybody |

A wake that sent nothing ends its record line with `, nothing sent: <reason>` — *answered* and
*it did not say* print no clause, the first because it is the ordinary wake and the second because
a step that never ran has nothing to explain.

**Who answers, and when** (task p13-02). Step 8 is the seam's own word and is the last word. But
step 8 *needs* step 6, so a wake whose modem never came up never reaches a backend at all, and
until this task such a wake left `it did not say` exactly where a person was looking for a reason —
which was the whole of what 13 November's wake request left behind. **Step 6 answers for those
wakes**: `the uplink's backoff` when the backoff skipped the modem, `no uplink` for every other way
step 6 did not succeed (the band does not allow it, `saving`'s every-fourth-wake counter, the
sequence failed). One reading, written in one place, read by the record line, the journal entry and
`wake`'s list.

**What `wake` says.** The last-ten list used to give each wake's length and nothing else, and on
7 November 2026 three wakes of 40 s and four of 6 s were read first as wakes that skipped the
modem and then as a failed connection — `lte` said both readings were wrong. So each wake in that
list now carries three letters:

| | Means |
|---|---|
| `M` | the uplink came up (step 6 succeeded) |
| `S` | the server answered (step 8, and only an answered heartbeat) |
| `J` | a job of the backend's own ran inside the exchange |
| `-` | that one did not happen, in a wake that ran to its end and wrote down what it did |
| `?` | **not recorded**: a wake ended by a reset, a crash or the bound before step 10, or one written by a firmware older than this |

The three bits live in each entry of the duty cycle's retained record, beside the reason and the
duration, and each is written the moment it becomes true rather than at the end — so a wake that
does not come back still leaves what it had managed. **The record's size did not change for
them**: the two bytes they came from are the top half of a `lasted_s` that no wake ever filled (a
wake is bounded at five minutes, and `stay` holds one open for twelve hours at the most), so an
entry written by the build before this reads back with the right length and no claim about what it
did.

**And why a wake sent nothing** (task p13-02). `---` said that nothing was sent and stopped there,
and the one wake a person cares most about — the radio's line, inside a wait — left no other trace
anywhere: only a *timer* wake inside a wait is news for the journal. So each entry of the list now
ends with `; nothing sent: <reason>` in the same words the table above gives, and with the seconds
when the reason is a wait:

```
   2. attention  --- attention 5 (obs), detail 12; awake 12 s; nothing sent: a wait still owed, 30 s (the time away: the radio)
   3. timer      --- ; awake 6 s; nothing sent: the uplink's backoff
```

Since task p14-06 a wait that outlived a sleep also says **what measured that sleep** — *the
timer*, *the radio* or *nothing measured it* — because that is the first question a wait which
survived a quarter of an hour raises. It is two bits of the `did` byte, which had four free, and
neither of them is written with the `kDidSaid` bit: a wake nobody wrote down still reads `???`
for the three letters.

That is three bytes an entry — the reason and a 16-bit count of seconds — and the record **did**
have to grow for them: 100 bytes became 120, inside the 128 a retained slot holds. A record of
another size in the same slot is not the same record (`interfaces/retained.h`), so the first boot
of the release that carries this reads none: the wake log, the band's hysteresis, the spurious
count and the faults in a row all start again for that one wake, exactly as after a power cut, as
they did at task p10-01's growth. That is the rule working and not failing — the old bytes meant
something else.

### What is journalled, and what is not

**Not every wake.** The journal is 1024 entries of 64 bytes in one 64 KB partition and it exists
so that an *update* is never guessed at (`core/update/journal.h`: "it wraps without ever losing the
entries of the update in progress"). A unit checking in every quarter of an hour writes 96 records
a day, which would wrap the whole log in ten days and push every update and every incident out of
it — and would erase a sector every sixteen hours for the life of the unit. Task p3-04 refused to
journal every *sleep* with the same arithmetic.

So a wake with nothing to say is in the console's log and in retained storage; the journal gets a
line when the wake has **news**: a step failed, was refused or ran out of time; the band changed;
the ladder was climbed; this firmware was on trial; the radio's clock was set; an exchange
happened; an update step or a fetch happened; the wake was a crash or a watchdog; **or the wake
came round on its timer and still owed a wait**. After a week on a mast `journal` is then a page
of *what happened*, which is what the task asks it to be.

**Why a wait owed is news and the other silences are not** (task p10-02). Since task p10-01 a
sleep takes its own length off a wait and the sleep is lengthened to cover one the seam carried
out, so a *timer* wake that still owes means a sleep did not cover its hold — the failure of the
night of 8 November 2026, and it must leave a trace. It costs at most one entry per failed server
call: a hold under ninety per cent of the interval is spent whole by one sleep, and a longer one
costs a second wake and is then spent too. The very worst a night of them could cost is one entry
a wake — 48 at the quarter-hour interval, so even a night of nothing but these would take three
weeks to wrap the log. The others are left out each for its own reason: *not configured* is every
wake of an unprovisioned unit for ever (the log wrapped in ten days), *no uplink* is already news
through step 6's failure, *halted on a 401* raises the Walter's own attention which the record
carries, and a wake on the radio's line inside a hold is expected (p10-01 claims no part of such a
sleep on purpose).

The entry carries the reason and the seconds still owed — five bytes, which **cost nothing**: a
journal slot is 64 bytes whatever the payload holds. A payload of the older, 32-byte shape is
still decoded, and answers *it did not say* for the two fields it never held: a unit's journal is
the log of the firmware that wrote it, and an update may not blind it.

The record is written **before** the sleep, because a sleep does not return. A sleep that was then
refused writes its own entry after it (`core/cycle/sleep.h`), and the two together are the story.

### What the server is told about what woke this unit (task p13-02)

On 13 November 2026 a wake request reached a unit's radio, the radio answered *ok*, and the unit
did not check in. From meshmaster.dk there was no way to tell apart **the line never moved**, **the
line moved and nothing valid was on it** and **the unit woke and sent nothing** — and those three
are a radio fault, a wiring fault and a firmware fault. Everything above is in RTC memory, which
holds ten wakes and which a power cut takes.

So four running totals go out with every heartbeat - and a fifth since task p15-06 - in its
`errors` object, beside the library's own six and the `radioIdentityLost` of task p10-03:

| | |
|---|---|
| `wakesTimer` | the check-in: the sleep timer came round |
| `wakesRadio` | the radio's line, with a valid `EV_ATTENTION` inside §8's window — a wake somebody asked for |
| `wakesSpurious` | the radio's line, and nothing valid inside it |
| `wakesSilent` | a wake that **sent nothing**, whatever woke it: the reason it settled on is one of the *nothing was sent* ones above. *the backend answered* is not one, and neither is *it did not say* — which since this task means the exchange failed, and a wake that tried is not a silent one |
| `wakesMidway` | a wake that **did not reach sleep**: the bound ended it, it crashed, or the chip's own watchdog took it (task p15-06, below) |

A wake that is none of the first three — a person's cable, a power-on, a reset, a crash, a
watchdog — moves none of the first three: they count the three a server cannot see, and the
heartbeat's own record carries the total of all wakes; `wakesMidway` is about the wake *before*
and is counted whatever woke this one. The first three are counted at step 1, the fourth at step
10, the fifth at step 1 of the boot after the wake it is about, and they are kept in **one value
of the settings store**, `agent_wakes`, as hex (`core/cycle/wake_counters.h`) — the shape
`agent_state` has used since task p4-05, and for the same reason: RTC memory has no slot to spare
and a power cut takes it, and *how many times has this unit woken on its radio's line* is exactly
the question a person asks after the power came back.

The value is **version 2** since task p15-06 and the four of version 1 are still read, so a unit
updated into that release keeps months of counting and starts the fifth at 0; the older shape is
rewritten in the new one at the first save.

**What it costs the flash**: one value of the store a wake, 42 characters, **and only when one of
them moved**. meshmaster.dk's bound on the whole `errors` object is 4 000 characters of JSON
(`AgentLimits.Json`); twelve counters are a little over three hundred.

### What a wake was doing when it was ended (task p15-06)

In the night of 17 November 2026 this unit took the hourly `status` job of its own repeater at
01:29 UTC and again at 02:05. Both times it sent its heartbeat, claimed the job, and was never
heard again in that wake: five minutes later the bound on a wake ended it by reset, the next boot
read that as a watchdog, and the unit slept half an hour instead of a quarter. The job expired
unanswered and nothing was measured for over two hours. **What those two wakes were doing for five
minutes, nobody could say**: the journal had *woke by watchdog*, the list of ten had *it never
reached sleep*, and the log, the claimed job and the letters of the wake were in RAM.

So **one line, in RTC memory, written as the wake goes** (`core/cycle/wake_progress.h`): where
this wake is. It is not a log and not a trace — it is one record, overwritten, so that the one
wake that matters is the one that left it.

| | |
|---|---|
| the step | of the ten, written by the wake cycle as each is begun — so a wake ended between two steps names the one it had reached |
| what is out | a call to the server, by the name `agent calls` prints for it (`heartbeat`, `claim`, `observations`, `a result`, `a lease`, `progress`, `config`, `a download range`) |
| a job in hand | that a claimed job is being run and of which type, and **not** cleared by a call: a result posted inside a job still reads as that job |
| its requests | how many requests of the radio or the mesh the job in hand has made (`agent::LinkMeshCounters::requests`), saturating at 255 |
| waiting | that the modem or the link is being waited on, when this layer knows it |
| the seconds | the second of the wake it began at, and the second the record was last written in — so *for how long* has an answer |

Where each comes from, and all of it **without a line of the copied library changing**: the step
is `core/cycle/wake_cycle.cpp`'s `begin(Step)`; the call is `ModemUplink::call()`, the one place
every call to the server passes; the job and its requests are `mm::JobRunner::openJob()` and the
link's own counter, read by `AgentBackend::noteProgress()` from the **pump** — the function the
library runs at every idle wait, which this board wraps; and the link's wait is `LinkMesh::await()`.

**The boot after says it**, at step 1, where the wake's reason is already settled:

```
journal
   3. seq 1: the wake before did not reach sleep (watchdog)
       it was in: step 8 (backend), a `status` job, 1 request(s), 297 s in it
wake
   2. timer     ???; it never reached sleep (a reset, a crash or the bound)
       it was in: step 8 (backend), a `status` job, 1 request(s), 297 s in it
```

The phrase is on a line of its own because a journal line's or a list entry's own prefix and the
phrase together do not fit a 100-column terminal, and every line this firmware prints does. The
report's `wakes` section is `wake`, so it carries the line too, which is how a unit on a mast is
read.

**A wake that reached no step leaves nothing.** A boot with a USB host on the port never runs the
wake cycle — a person is here and the console is the wake — and a bench session whose cable comes
out is ended by the bound by design. Those have no step in the record, so they get no line, no
journal entry and nothing counted; what the boot log says about them is `restarts`' business.

**Where the 24 bytes came from.** RTC SLOW memory is 8 192 bytes and the pinned modem component's
own `.rtc.data` is 6 996 of them, so a ninth retained slot of the usual 128 would not fit in what
was left. `interfaces/retained.h` gives each slot its own room since this task: the first seven
are 128 and byte for byte where they were — so the records of a unit in service survive — the
wake cycle's telemetry slot gives back the twelve bytes its record never used, and this one is 24.
The cost is one wake of the telemetry ring's readings at the first boot of the release, which is
the price every change of a retained record's shape has paid.

### The fifth total: wakes that did not reach sleep (task p15-06)

`wakesMidway` goes out with the four above, in the same `errors` object and at every heartbeat.
It is counted by the **boot after** — a wake ended by a reset counts nothing — from the one thing
that can tell it from a power cut, the record of where that wake was. `cycle now` at the console
is a second pass of the same boot and counts it once.

## The budget, and the things that may hold a wake open

`kWakeBudgetMs` is **two and a half minutes** and is what a wake may spend in all. Each step has
its own bound and its own *least*: a step is begun only when what is left of the budget is at
least its least, and its deadline is the sooner of its bound and what is left. A step that cannot
be begun is recorded as `t` — *no time* — with a reason a person can read.

The arithmetic: the uplink's registration wait is two minutes of it
(`core/uplink/policy.h`, `kRegistrationDeadlineMs` — the first number the bench changes), the rest
of a wake is seconds, and the **bound on a wake** (`core/cycle/wake_guard.h`) is five minutes — so
there is twice the budget of margin before the rail that resets a wake, which is what one
worst-case modem call that comes back late costs.

A few things are allowed to hold a wake open past the budget, each up to its own bound, and every
one of them **raises the bound on the wake** rather than removing it:

| | Why | Up to |
|---|---|---:|
| a climb | the ladder waits 30, 60 and 120 s between rungs, and **the state of a climb does not survive a sleep** — only its counts and a stop do. So a climb runs to its end inside its wake or starts again at rung 1 in the next one, and the caps are what keep that from becoming a loop | 6 min |
| an update **resumed from the journal** (steps 3 and 9) | "a flash may take several wakes' worth of time and is allowed to hold the wake open up to its own bound" (the task). A flash, a 20 s boot window and a three-minute proving period do not fit in five minutes | 20 min |
| an **apply** claimed from the server | the same machine, run to its end inside step 8 — and that end may be a **rollback**, which is the flash and the proof twice. The unit's own journal says a flash is 14 min 51 s, so a whole run is `core::update::kWholeRunMs` (task p14-04; 46 min 10 s since task p18-04 gave each pass the second boot window of a unit whose reset line does not reach its radio and task p18-05 the one second try at a flash that wrote nothing). At the thirty minutes this used to be, the bound fell in the middle of the second flash: no result could be posted and the lease was left to lapse | 46 min 10 s |
| a **session** of the radio as a scanner **taken up out of the journal** (step 3) | a session is only ever *begun* by a person or a job, and is **ended by the next wake** whatever ended the last one — a restart, a crash, this bound, a power cut (`docs/update.md`, *A session*). So step 3 may have the golden image's whole way back in hand, and, where the session was still in its steps, the twenty-minute ceiling before it. It is the number that set `kRaisedBoundMax`, and since task p18-04 it carries the wait for the watchdog a scanner leaves running - and since task p18-05 the one second try at a flash that wrote nothing, in each of its two passes | 67 min 25 s |
| a trial | this firmware proving itself (S-6) has to finish before the wake may sleep: a deep sleep during a trial is a rollback of the image being tried. Its loop also asks the backend seam for **one more heartbeat and nothing else** while the third condition is open (task p14-05, `interfaces::Beat`): step 8's one exchange is not enough to condemn an image on, and the implementation's own wait is what paces it | 6 min |
| a backend exchange | step 8's `exchange()` is given the longest machine's deadline, because a `twinoak.fw.*` job (task p4-05, §13) runs one of the update machines above *inside* it, and a `config.read` of a neighbour is eleven minutes of airtime-budgeted transactions. It is a minute longer than the longest of them, for the heartbeat and the claim that come first — and it is not only the guard's business: the library is given what is left to this deadline as its wake's **budget**, and a result made after that budget has run out is kept rather than posted (task p14-04). Since task p16-04 the longest is a **session** and not an apply, so the number is the session's and a minute: a real session is two flashes of a quarter of an hour, and a result kept for the next wake on a job leased an hour from the claim is a job the server fails while the radio is perfectly well. **And since task p16-06 that deadline is a ceiling and not a promise.** It was the better part of an hour while the bound on the wake was five minutes, so an exchange in a wake of the cycle's own was promised thirty-nine minutes and reset at five - which is what the two wakes of 17 November 2026 were. The two are worked out in one place now (`core/cycle/wake_room.h`): the deadline is this number or the most a wake may ever be raised to, whichever is sooner; **every call takes what is left of the sooner of it and the bound**, so no call is begun that the wake has no room to finish and the wake is never reset in the middle of one; and **the claim raises the bound to cover the leases it took on** - the latest of them, and a call for each result and the observations, never past the ceiling. A wake that claims nothing still keeps its five minutes | 68 min 25 s |

The time those take is **outside** the budget, because both machines are already bounded and
capped by their own rules — and because a wake that spent six minutes on a ladder must still be
able to reach its uplink and say so.

A raised bound is still a bound. A wake that overruns even the raised one is ended by a reset, the
journal carries the unfinished work, and `resume()` takes it up again at the next boot. That is
the whole reason the reset is safe (`docs/update.md`).

### The bound covers what the wake took on (task p16-06)

In the night of 17 November 2026 this unit took the hourly `status` job of its own repeater at
01:29:53 UTC and was next heard at 01:34:56, as a new boot; the same at 02:05:24 and 02:10:28. The
job was leased four minutes, was taken at both wakes and answered at neither, and each reset was
read as a watchdog and followed by half an hour of sleep: over two hours with nothing measured, and
a job meshmaster.dk failed for want of an answer.

What those wakes were in is not known — task p15-06 is how the next one says. What is known is the
arithmetic: an exchange was promised the longest machine's deadline and the wake it was in was
bounded at five minutes. A job leased four fits inside that only when everything else in the wake
takes under one — the radio's question, the modem's attach, the heartbeat, the claim, a result's
call, and an observations call that stalls for the modem's thirty-second command timeout at nearly
every wake.

**One figure, in one place** (`core/cycle/wake_room.h`):

| | |
|---|---|
| the exchange's deadline | its own number (`kBackendBoundMs`), or the most a wake may ever be raised to (`kRaisedBoundMax`) if that is sooner. A **ceiling**: the library's wake budget comes off it, so a result a long job made is posted rather than kept |
| every call | what is left of the **sooner** of that deadline and the bound as it stands. Too little is no call, by the rule a call already had for the deadline (`kLeastCallMs`), and the transport is given the same figure — so a call is bounded by the wake it is in |
| a claim | the bound raised to the end of the **latest lease** among the jobs it took, and the calls that come after them: a result for each job and the observations, each at the modem's command timeout. Never past the ceiling. A wake that claims nothing keeps its five minutes |
| a lease the ceiling cannot cover | **cut**, to where the room for those calls begins. The copied library then ends the job `timeout` by its own rule and the result is posted, which is what the server is owed |

Nothing else moved. An apply still asks for `core::update::kWholeRunMs`, a session for
`update::kSessionBoundMs`, a climb for the ladder's own, and the console's `agent wake` raises the
bound for the length of the command and puts it back; `WakeGuard::allow()` only ever raises, so a
claim made inside one of those leaves it where it was. And the rail is untouched: a wake that hangs
past its bound, raised or not, is still ended by a reset, and the sleep after a reset is what it
was.

**What a wake says about it.** The wake's record and `wake`'s own lines carry *the bound was raised
to N s for 2 jobs* and, where one happened, *1 call(s) were not begun: the bound had no room to
finish one*. A call with no room is a reason for a journal entry; a bound that was merely raised is
not, because an ordinary claim of a leased job raises it at every wake that has a job.

### A wake claims until the battery says sleep (task p18-02)

A wake used to make **one** claim and run the two jobs it was handed, so a NoiseScope sample of six
steps and a report took five wakes — which is what 21 November 2026 at Bellevue cost. The rule for
this board is that it takes jobs until none are left or the battery says sleep, and the battery is
what says how many times it may ask:

| band | claims a wake may make | transactions it promises |
|---|---|---|
| `plenty` | 8 | `2 × the dearest job` |
| `normal` | 4 | half of it |
| `saving` | 1 | one real job's worth |
| no reading | 4 | the medium one |
| `critical` | none — the modem is not brought up at all | none |

**Both numbers are placeholders** and the bench sets them (`docs/bench.md` §17,
`components/agent/include/walter_agent/identity.h`): a worse band never promises more, which a
`static_assert` holds. The claim count is **not** sent to the server — it is this end's own bound —
and what the server is told is what the wake has left in transactions (the claim's
`transactionsLeft`) and nothing about how many times it may ask.

**The bound covers every claim, not only the first.** `coverTheLeases` runs at each one and raises
the bound from where the wake stands then, so a wake of three claims is three leases covered and is
never ended by the reset the bound exists for. And **a claim is not made at all** where the wake
could no longer be raised far enough to hold a job's lease and the calls after it
(`kRaisedBoundMax`, `WakeRoom::couldHold`): the wake ends its jobs there, which is *no room* and
not a failure — nothing goes out, nothing backs off, and the work is still the server's.

**Why the last claim was the last**, which is the one thing no number says. Seven words
(`interfaces::Claims`), in the journal's own line, in `agent wake` and in the `jobs` section of a
`twinoak.report`:

| | |
|---|---|
| `nothing waiting` | the server said nothing more waits — the ordinary end of a wake that did its work |
| `the band's number` | the battery's claims for this band are spent |
| `no room` | the wake could not be raised to cover another claim, its seconds at the server are gone, or a job was left to its lease for want of budget: a wake that could not pay for what it had does not take on more |
| `no transactions` | what is left of the band's airtime cap cannot hold a job at the cheapest price there is |
| `a result kept` | a result of the last claim did not fit the bytes a wake may post and is kept for the next one |
| `the radio is away` | a job took the radio away — an apply, a session, a power cycle — and it is not back by the supervisor's word |
| `a claim failed` | the claim did not come back, and the server had spoken |
| `it did not say` | no claim was made at all, and then the claim step's own outcome is the answer |

**And what this firmware holds is named in the claim.** meshmaster.dk hands a claim that states
`holding` every job it has leased to this exit node whose lease is still running, that has no
result, and whose id the claim did not name — it keeps no record of an answer that did not arrive.
Three kinds of work of this unit's are exactly that and are in no buffer of the library's: a job
the result gate left to its lease, a session the bound cut, and an apply whose result waits for the
next boot. Their ids go into `holding` behind the library's own, so no download is paid for twice
and no flash of a radio is begun twice; the lease then lapses and the server's own rules take the
job from there.

**What this does not do.** A job the radio never answers ends `unreachable` and not `timeout`: the
library gives a login two tries and the job is over in half a minute (`agent/LoginSequence.h`).
`timeout` is its word for a job the **lease** ended, which is the cut lease above and a job that
cannot be finished inside the lease it was given. Both are a result the server is told of, which is
what the night of 17 November never got.

## Nothing runs twice

Each step runs once per pass, in order, and `run()` is not re-entrant. **At most one update step
happens in a wake, of one MCU** (S-7): when both are asked for, the Walter's own goes first —
`docs/ARCHITECTURE.md` §7 has it lead a link-version change, and its own update ends in a restart
anyway — and the radio's is refused with `one update a wake, and the other one went`, recorded,
and told at the next exchange. The two machines already refuse each other through the journal;
this is the third lock on the same door, and the one that reports it.

**And a wake that ended a session runs no update step** (task p16-03). Step 3 takes up an open
session of the radio as a scanner — the scanner image is on, the golden image is not back — flashes
the golden image back and proves it, and that counts as this wake's one update step: step 9 then
refuses the other with `one update a wake, and the other one went`, exactly as it refuses the
second of the two MCUs. A session is never *begun* in a wake by this firmware; the two things that
begin one are a typed `rak session` and the `twinoak.noisescope.session` job. **A session the job
began counts the same way** (task p16-04): it is two passes of the radio's update machine, so the
exchange reports it back as the `Ask` the wake carried out — `update the radio` — and step 9
refuses anything else.

**Asks do not cross a sleep.** A refused ask is remembered and reported; the ask itself is not.
A backend that still wants something asks again at the next wake, which is one exchange later and
costs nothing, and a firmware that remembered an ask across a reset would be a firmware that could
be made to do something nobody asked for twice.

## The sleep, and when it is shortened

The interval is step 5's (`docs/power.md`). It is **shortened to five minutes when a download is
half done and the band is `plenty`**, so that a release arrives in a morning rather than in a day —
and **never in `saving` or `critical`**, where the wakes are what the pack can pay for.

*Half done* is **either downloadable slot** — the candidate's or the scanner image's, which an
offer with `into: scanner` fills by the same ranges — answering **a transfer at X of Y with X
neither 0 nor Y**: the store's own progress markers, and only where no record has ever stood over
them (`ARCHITECTURE.md` §6). The golden image is never downloaded into and the base is `rak
provision`'s, so neither is asked about. Until task p15-01 a committed radio update left those markers readable in
the area the old golden image had been in, so a unit shortened **every** sleep from its first
update on: 15 minutes became 5 minutes 40 seconds, and three wakes a quarter-hour where there
should have been one. The journal's wake line says `the sleep was shortened` when it happens, and
a unit with no download under way must not be saying it.

A sleep is refused when the radio's update has a step in the air, when a transfer into the other
application slot is open, or when this firmware is on trial. The wake then ends in
`the sleep was refused`, the bound on the wake is what ends it, and the next boot resumes.

---

# The page for the implementer of `Backend`

This is the seam to the agent work (`spec/SYSTEM.md` §8, `spec/proposals/01` and `03`; the Walter's
phase 4). **Nothing in `interfaces/backend.h` names MeshMaster, a URL path, JSON or a job**: the
agent lives entirely above the seam, so the wake cycle knows only a plain structure in and a closed
set of requests back.

Two implementations fill it. `agent::AgentBackend` (`components/agent/agent_backend.h`, task p4-05,
§13) is the real one — one call to `mm::AgentLoop::wake()`, the MeshMaster agent behind it, built
when `WALTER_AGENT` is on (the default). `core/cycle/null_backend.h` is the other: it does nothing
and answers at once, and is what a `WALTER_AGENT=0` build runs — the unit of phases 0–3, the wake
cycle over a seam that asks for nothing. Both are tested; `tools/verify.sh` runs the wake cycle's
own tests in each. This page is the contract both honour, so that the cycle never has to know which
one it is talking to.

## What you implement

One method:

```
Exchanged exchange(const BackendKnowledge& known, std::uint64_t deadline_ms, BackendAnswer* out)
```

- `known` is a plain structure this firmware has already filled. No pointers, no heap, nothing to
  free: copy out of it what you need.
- `deadline_ms` is on the same monotonic millisecond clock `core/` uses (`interfaces/clock.h`),
  and it is **the wake's budget, not a timeout you picked**. A backend that sits past it is a unit
  that does not wake again.
- the answer is `asked something`, `nothing to do`, or `the exchange failed`. *Failed* is safe:
  nothing is assumed, the readings stay where they are, and the refusals this unit is carrying are
  still carried.

## What you may rely on

- **You are called once a wake, at step 8, and only when the uplink came up.** If the band does not
  allow the modem, or the backoff says not this wake, you are not called at all — and nothing is
  queued for you.
- **The structure is complete as of this wake**: both firmwares with their descriptors, the
  unit's identity as the radio gave it (the repeater's key is the unit's key, S-1), the three slots
  of the image store, the radio's `STATUS`, the supervisor's last three incidents, the readings
  kept since anybody last took them with the count of what was dropped, this wake's record, and
  the uplink's signal.
- **A reading whose `time_known` is false was taken before this unit knew what day it was.** Its
  order in the array is the only ordering there is. Nothing is guessed at.
- **The readings are emptied only when you say you took them** (`took_readings`). An exchange that
  failed, or one whose answer does not say so, leaves every reading where it was.
- **Refusals reach you.** "I did not do what you asked, because" is the one thing you cannot work
  out for yourself, so it survives the sleep and is in `known.refusal[...]` at the next exchange
  that works.

## What you may ask for, and nothing else

| | What this firmware then does |
|---|---|
| `fetch a release` | `base_url` + `manifest.json`, the signature, the six checks, the image into the candidate (or scanner) slot in resumable ranges (`core/update/fetch.h`) |
| `update the radio` | the stored candidate flashed, health-checked, kept or rolled back (`core/update/radio_update.h`) |
| `update the walter` | the image in the other application slot made the next to boot, with one boot to prove itself (`core/update/self_update.h`) |
| `change a setting` | by the name a person would type, **non-secret keys only** (`core/settings/keys.h`) |
| `climb the ladder` | the supervisor's ladder, now (`core/supervisor/supervisor.h`) |
| `nothing` | — |

A request outside that set **cannot be expressed**, which is the point: you cannot make this
firmware do something it has no tested procedure for. You may ask for more than one; the wake
cycle decides the order and what fits.

## What it may refuse, and why

| | Means |
|---|---|
| `the battery's band does not allow it` | `core/power/interval_policy.h`: below the floor there is no update, no flash and no modem |
| `one update a wake, and the other one went` | S-7 |
| `an update of the other MCU is unconfirmed` | S-7, through the journal |
| `a secret is never set from here` | a SIM's PIN and a server's token are a person's, at the console, over a cable |
| `no such setting` | including every threshold that protects the hardware: they are build constants and not keys |
| `the value is not one that key takes` | the key's own row in `core/settings/keys.h` |
| `this build cannot do it` | no image store, no journal, no supervisor |
| `the wake's budget ran out` | it was not begun |
| `it was tried and failed` | the journal says what happened |
| `not an https base url, or too long` | `https://` only, 128 characters |

## What the seam does not carry, and where the agent puts it instead

The seam is deliberately narrow: a job, a login, a lease, a result envelope, a progress report,
`nextCheckInS` — none of them is in `interfaces/backend.h`. That was never a gap to be filled *at*
the seam; it is the line between what the wake cycle owns and what the agent owns. Task p4-05 built
the agent on the other side of that line (§13), so each of these now lives **above** the seam:

- **The mesh.** The job, the login, the CLI line over the air, the observation pull are
  `agent::LinkMesh` (task p4-02) over the phase-1 link client, which the library drives — not the
  wake cycle. A job for the unit's *own* repeater goes through the local door and sends nothing on
  the air. Rail R14 still holds that the wake cycle itself calls no mesh request.
- **Job identity.** The id, the lease, the result envelope and the extend are the library's and
  meshmaster.dk's, carried over `agent::ModemUplink` (task p4-03). When a firmware job runs one of
  this firmware's update machines, the exchange reports it back as the `Ask` the wake *carried out
  itself* (`BackendAnswer::did`/`carried`), so S-7 and the journal see it like any other update.
- **Which slot an offer fills.** `twinoak.fw.offer`'s payload carries `into` (task p16-02):
  left out or `candidate` it is the offer it always was, and `scanner` sends the same fetch into
  the **scanner slot**, where the manifest's image whose role is *scanner* is the one chosen for
  the board and the band. Everything else is unchanged — the manifest and its signature, the six
  checks, the ranges, the progress, the lease, and a download taken up again at the next wake.
  A word that is neither, and `scanner` for the Walter's own product, are answered
  `stored: false` with a reason and nothing is fetched. A slot that already holds the offered
  file, verified, is answered `stored` without a single range asked for: the hash in the payload
  and the store's own record settle it, and a few hundred kilobytes over a metered link are
  spared. `fetch [base-url] [candidate|scanner]` is the same thing at the console
  ([`console.md`](console.md)), for a unit somebody can reach with a cable.
- **A session of the radio as a scanner.** `twinoak.noisescope.session` (task p16-04) is the sixth
  type the extension answers, and it runs `core/update/scanner_session.h`'s machine *inside* the
  exchange as an apply runs its own: the program comes out of the payload, each step's lines are
  posted as the step hands them over (`POST /jobs/{id}/lines/{from}`, bodies of at most 8 192
  bytes cut at a line's end), the journal goes out as progress a line at a time, and the result
  says `restored`, `refused` or `stuck`. The seam sees it as the one ask it has words for —
  `update the radio`, because that is what two passes of the radio's update machine are — and
  S-7's one-update-step rule holds it to one a wake. A board that restarts in the middle owes the
  result: the wake that ends the session posts it from the journal
  ([`update.md`](update.md), *A session as a job*).
- **Partial progress and lease extension.** `agent::FirmwareJobs` (task p4-05) posts `progress`
  and asks `extend` as a `twinoak.fw.offer` downloads in resumable ranges; a wake that runs out of
  download budget stops mid-image, posts no result, and the next wake resumes — into the
  candidate slot or the scanner's, whichever the offer named. This is
  `spec/proposals/02`'s shape, now built — for a **radio** image. The Walter's own slot keeps no
  progress record and is erased when a write opens on it, so there the next wake starts the file
  again from zero, which is what that branch's messages now say (task p10-04;
  [`flashing.md`](flashing.md) has the arithmetic of whether it fits one wake).
- **`nextCheckInS` and the per-wake budget** (proposal 01's B and C). The library proposes them in
  the heartbeat; the wake cycle honours an accepted `nextCheckInS` **only when it is longer** than
  the band's own interval (`BackendAnswer::sleep_at_least_s`), because the battery's say wins. The
  per-band transaction cap is `agent::identity.h`, a bench placeholder.
- **The console line.** `CLI` over the link is a person's command, and `agent config.apply`/`cli`
  jobs for the own repeater reach it through the local door — never as a wake-cycle ask.
- **The time of day.** A heartbeat's reply may carry the server's time; `AgentBackend` offers it to
  the `NetworkClock`, which keeps it only when the cellular network has not already given one. A
  unit that has not attached still says it does not know what day it is (`known.time_known`).
