# A unit in the field

What a TwinOak repeater-v2 unit on a mast does by itself, what it does when something is wrong,
what it does for the server that looks after it, and what a person can and cannot do about it
from a distance. For the person who owns the mast, not for the person who writes the firmware —
the firmware's own map is
[`ARCHITECTURE.md`](ARCHITECTURE.md), the wake is [`cycle.md`](cycle.md), the console is
[`console.md`](console.md), putting a release on a module is [`flashing.md`](flashing.md) and
the first bring-up of a unit is [`bench.md`](bench.md).

> **No unit has run this yet.** One Walter on a development build checked in with the real
> meshmaster.dk once, on 23 October 2026; everything else below is built and tested on a host
> against simulators, and none of it has been on a mast. The first unit to run it is the first
> unit, and [`bench.md`](bench.md) is the walk that proves each of these sentences one at a time —
> with `v0.1.0`, the first release ([`flashing.md`](flashing.md)).

## The two boxes

A unit is two boxes on one battery.

The **LoRa box** (a RAK4631) is the repeater. It is the unit's identity on the mesh and the only
thing that talks to the air. **It does not need the other box**: it boots, repeats and answers
mesh administration with the LTE box asleep, dead or absent. If the LTE box is removed from a
working unit, the mesh does not notice.

The **LTE box** (a Walter: an ESP32-S3 with an LTE-M modem) is the one this firmware runs on. It
looks after the repeater and is the unit's way out: it watches the repeater, replaces the
repeater's firmware when a new one is published, updates itself, and reports in over the mobile
network. It is asleep almost all of the time. **It does not need the repeater's firmware**: with
the reset line, the power-kill line and the debug port it can bring back a RAK whose application
is gone.

So: *the repeater keeps repeating* is the first thing, and everything the LTE box does is arranged
not to get in the way of it.

## What one wake does

The LTE box sleeps and wakes. **One wake is one pass of ten steps, in order, and then deep
sleep.** It is minutes at the outside and seconds in the ordinary case
([`cycle.md`](cycle.md) has the table and the time each step may spend):

1. **why am I awake** — the timer, the radio asking for attention on its line, a person with a
   cable, a power-up, or a crash;
2. **is the radio alive** — one `PING`. If it is not, the ladder (below);
3. **was something in progress** — the journal says; an update that was interrupted continues here
   and nothing else is begun;
4. **what does the radio say** — its state, its battery and panel currents, its temperature;
5. **what may this wake afford** — the battery decides the rest of the wake and the next interval;
6. **the modem** — attach, and read the network's time;
7. **set the radio's clock** — from the network's time, when the radio's is unset or wrong;
8. **report in** — one exchange with meshmaster.dk: a heartbeat, and the jobs the server has for
   this unit, run one at a time (*As an exit node*, below);
9. **at most one update step**, of one box, never both (and only if the battery allows);
10. **tell the radio I am going, and sleep** — always. A wake that reached nothing else has still
    done the one thing that matters.

**How long until the next wake comes from the battery.** A full pack under a sunny panel gets a
wake every quarter of an hour; a sinking one in December gets one a day; below a floor the modem
is not used at all ([`power.md`](power.md) is the table). The thresholds are honest guesses until
a pack has been measured.

Nothing a wake does is retried for ever: every step has a deadline, a whole wake has a bound, and
a wake that has not reached sleep in five minutes is **ended by a reset** — the work is in the
journal and the next wake takes it up. The sleep after a fault is *longer*, not shorter.

## As an exit node

Step 8 is the one a person sees from the outside. A provisioned unit is registered at
meshmaster.dk as **an exit node** — *the uplink of* its repeater — and from then on it is the way
the server reaches that repeater, and sometimes its neighbours too.

**When it checks in.** Only when it is awake, which is on its own interval and nobody else's: a
quarter of an hour on a full pack, an hour on a middling one, six hours when it is saving, once a
day at the floor. The server may ask for a *shorter* interval and is not given one — the battery
wins; it may ask for a longer one and gets it. Between wakes the unit is in deep sleep with the
modem off and **cannot be reached at all**. Nothing queued on the server is lost by that: a job
waits until the next wake, and a wake takes as many as its band affords (two at the top, none at
the floor).

