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
| 8 | `backend` | one exchange over the seam, and the settings it asked for (`interfaces/backend.h`) | `uplink` | 60 s |
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
happened; an update step or a fetch happened; the wake was a crash or a watchdog. After a week on a
mast `journal` is then a page of *what happened*, which is what the task asks it to be.

The record is written **before** the sleep, because a sleep does not return. A sleep that was then
refused writes its own entry after it (`core/cycle/sleep.h`), and the two together are the story.

## The budget, and the two things that may hold a wake open

`kWakeBudgetMs` is **two and a half minutes** and is what a wake may spend in all. Each step has
its own bound and its own *least*: a step is begun only when what is left of the budget is at
least its least, and its deadline is the sooner of its bound and what is left. A step that cannot
be begun is recorded as `t` — *no time* — with a reason a person can read.

The arithmetic: the uplink's registration wait is two minutes of it
(`core/uplink/policy.h`, `kRegistrationDeadlineMs` — the first number the bench changes), the rest
of a wake is seconds, and the **bound on a wake** (`core/cycle/wake_guard.h`) is five minutes — so
there is twice the budget of margin before the rail that resets a wake, which is what one
worst-case modem call that comes back late costs.

Two things are allowed to hold a wake open past the budget, each up to its own bound, and both
**raise the bound on the wake** rather than removing it:

| | Why | Up to |
|---|---|---:|
| a climb | the ladder waits 30, 60 and 120 s between rungs, and **the state of a climb does not survive a sleep** — only its counts and a stop do. So a climb runs to its end inside its wake or starts again at rung 1 in the next one, and the caps are what keep that from becoming a loop | 6 min |
| an update | "a flash may take several wakes' worth of time and is allowed to hold the wake open up to its own bound" (the task). A megabyte over a 100 kHz debug port, a 20 s boot window and a three-minute proving period do not fit in five minutes | 20 min |
| a trial | this firmware proving itself (S-6) has to finish before the wake may sleep: a deep sleep during a trial is a rollback of the image being tried | 6 min |

The time those take is **outside** the budget, because both machines are already bounded and
capped by their own rules — and because a wake that spent six minutes on a ladder must still be
able to reach its uplink and say so.

A raised bound is still a bound. A wake that overruns even the raised one is ended by a reset, the
journal carries the unfinished work, and `resume()` takes it up again at the next boot. That is
the whole reason the reset is safe (`docs/update.md`).

## Nothing runs twice

Each step runs once per pass, in order, and `run()` is not re-entrant. **At most one update step
happens in a wake, of one MCU** (S-7): when both are asked for, the Walter's own goes first —
`docs/ARCHITECTURE.md` §7 has it lead a link-version change, and its own update ends in a restart
anyway — and the radio's is refused with `one update a wake, and the other one went`, recorded,
and told at the next exchange. The two machines already refuse each other through the journal;
this is the third lock on the same door, and the one that reports it.

**Asks do not cross a sleep.** A refused ask is remembered and reported; the ask itself is not.
A backend that still wants something asks again at the next wake, which is one exchange later and
costs nothing, and a firmware that remembered an ask across a reset would be a firmware that could
be made to do something nobody asked for twice.

## The sleep, and when it is shortened

The interval is step 5's (`docs/power.md`). It is **shortened to five minutes when a download is
half done and the band is `plenty`**, so that a release arrives in a morning rather than in a day —
and **never in `saving` or `critical`**, where the wakes are what the pack can pay for.

A sleep is refused when the radio's update has a step in the air, when a transfer into the other
application slot is open, or when this firmware is on trial. The wake then ends in
`the sleep was refused`, the bound on the wake is what ends it, and the next boot resumes.

---

# The page for the implementer of `Backend`

This is the hand-over to the agent work (`spec/SYSTEM.md` §8, `spec/proposals/01` and `03`;
the Walter's phase 4). **Nothing in `interfaces/backend.h` names MeshMaster, a URL path, JSON or a
job**, and the only implementation in this repository does nothing
(`core/cycle/null_backend.h`): the server side is three accepted proposals and a decision about
where it will be built, not a protocol, and code written here against a guess would have to be
found and removed later.

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

## What this firmware does **not** offer you, and what an agent will need

Said plainly, because it is the list phase 4 has to add:

- **Nothing of the mesh.** No job, no login, no CLI line over the air, no observation pull — the
  link client has all of it (phase 1) and the wake cycle calls none of it. A test runs a simulated
  month of wakes and holds the radio to that.
- **No job identity.** There is no job id, no lease, no progress report and no result envelope.
  An ask is carried out or refused inside the wake it arrived in, and what happened is in the next
  exchange's `known`. A server that needs to correlate a request with a result will have to carry
  its own id outside this interface — which is exactly the shape `spec/proposals/02` describes
  (`twinoak.fw.offer` / `.apply` / `.state`) and exactly what should not be guessed at here.
- **No partial progress during a download.** A fetch that stops on its budget leaves the bytes and
  resumes at the next wake; nobody is told how far it got until the next exchange. Lease extension
  and byte-level progress (proposal 02) are above this seam.
- **No `nextCheckInS` and no `maxTransactionsThisWake`** (proposal 01's B and C). The interval is
  the battery's and this firmware does not negotiate it; an implementation that wants to tell a
  server when to expect the unit back has `known.wake.next_interval_s` and may send it.
- **No console line.** `CLI` over the link exists and is a person's command, not an ask.
- **No time of day you can rely on.** A unit that has not attached does not know what day it is,
  and says so (`known.time_known`). It will not guess one.