**What it will do for its own repeater**, over the UART between the boxes, with no packet on the
air: read and change its settings, read its telemetry and status, set its clock, its access list,
its password; flash it a new application and roll it back; power-cycle it. These are the jobs the
server shows against the repeater itself, and the unit is simply how they get there.

**What it will do for a neighbour** — another repeater on the air, which this unit's radio can
reach: the same read-only and administrative jobs the MeshMaster agent does anywhere, each **one
packet** from this unit's radio and one reply. It never floods the mesh with chat, never relays
somebody else's traffic as a job, and it answers a neighbour's job only if that repeater's own
access list lets this unit in. A job for a neighbour costs airtime, so the server's own budget
(two transactions a minute by default) is what paces them.

**What it will not do.** It will not take a server's word about an image: a release is checked six
ways against the bytes in its own flash, the server's `expectedSha256` is a thing it must *already*
hold, and whether a flashed image is good is decided by the health check in the box — a `rolledBack`
is the unit disagreeing with the server, and it is allowed to. It will not do two updates in one
wake, nor any while its own image is unconfirmed. It will not erase a chip or write UICR for
anybody (that is a typed command at a bench, `spec/SYSTEM.md` S-5a). And it will not keep a job's
reply: what the mesh said goes into the job's result and nowhere else — not into the log, not into
the journal.

**What *asleep* means on the page.** The exit node's page shows a unit *asleep* when the last
heartbeat said so, with the time it expects to wake. That is the normal state, and *last seen two
hours ago* on an hourly unit is a healthy unit. A unit is **silent** when it has missed several
wakes in a row — then read the list below.

**What *Wake it up* costs.** Nothing on the server can wake a sleeping unit: there is no path to
it. *Wake it up* queues work and shortens the *requested* interval for the next check-in; it takes
effect at the next wake the battery allows, and the unit will refuse an interval shorter than its
band's. A person at the mast can shorten it for real — the radio's attention line wakes the LTE box
at once, which is what the repeater uses when something is wrong.

**How an update reaches it, and what a person sees.** Four steps, each its own job, each visible:

1. **offered** — the server names a release; the unit fetches the manifest and the image in ranges
   over several wakes if it must, checks all six, and stores it as the **candidate**. The page
   shows `stored` with the hash and the bytes (`resumed` when a wake took up where another left
   off). A download that is cut off simply resumes; nothing is reported until it is whole.
2. **applied** — the server asks for the candidate it named. The unit flashes it, resets the radio,
   waits for it to come back as itself, and journals every transition.
3. **committed** — the new image passed its health check and is now the golden one. The page says
   so, and `rak hello` at the cable says the new version.
4. **rolled back** — it did not. The golden image goes back, proves itself, and the broken
   candidate is on a bad list so that nothing tries it again. The page says `rolledBack` **with
   the journal**, which is where the reason is. This is not a failure of the unit; it is the unit
   doing the thing it exists for.

A `twinoak.fw.apply` of the *LTE box's own* firmware is the same, one step longer: the board
reboots into the image being tried, and the result is posted by the **next** wake — after the new
image has reached its main loop, talked to the radio and been recognised by the server in one
boot. If any of the three fails, the next boot rolls back.

**When the server says *uplink silent*.** The server has not heard from this unit for several of
its own expected intervals. In order:

1. **Look at the battery** in the last heartbeat. A unit that reported `saving` or `critical` is
   working correctly and is simply slower — six hours or a day between wakes, and no modem at all
   at the floor. Wait a day before worrying.
2. **Is the repeater still on the air?** If the mesh still sees it, the unit's LoRa box is fine and
   only the LTE box or its coverage is not — which changes nothing for the mesh and can wait for a
   visit.
3. **Coverage.** A modem that cannot attach is the commonest cause and the one nothing can be done
   about remotely. The unit keeps trying on its own interval, with a backoff, for ever.
4. **A self-update that went wrong** is the one that would be the unit's own fault — but an image
   that cannot reach the server is rolled back by the next boot, so a unit that is silent *because*
   of an update should come back by itself within two wakes. If it does not, it is not the update.
5. **Then it is a visit**: a cable, `dump`, `journal` and `wake` (*Reading a unit from a cable*).
   The journal survived whatever happened.

## When the radio is silent

A repeater that does not answer `PING` is the one thing this unit exists for. The LTE box first
works out **what the silence means** — the radio is running NoiseScope and is answering text
instead of frames; or this Walter has only just powered up and its own power-up glitched the
reset line, so the radio is still booting; or an update is in progress and the radio is meant to
be quiet; or the radio really is not answering. Only the last is an incident.

Then it climbs a ladder, slowly, one rung per try, each with a wait and a **daily cap**
([`supervisor.md`](supervisor.md)):

1. **ask again** — three more attempts after a pause;
2. **a reset pulse** — 100 ms, and never two inside a second;
3. **a power cycle** — the LoRa rail off for a few seconds;
4. **look at what is on the chip** — read the golden image back over the debug port and compare.
   If the flash matches, the firmware is intact and flashing would change nothing. If it differs,
   the golden image goes back on;
5. **stop, record it, and report it at the next check-in.**

A radio that does not answer after its golden image has been put back is **a hardware fault**: the
unit stops trying, says so, and leaves it alone until a person, the server, or a long quiet period
(days) lets the ladder be tried once more. That is deliberate. A unit that power-cycles its radio
every minute is worse than one that waits for somebody.

**What a person sees:** `rak watch` on the console lists the incidents — when, what was seen, each
rung and its outcome, how it ended — and the day's counts against the caps. The counts survive a
restart and a power cut; they are forgiven only when a whole day can be shown to have passed.

## When the modem has no coverage

Nothing dramatic. The attach fails with a named reason, the reason has a retry rule, and the unit
goes to sleep:

| What the modem said | What the unit does |
|---|---|
| `modem-silent` — the module answered nothing | one reset of the modem inside the wake, then try again next wake |
| `no-sim` — no card, or it does not answer | nothing automatic: a person has to act |
| `sim-locked` — the card wants a PIN | **nothing is sent to the card.** Never automatically: three wrong PINs block a card for good |
| `not-registered` — no network found | try again, with a **growing backoff counted in wakes**: next wake, then every second, then every fourth, up to a cap |
| `no-data` — registered, no packet service | a few retries inside the wake, then the backoff |

The backoff is counted in **wakes**, not in hours, because a Walter has no clock it can trust
across a deep sleep. `lte` on the console says which failure it was and how many wakes until the
next attempt.

**A unit with no coverage is still doing its job.** The readings it takes are kept in a small ring
and sent when there is a way out; the radio's clock is the one thing it cannot set without the
network, and `spec/LINK-PROTOCOL.md` is explicit that a clock which goes backwards is worse than
no clock, so it leaves it alone.

## When the battery is low

The battery is the repeater's as much as the Walter's, and the Walter is the one that gives way.
Five bands ([`power.md`](power.md)), each with an interval and a rule about what a wake may do:

| Band | Roughly | What a wake does |
|---|---|---|
| `plenty` | above three quarters, or charging steadily | everything, every quarter of an hour |
| `normal` | between | everything, hourly |
| `saving` | about a quarter left | every six hours, and **the modem only every fourth wake** |
| `critical` | below the floor | once a day, and **the modem is not used at all** |
| `no-reading` | the radio is silent, so there is no gauge | the medium interval, and the modem *is* used — a unit whose radio has died is exactly the one that must reach its uplink and say so |

An update — of either box — is refused while the battery is below its own threshold or falling
fast. A repeater that keeps repeating is worth more than a report that it is about to stop.

The gauge is on the **radio's** I2C bus, so a dead radio is a dead gauge. The one flash that exists
to unbrick a radio is allowed on the last good reading rather than refused for want of a fresh one.

## Reading a unit from a cable

Plug a USB-C cable into the LTE box and open the port at any baud. **A boot that finds a USB host
stays awake** — this chip cannot be woken from deep sleep by a cable, so press the reset button
after plugging in if the port is quiet. While a host is there, nothing sleeps by itself.

Four commands answer almost every question ([`console.md`](console.md) has them all):

- **`status`** — is this unit alive, does it know its radio, has it a way out, and when does it
  next do something. Twelve or thirteen lines. Read this first.
- **`journal`** — *what happened*, oldest first, from flash. **Not one line per wake**: a wake
  with nothing to say is not journalled, so a week of a healthy unit is a page and not a thousand
  lines. A line appears when a wake had news — a step failed or was refused, the band changed, the
  ladder was climbed, the clock was set, an update step happened, the wake was a crash. Updates
  and incidents are never pushed out of the log by ordinary wakes.
- **`wake`** — the last ten wakes with their reasons and how long each lasted, from RTC memory
  (so a power cut empties it, and a reset does not).
- **`dump`** — the last four kilobytes the port printed, boot lines included. The command to run
  *before* asking what happened.

A wake's record is one line, and its `steps` field is one letter per step:
`y` done, `-` nothing to do, `s` skipped because something it needed did not happen, `t` not begun
for want of time in the wake's budget, `r` refused, `x` attempted and failed, `.` not reached.
A healthy day with coverage is mostly `y` and `-`. [`cycle.md`](cycle.md) has the table.

Two more worth knowing: **`rak watch`** (the supervisor's incidents and the day's counts) and
**`cycle dry`**, which prints what a wake *would* do right now, step by step, without doing any of
it. `cycle now` runs one for real — and it ends in deep sleep, so the port goes quiet. That is the
command working.

## "The supervisor has stopped"

At every boot, a unit whose supervisor has stopped says so:

```
supervisor: STOPPED: <reason>; N s of the quiet period left
```

It means rung 5 was reached: the ladder was climbed to the top and the radio still did not answer,
or what was found is not something a reset or a reflash can mend (a protected chip, a chip that
cannot be read). **The unit has deliberately stopped trying.** It is not broken, and it is not
hiding anything: `rak watch` has the whole incident, rung by rung, and the stop is in the journal.

What to do:

1. **Read `rak watch` and `journal`** and find which rung failed and what the reason was. The
   difference between *the chip holds the golden image* (so the firmware is intact and something
   else is wrong — a connector, a radio, the antenna) and *the chip differs* (so a flash was
   tried) is the whole diagnosis.
2. **If it is worth another try**, `rak ladder` on the console lifts the stop and climbs again from
   rung 1, and says so in the journal. Without that, the unit waits out a quiet period of days and
   then tries once by itself.
3. **If the reason is `protected`**, stop: see below. Nothing remote will mend it.
4. **If the radio answers again and nothing was done**, the day's caps may be what refused a rung
   (`today: resets 6/6`). `rak watch reset-caps` clears them — a decision, not a formality.

## How a unit is provisioned

A unit is provisioned once, at a bench, over the cable, before it goes out
([`provisioning.md`](provisioning.md) is the session, line for line):

- the APN and, if the card wants one, the SIM's PIN — **typed once and printed by nothing
  afterwards**; `get` says *set* or *unset* and never the value;
- the server and **this unit's own token**, made once on meshmaster.dk when the unit is
  registered as an exit node and readable nowhere afterwards;
- the unit's name and where it is;
- the check-in interval (`auto` lets the battery choose, which is what a mast wants).

The settings live in the Walter's own `nvs` partition, above the application slots, so flashing a
new application keeps them. **They are not encrypted**: a person with the board can read the PIN
and the token out of the flash with `esptool`, and the unit says so at every boot. Treat a unit
that has been in somebody else's hands as a unit whose secrets are theirs.

**The radio's base and its first golden image** are the two things provisioning cannot do by
itself. A radio as RAK ships it runs RAK's own firmware, not the bootloader and SoftDevice our
images are linked against; and a new unit's LTE box has an empty image store and will not flash a
radio it has no known-good image for. So, at the bench, with the radio on the cable:

1. the repeater release's base is put into the LTE box's store (`tools/walterctl.py push --as
   base`), and a person types `rak provision <device id> confirm` — the one command in this
   firmware that erases a chip, which is why it is typed, naming the radio, and never automatic
   (`spec/SYSTEM.md` S-5a; [`provisioning.md`](provisioning.md), *A factory radio*);
2. a signed release of the repeater's firmware is put into the store as the **candidate** — over
   the cable with `tools/walterctl.py push --as candidate`, or over the air by the unit itself.
   For the over-the-air half the server has to be able to pick an image, which it does by role,
   board **and** band; with no golden image yet those three come from `HELLO`, so the radio must
   speak box link **1.1** (repeater v0.1.1 or newer, task p8-05). A radio on a 1.0 image says no
   band and takes its first install over the cable;
3. the radio is flashed from it with `rak update --first-install` (the radio has no application of
   its own, and until this commits the LTE box leaves the silent radio alone), and the image that
   passes its health check becomes **golden**;
4. from then on a new release becomes the *candidate*, is flashed, must boot and prove itself, and
   is either promoted to golden or rolled back — and a candidate that failed once is never tried
   again ([`update.md`](update.md)).

**Until a release key exists, a unit accepts nothing.** Every manifest is refused at the first of
six checks, which is correct: the alternative is a unit that trusts whatever a server sends.
`trust` on the console says which keys this build accepts — today, none.

## What cannot be recovered remotely

The honest list. Each of these needs a person with the unit in their hands.

- **A protected nRF52.** If the radio module's access port is closed (some module batches ship that
  way), the LTE box can see the chip but not its memory. By itself it **detects this, says so, and
  stops**. Opening the port needs a chip erase, which destroys the bootloader, the SoftDevice, the
  node's identity and its settings — so nothing automatic in this firmware can do it: not the
  supervisor, not an update, not a job from the server. A person with the unit in their hands can:
  `rak provision locked confirm` on the console (`spec/SYSTEM.md` S-5a), which erases the radio
  whole and gives it its base, after which it is a factory radio again and gets its first
  application as above. A radio whose application still answers must be named by its key
  (`rak provision check` says how), so that nothing with an identity is erased by a slip.
- **A dead bootloader on the radio.** The LTE box's *updates* replace the radio's application and
  nothing else: never the bootloader, never the SoftDevice, never the file system that holds the
  node's identity. That is why a failed update cannot brick the radio — and why a bootloader that
  is itself damaged is not repaired by any update. The same typed `rak provision` is the repair:
  with a cable, at the unit, never from a distance. The command as a MeshMaster job, audited and
  behind a second factor, is S-5a's *later*.
- **A locked SIM.** Three wrong PINs block a card; only the operator's PUK opens it. The firmware
  therefore sends a PIN **once** and remembers the refusal, and a second attach does not try again
  until somebody sets a new PIN. Nothing automatic can unblock a card.
- **A Walter whose own both slots are bad.** The LTE box updates itself A/B with rollback, and a
  new image that cannot reach its main loop, the radio *and* the server inside one boot is rolled
  back by the bootloader. The one state nothing can mend remotely is an image that was confirmed
  and is nevertheless broken — which is what the three conditions exist to prevent. Recovery is
  `esptool` over the module's USB-C.
- **A radio that answers nothing after its golden image is back.** The ladder reaches rung 5 and
  stops, which is the right answer: the fault is hardware, and no amount of resetting mends
  hardware.
- **The battery.** A pack below the floor gets a unit that wakes once a day and does not use its
  modem. If the panel never recovers it, the unit goes quiet, and the last thing it will have said
  is that its battery was sinking.

## And what *is* recoverable from a distance

For balance, because the list above is the short one:

- the radio's firmware, as often as a release says so, with a known-good image behind it;
- the LTE box's own firmware, with one boot's trial and automatic rollback;
- a radio that is hung, confused, or has had its application corrupted — the ladder;
- a radio whose clock has drifted, every wake;
- a non-secret setting of the repeater's, over the server, as a job;
- a repeater that needs its power cycled, as a job, within the day's caps;
- a reading of what happened: the journal survives power cuts and is what a person reads first.
