# Updating the radio, and the Walter itself

How the Walter replaces the repeater's firmware without ever leaving the unit worse off than it
found it (`spec/SYSTEM.md` S-5 and §6, `docs/ARCHITECTURE.md` §7), and how it replaces its own
(S-6, S-7). Task p2-03 built the radio's half and task p2-04 the Walter's; the code is
`components/core/update/` and the tests `host_test/core/update/`. This file is what a person at
the console needs: the diagrams, the checks, what each line printed means, and what state a unit
is in after a power cut at each step. The Walter's half is the second part of this file.

## The idea in one paragraph

A new image never replaces the old one by being written. It is flashed into the radio, the radio
is reset into it, and it then has to **prove itself** - answer `HELLO` as the image it claims to be,
take the clock, say its radio initialised, stay up for a proving period of minutes without
restarting, answer for its telemetry. Only then does the store's **golden** pointer move to it
(`image list` shows it golden) and the old golden image is let go. If it fails any part of that, its
hash goes on the **bad list first** and the golden image is flashed back, booted and proved by the
same check. A candidate that failed is refused for ever after, before a sector moves. Every state
is written to a **journal** in the Walter's own flash before its action starts, so a Walter that
loses power in the middle reads the journal at boot and picks up where it was. It never guesses.

## The machine

```
idle ─► check ─► flashing-candidate ─► booting ─► proving ─► committing ─► done
           │            │                 │          │  └─► node-fault
           │            └─────────────────┴──────────┴──► rolling-back ─► rolling-back (booting the golden image)
           └─► refused (a precondition; nothing was touched)                 │
                                                                             ▼
                                                     rolling-back (proving the golden image) ─► restored
                                                                             │
                                                                             └──► stuck
```

`core::update::RadioUpdate` is a state machine over interfaces - the image store, the flasher, the
link client, the journal, a clock, the battery - driven one `step()` at a time. Each call does at
most one thing that takes time (a flash, a `HELLO`, a `PING`) and returns, so phase 3's wake cycle
can run it in slices and every transition is a line a test can hold. `rak update` on the console
runs it to the end inside the command.

| State | What is happening | Journalled before |
|---|---|---|
| **check** | the preconditions, below; nothing is touched | the start of the run, with the image's SHA-256 |
| **flashing-candidate** | the flasher (`docs/ARCHITECTURE.md` §5) writes the candidate into the application region, verifies it, writes the bootloader's settings record and **starts the image** — with a pulse on the reset line, the one reset that clears a watchdog the firmware it replaced had started (task p18-01), or, on a module whose `UICR.PSELRESET` says it has no reset pin, with a reset through the debug port (task p18-04, *What starts a flashed image* below). Either way it is the **only** reset of a flash, so the boot that is proved is the boot it began | the flash |
| **booting** | waiting for the radio to come up: an `EV_BOOT` or a `HELLO` answer, inside the boot window. A window that stays empty after a **pulse** is worth one look at the chip first (*What starts a flashed image*), and a second entry says what it found | the wait |
| **proving** | the health check, below | the check |
| **committing** | the store's promotion: the golden pointer moves to the candidate's area (seven flash operations, whatever the size) | the promotion |
| **done** | the candidate is the golden image; the candidate slot is empty — plainly empty, not a download half done: the area it now points at is the old golden image's, and the markers that image's own download left there are not read as a transfer (`ARCHITECTURE.md` §6, task p15-01) | terminal |
| **rolling-back** | the candidate's hash is put on the bad list, then the golden image is flashed back and started by the same pulse | the rollback, with why and at which step of the check |
| **rolling-back (booting the golden image)** | as booting, for the golden image | the wait |
| **rolling-back (proving the golden image)** | the same health check, held against the golden image's own descriptor | the check |
| **restored** | the golden image is back, proved and still golden; the candidate is in the store but on the bad list | terminal |
| **stuck** | the golden image did not come back either, or there was none (a first install that failed): the radio has no proved application. The supervisor's last rung, a report, and no further attempt by this machine | terminal |
| **refused** | a precondition failed at check; nothing was touched | terminal |
| **node-fault** | the image proved everything an image can and **the node** is the fault: today that is one thing, a radio that has lost its identity (check 1a below). No rollback is spent, nothing goes on the bad list, nothing is promoted - the candidate is on the radio and in the store, the golden image is untouched, and the way out is a person's `erase` and `reboot` at the radio. The same candidate may then be applied again | terminal |

## What starts a flashed image (task p18-04)

A flash ends with the core halted and the new image in flash; something has to start it. Which of
the two it is, this firmware asks the chip at every flash and decides from the answer — never from
what it decided last time.

**`UICR.PSELRESET[0]` and `[1]`, read.** They are the two registers a pin reset is configured by
(Product Specification 5.3.6.2, 4.5.1.62–63): `PIN` in bits [4:0], `PORT` in bit 5, `CONNECT` in
bit 31 with 0 meaning connected, and the rule that "all PSELRESET registers have to contain the
same value for a pin mapping to be valid. If values are not the same, there will be no nRESET
function exposed on a GPIO." A provisioned RAK4630 holds 18 in both: nRESET on P0.18. **This
firmware never writes them** — a module without a pin reset has none whatever the wire does, and
putting one there is an act of provisioning and a person's decision. Rail R3 holds the read to one
function of `core/nrf52/nrf52.cpp` and holds it to being a read.

- **The pin reset is enabled** → the flash ends in a pulse on nRESET (task p18-01).
- **It is not** → the flash ends in a reset through the debug port, which is what every flash
  ended in before task p18-01. A pulse on a pin that is not nRESET starts nothing, so none is made.

**A pulse gets no confirmation.** `SYSRESETREQ` can be read back in `DHCSR.S_RESET_ST`; a pin
cannot. So when nothing of the radio's comes inside the boot window after a **pulse**, the debug
port is opened once more and the core's state read — **one such look a flash, never two**:

| What the look finds | What happens | The journal's line |
|---|---|---|
| the core still **halted** where the flash left it | the pulse reached nothing: the image is started through the debug port and the window begins again | `booting: the pulse reached nothing…` |
| the core **running** (or sleeping, or locked up) | the pulse worked and it is the image that is quiet: nothing is sent to it | `booting: the pulse started the image…` |
| the port closed or silent | the ending there has always been | `booting: the chip could not be asked…` |

**And what the debug port's reset costs**: a soft reset is not among the resets that clear the
nRF52840's watchdog (5.3.6.8's *Reset behavior* table), and NoiseScope starts one at 60 s and feeds
it from its own loop while a repeater feeds nothing. So an image the debug port started **over a
scanner** inherits that watchdog and is reset once inside its time — which is the fault of
21 November exactly (task p18-01). The proving period therefore begins at the boot that follows
that reset, or when `kScannerWatchdogWaitMs` has passed since the image started without one; the
journal says which, and the proof itself is unchanged — a restart inside the proving period is
still a failure.

**What the unit remembers, and says.** Whether a pulse has been seen to restart the radio, seen not
to, or never tried is kept beside the reset spacing in RTC memory and printed by `rak watch` and in
the check-in's `radio` section. It is for a person to read: every flash decides for itself, from
the chip.

## The preconditions (check)

Each is a named refusal, printed as `error: rak update: refused at check: <reason> (nothing was
touched)`; the journal gets the same words.

| Refusal | Means |
|---|---|
| *no board profile for the slot* | `rak @1 update` on a single-radio unit |
| *the store holds no image to flash* | no candidate (`image list` says `empty`); for `rak role scanner`, no scanner image |
| *the stored image does not verify against its record* | flash rot: `image verify candidate` says the same. Nothing is flashed from an image whose bytes are not its hash's |
| *this image failed a health check once and is never tried again* | the hash is on the bad list (`image bad`) |
| *the image's role is not one this slot may hold* | a scanner image as the candidate, or a repeater image for `rak role scanner` |
| *the image's board is not the board in this slot* | the descriptor's board is not the slot's profile, or its `slot` byte names another position |
| *the image's band is not the golden image's* | a 433 image on an 868 unit: the band this slot is known to be is the golden image's |
| *there is no golden image to return to (a first install needs saying so)* | no golden image, and the command was not `rak update --first-install` |
| *the link is down and nobody has said why* | `HELLO` is not answered. The supervisor (task p2-05) may pass the update a link it has classified as down for a known reason; a first install implies it (a blank chip answers nothing). **Only the supervisor's `silent` is such a reason** (`core::supervisor::linkKnownDown`): a slot running NoiseScope and a slot awaiting its first install answer no `HELLO` either and neither is passed through, so an apply of theirs is refused here |
| *the battery could not be read* | `POWER_GET`'s battery channel did not read (a first install, with no radio to ask, is let through: a person at the console is the gauge). The one other way past it is the same classified link above - the INA3221 is on the *radio's* I2C, so a link the supervisor has given up on is a gauge this Walter cannot reach |
| *the battery is below the update threshold* | below `kBatteryMinMv` |
| *the battery is falling fast* | a trend steeper than `kBatteryFallingMvPerHour`; the trend comes from phase 3's power ring, and until then no reading has one |

And two that are refused before the run even starts, by `start()`, with their own lines:

- *the journal says a radio update is in progress: it is resumed, not restarted (S-7)* - a run is
  journalled as unfinished. It is taken up at the next boot (or now, with `reboot`), never
  started over.
- *the Walter's own update is unconfirmed: never both at once (S-7)* - the Walter's own A/B update
  (task p2-04) is journalled as on trial. One wake updates the radio or the Walter, not both.

## The health check (proving)

All of, in this order, against the **descriptor of the image that was flashed** (the store keeps a
copy of its 96 bytes in the record; `spec/FIRMWARE-MANIFEST.md` §4: "After a flash, the Walter's
health check holds the `HELLO` it gets against the descriptor it flashed"):

1. **`HELLO` answers**, and its `fw_version`, `role` and `board` are the descriptor's, and its link
   major is one this Walter speaks. A radio that refuses `HELLO` with `ERR_UNSUPPORTED` has booted
   and does not speak this major (`spec/LINK-PROTOCOL.md` §5.1): a failure, not a silence.
1a. **The node is there, and not refusing.** From repeater `v0.1.2` a radio whose file system
   would not mount makes no key: it keeps the link up, advertises `caps 0x21` (`CLI`, `ATTENTION`),
   answers `IDENTITY` with **`ERR_STATE`** and raises `EV_ATTENTION` reason 6 detail 1 (the
   repeater repository's `docs/flashing.md` §3a and its `tasks/phase-3.9/reports/01.md`). **That
   is not a failed image** - the image booted, answered and said what it is - and no image can
   mend it, so the run ends **node-fault** and not in a rollback: a rollback would ban a good
   image and flash the golden one into the same unmountable file system. A radio that has **no**
   identity yet - an all-zero key, or none asked for - is no fault at all: a first install starts
   exactly there, and the check goes on. The three states are told apart in one place,
   `components/core/include/core/link/radio_identity.h`. The frame costs nothing in the ordinary
   case: it is sent only when §5.1's `caps` are the lost mode's own, or when check 2 is about to
   fail (a radio in that mode has no clock either, and *the radio would not take the clock* was
   what spent the rollback before this check existed).
2. **The clock can be set**: `TIME_SET` accepted - the first thing a healthy repeater must take
   (§5.4). A Walter with no time of its own **in this boot** cannot make this check; it says so
   (*the clock check was skipped*) and asks `TIME_GET` instead, which must answer. Its own clock
   is the network's, taken again at every wake because the monotonic counter restarts at each one
   (`core/cycle/network_clock.h`): a wake's step 6 takes it, `lte up` at the console takes it, and
   the agent's server time fills it where the cellular network gave none. So the way to make this
   check on the bench is `lte up` before `rak update`.
3. **`STATUS` says the radio initialised** (§5.3, flag bit 4).
4. **The link stays up** - a `PING` every `kProvingPingIntervalMs` - for the whole
   **proving period** `kProvingPeriodMs`, with **no restart** in it: the `boot_id` unchanged and
   no `EV_BOOT`. A candidate that boots, answers and restarts itself two minutes later has failed.
5. **Where the role has telemetry** (`HELLO`'s `POWER` capability), **`POWER_GET` answers**.

Anything else is a failure, named with the step it failed at: *no HELLO inside the boot window*,
*HELLO's version is not the flashed image's*, *HELLO's role is not the flashed image's*, *HELLO's
board is not the flashed image's*, *the radio speaks a link major this Walter does not*, *the radio
would not take the clock*, *STATUS does not say the radio initialised*, *the radio restarted inside
the proving period*, *the radio went silent inside the proving period*, *POWER_GET did not answer
though the role advertises it*, *the flasher did not finish*. The one answer that is **not** a
failed image is check 1a's: *the radio refuses: its identity is lost, and no image mends that*,
which ends the run *node-fault*.

The scanner's proof (`rak role scanner`) is different and smaller: inside `kScannerWindowMs`,
something that calls itself a scanner must be there - the text-mode detection of task p1-02 (no
`HELLO` answer and NDJSON lines, or a line in answer to `info`), or a `HELLO` whose role is
*scanner*. Nothing else is asked of it, and nothing in the store moves.

## The journal

`components/core/update/journal.h`: the `journal` partition of `partitions.csv` (64 KB at
`0x620000`, its own partition and not a namespace in NVS: its own wear, its own failure modes),
behind `interfaces::FlashRegion`, so the host tests run it over the simulated flash with the power
going at every write.

- **Entries are 64 bytes, fixed**: a sequence number (never reused), whose entry it is (the radio's
  update, the Walter's own, the supervisor), 48 bytes of payload, a CRC-32 over the body, and a
  magic word. **The body is one write and the magic word a second one, after it.** An entry is
  valid only with its magic word there and its CRC right, so whatever a power cut does - half the
  body, no magic, half the magic - leaves a slot that does not validate and is stepped over; the
  entry before it governs. A fixed size means the reader never trusts a length it found in flash.
- **A ring of sixteen sectors**, 64 entries each. When a sector is full the next one is erased and
  appending goes on there; the oldest is overwritten first. The first entry of a run is
  **protected**: an append that would have to erase the sector holding it is refused instead. A run
  writes a dozen entries at most, so a full journal is a bug, not a day.
- **At boot** every sector is scanned once (a 64-byte buffer, 1024 reads; nothing of the log is
  held in RAM), the newest valid entry is the one with the highest sequence number, and the entries
  can be walked backwards from it - which is what `resume()` and `rak update status` do.

The radio's update writes one entry per state it enters, with the state, what the run is for
(update, first install, a role switch), the slot, the image's SHA-256, and on a rollback or a
terminal state why and at which step. A resumed entry carries a flag, and the story shows it as
*(resumed after a restart)*.

## After a power cut

The Walter reads its journal at boot and, when the last radio-update entry is not terminal, prints
`update: the journal says a run was in progress (<kind>); resuming at <state>` and drives the run
to its end from the main loop, one step per turn, printing each transition as `rak update` would.
What the unit is in, by the entry the journal shows last:

| Last entry | The radio | What resume does |
|---|---|---|
| **check** | untouched, running what it ran | the preconditions again (and may refuse) |
| **flashing-candidate** | anything from the old application, through a half-written candidate, to the whole candidate not yet reset into. Mid-flash the bootloader's settings say *bank 0 holds nothing*, so the radio sits in its bootloader, not in half an application (`docs/ARCHITECTURE.md` §5) | the flash again - the flasher's scan skips the pages already right and writes the rest; then the reset |
| **booting**, **proving** | the candidate on the chip, reset into, perhaps running | **the proving over, from the start** - the whole period again. A proof interrupted is never assumed to have passed |
| **committing** | the candidate running; the store's golden pointer moved or not | if the golden image is already this hash: *done*; otherwise the promotion |
| **done** | the candidate, golden | nothing: the run is over |
| **rolling-back** | from the failed candidate through a half-written golden image, in its bootloader meanwhile | the hash onto the bad list again (a no-op the second time), the golden image flashed again, the reset |
| **rolling-back (booting / proving the golden image)** | the golden image on the chip | the golden image's proving over, from the start |
| **restored**, **stuck**, **refused**, **node-fault** | as the word says | nothing: the run is over |

The host tests run exactly this: the Walter reset after every one of the journal's entries of the
good path and of a rollback, and the power cut during every write of every entry, and the run
reaches the same end each time (`host_test/core/update/test_radio_update_power_loss.cpp`; the
counts are in `tasks/phase-2/reports/03.md`).

## The console

`docs/console.md` has the table; the lines, as they come:

```
> rak update
rak update: slot 0; every transition is printed as the journal takes it (this takes minutes:
  the flash, then a 180 s proving period)
update: check -> flashing-candidate: the preconditions hold
  reading the pages the image covers: page 1/62 (0x00026000)
  ...
update: flashing-candidate -> booting: flashed; the radio was reset
update: booting -> proving: the radio answered
update: proving -> committing: the health check passed
update: committing -> done: the candidate is the golden image
rak update: done: 0.3.1+abc1234 (sha256 1f2e3d4c5b6a7988...) is the golden image; 'image list' shows it, 'rak hello' its version
```

- `update: <from> -> <to>: <note>` is a transition, printed the moment its journal entry is in
  flash. The indented lines between are the flasher's progress, one per page per pass.
- `rak update: done: …` - the candidate is golden. `rak update: restored: the candidate failed its
  health check (<reason> at <step>) and is on the bad list; the golden image <version> is back and
  proved` - a rollback, and the next `rak update` with that candidate is refused. `rak update:
  STUCK: <reason> - the radio has no proved application; a person's business now` - the golden
  image did not come back (or, after `--first-install`, *no image to return to*).
- `rak update: the image is fine, the node is not: the radio refuses: its identity is lost, and no
  image mends that` and, under it, that the candidate is on the radio, on no bad list, and that
  `rak cli erase` then `rak cli reboot` (`docs/flashing.md` §3a) is the way out - check 1a. The
  radio becomes a **new node** when a person does that, so its key has to be confirmed and its
  repeater registered again on meshmaster.dk, and the settings set again; then this update can be
  applied a second time, because the image was never the fault.
- `error: rak update: refused at check: <reason> (nothing was touched)` - a precondition.
- `error: rak update: an update is already running (<state>); 'rak update status' says where` - a
  resumed run is being driven from the main loop; wait for it.
- `update: stopped in <state>: the journal could not be written` - the machine does not act
  without its journal. Nothing more happens until the next boot reads what was written.
- `(the clock check was skipped: this Walter had no time of its own in this boot, so there was
  none to give the radio. 'lte up' before 'rak update' lets it run)` after the outcome, when it
  was.

`rak update --first-install` is the same run with no golden image to return to - a person's
command only: a unit's first repeater image, loaded as a candidate over USB. If it fails, the
unit is *stuck* with *no image to return to*, and the candidate is on the bad list. On a radio
that `rak provision` has just given its base (task p5-03) the bootloader's settings page is
erased, which the flasher takes as it takes any page: the record it writes before the first erase
and the valid one after the read-back are what the bootloader starts from. A committed first
install is also what ends the slot's *awaiting its first install* (`docs/supervisor.md`).

`rak update status` prints one line about the journal (entries, sectors, the next sequence number,
the head), then the last run's story oldest first - `1. seq 12: check - update, slot 0, image
sha256 1f2e3d4c5b6a7988...`, `2. seq 13: flashing-candidate`, … `5. seq 16: committing (the
health check passed; 180000 ms of proving)`, `6. seq 17: done` - and a last line: `the last
update ended: <state>[: <reason>]`, or `now: <state> (resumed), <step>; <n> pings, <ms> of
proving; <k> entries this run` while one is running, or `the last update did not end: the
journal says <state>; it is resumed at the next boot`. On a unit that has never updated: `rak
update status: no update has been journalled on this unit`.

`rak role scanner` flashes the stored scanner image (`image receive scanner`), waits for a scanner
to show itself, and ends `rak role scanner: done: a scanner is running; golden and candidate
untouched ('rak text')`. `rak role repeater` flashes the golden image back and proves it as an
update would, through the rolling-back states, and ends `rak role repeater: done: the golden image
<version> is back and proved`. A scanner image that never shows itself is rolled back like a
failed candidate and goes on the bad list. These exist so that a NoiseScope session later needs
no new flashing logic.

## The numbers - to be set on the bench

Every window and threshold is a named constant in `components/core/include/core/update/radio_update.h`
with a sentence beside it; none has been held against a real radio yet (`docs/bench.md` §9).

| Constant | Today | What it is |
|---|---:|---|
| `kBootWindowMs` | 20 000 ms | from the flasher's reset until the radio must have answered `HELLO` or sent `EV_BOOT` |
| `kBootHelloIntervalMs` | 2 000 ms | the pause between `HELLO` tries inside that window (one try is up to three seconds of §3's retries) |
| `kProvingPeriodMs` | 180 000 ms | how long the link must stay up after a good start, with no restart |
| `kProvingPingIntervalMs` | 3 000 ms | a `PING` this often through the proving period |
| `kScannerWindowMs` | 20 000 ms | how long after the reset NoiseScope must have shown itself |
| `kStartAgainWindowMs` | = `kBootWindowMs` | the window that begins again after a look found the core halted and the debug port started the image (task p18-04) |
| `kScannerWatchdogMs` | 60 000 ms | the scanner's own hardware watchdog, copied from the repeater firmware's `WDT_TIMEOUT_S` (it does not say it on the link yet) |
| `kScannerWatchdogWaitMs` | 75 000 ms | and how long a proof waits it out when the debug port started an image over a scanner: that number with a quarter of it as margin |
| `kBatteryMinMv` | 3 700 mV | the battery below which no update starts (a single Li-ion cell: more than half its charge) |
| `kBatteryFallingMvPerHour` | -100 mV/h | a trend steeper than this refuses the update (no trend exists until phase 3) |
| `kStepIdleMs` | 100 ms | what `rak update` sleeps between steps that only waited |
| `kEntriesPerRunMax` | 48 | more journal entries than this in one run means something is looping, and the machine stops writing |
| `kSessionCeilingMs` | 20 min | how long the radio may be a scanner inside a session, counted from the moment one is found (`scanner_session.h`) |
| `kStepBytes` | 8 192 | the room one step's lines are laid down in, borrowed from the internal heap for the session where there is no PSRAM |
| `kStepBytesExternal` | 65 536 | and the room where there is: 64 kB of the module's PSRAM, which asking for it is what brings up (task p17-02) |

### And three that are **not** guesses

`kFlashWindowMs`, `kOnePassMs` and `kWholeRunMs`, in the same file, are what the unit's own journal
says a run costs. meshmaster.dk has driven two updates of the radio at Bellevue:

| | 13 Nov, 0.1.2 → 0.1.4 (job 28560) | 14 Nov, 0.1.4 → 0.1.5 (job 29475) |
|---|---|---|
| flashing the candidate | 14 min 44 s | 14 min 51 s |
| booting | 3 s | 3 s |
| proving | 3 min 03 s | 3 min 03 s |
| `check` to `done` | 17 min 51 s | 17 min 58 s |

So **a flash is a quarter of an hour**, an apply that commits is about eighteen minutes, and one
that **rolls back** — the flash and the proof twice — is about thirty-seven. `kFlashWindowMs` is
sixteen minutes, those runs with room for an image that grows again (the two were seven seconds
apart for seven seconds more image); `kOnePassMs` and `kWholeRunMs` are arithmetic over it and the
windows above. They exist because a caller that runs this machine inside something bounded — the
wake cycle's step, an apply claimed from the server — needs one number to bound it by, and until
task p14-04 those callers used numbers that were shorter than a rollback.

## A session: the scanner, a program, and the repeater back (task p16-03)

`rak role scanner` makes the radio a scanner. It stays one until a person types `rak role
repeater`. On a bench that is a person with a cable; **on a mast it is nobody**, and a board that
restarts between the two — or a server that stops answering — leaves a site with no repeater.

A **session** is the two switches as one thing with a program between them, and rails that end it
whatever happens in the middle. `components/core/include/core/update/scanner_session.h` is the
machine; `rak session <seconds> <command>` is a person's end of it and
`twinoak.noisescope.session` is the server's (*A session as a job* below).

```
check ─► flashing-scanner ─► scanner ─► step-1 … step-6 ─► room ─► flashing-golden
             │                   │                                       │
             └───────────────────┴──────► (the ceiling) ─────────────────►│
                                                                         ▼
                                            booting ─► proving ─► restored
                                                              └─► stuck ──┐
                                                                          │
 (the journal still says open: the next wake takes it up and tries again) ─┘
```

Those words are the journal's own: one entry of kind `0x000B` before each of them, with the
command on a `step-N` entry and the room's figures on the `room` one.

**The scanner's watchdog ends with the scanner** (task p18-01). NoiseScope starts the nRF52840's
hardware watchdog at 60 s and feeds it from its own loop; once started it cannot be stopped, and
the repeater that comes back after the session feeds nothing. A soft reset — which is what the
flasher used to leave by — is not among the reset sources that clear it (the Product
Specification's *Reset behavior* table), so the first session on a unit, on 21 November 2026,
measured every one of its six steps and then ended `stuck`: the watchdog reset the radio 59 s into
the golden image's three-minute proving period, and a restart there is a failure by rule. Every
flash is now left by **a pulse on the reset line**, which is in that table's WDT column, so the
image a flash starts has no watchdog behind it. The proof itself is unchanged: a restart inside
the proving period is still a failure, and nothing forgives a watchdog.

### The rails

1. **Open before the first flash.** The journal says a session is open, under a kind of its own,
   before the scanner image is flashed. It says **closed** when the golden image is back and
   proved, and at no other moment — `restored` is the one stage that means closed, and `stuck` does
   not.
2. **An open session with nobody running it is ended by the next wake.** After a restart, a crash,
   the bound on a wake or a power cut, **step 3** of the next wake takes it up (`docs/cycle.md`),
   after any radio update the journal says is in progress: the golden image is flashed back and
   proved, and the journal says closed. It needs no server, no uplink and no console.
3. **Twenty minutes as a scanner at most** (`kSessionCeilingMs`), counted from the moment the
   scanner is found. Past it the session goes to the switch back, whatever step it is in and
   whatever its lines are waiting for — so neither a program of six steps asking for 600 s nor a
   caller that sits on a step's lines can hold the radio off the mesh longer than that.
4. **Not begun unless it can be ended.** The check holds everything the switch to the scanner
   checks *and everything the switch back will*: a scanner image that verifies and is the golden
   image's board and band, a golden image that verifies, the battery's two figures, the link up,
   this board's clock set, no radio update in progress, this firmware confirmed and not on trial.
   Each refusal is one word, and nothing is touched when one comes.
5. **A switch back that is refused or fails leaves the session open**, and every wake tries again.
   One journal line a try.
6. **Begun by a person or a job, never by this firmware.** `rak session <seconds> <command>` and
   task p16-04's job are the only two callers of `start()`, and rail R20 of
   `tools/check_rails.py` holds it there. What this firmware does by itself is **end** one.
7. **One a wake, and it is that wake's update** (S-7): no other update step runs in a wake that
   ran a session, or the other way round.

### The refusals, one word each

| Word | Means |
|---|---|
| `noScanner` | no scanner image in the store, one that does not verify, one on the bad list, one that is not a scanner or not this board's — **or one of another band than the golden image's** |
| `noGolden` | no golden image to come back to, or one that does not verify |
| `battery` | the battery could not be read, is below `kBatteryMinMv`, or is falling faster than `kBatteryFallingMvPerHour` |
| `link` | the radio does not answer a `HELLO`, and there is no classified silence that is passed through here: a session is somebody's choice, never the one flash that exists to unbrick a radio |
| `clock` | this board does not know what time it is, so the greeting would give the scanner no epoch and its lines would be stamped `ts: 0` |
| `inProgress` | a radio update is journalled unfinished, this firmware is on trial or unconfirmed (S-7), or a session is open already — an open one is **ended**, never begun again |
| `program` | the program breaks one of the rules below |
| `noMemory` | neither room could be had: no external room, and the internal heap has no 8 192-byte block for a step's lines |
| `journal` | the journal would not take the entry that opens a session. Not one of the eight the task named, and `tasks/phase-16/reports/03.md` says why it is here |

### A step, and what ends it

A step is a command and its seconds, and the rules are held in one place
(`core::update::judge()`), so a person at a prompt and a payload from the server are refused in
the same words: **1 to 6 steps**; a command of **1 to 40 characters** of `a`–`z`, `0`–`9`, space
and the full stop whose **first word** is one of `stat`, `sweep`, `scope`, `hist`, `dwell`, `cad`;
**5 to 300 seconds** a step and **600 at most** over the program.

Once a scanner is found the greeting is said — `time <epoch>`, `auto 0`, `info` — with the room
already lent, so its three `ack`s and its `info` line are laid down in it and are handed over
**with the first step's lines**: the session's lines begin with them.

Then, per step, in **two** calls of the machine (task p18-03):

1. the step's journal entry is written — the stage `step`, which step it is, and the command — and
   the machine answers *changed* with **nothing said to the scanner**. `listening()` is false and
   `stepPending()` says how many seconds the step about to begin was given. This is the moment
   whoever drives the machine has to tell a server what is about to happen and to see to its
   lease, with the scanner still silent and the step's seconds not begun;
2. the next call says the command (`spec/LINK-PROTOCOL.md` §9). From here `listening()` is true,
   the step's seconds run, and lines are kept whole in the room until one of five things, which is
   how the step **ended**:

What the scanner said between steps is thrown away.

| Ended | Means |
|---|---|
| `done` | a `done` line for the command's first word |
| `clock` | its seconds are up (or the ceiling came), and `stop` is said |
| `full` | the room is used up, and `stop` is said |
| `refused` | the `ack` said not ok. **The next step still runs** |
| `silent` | no `ack` came |

The step's lines are then handed to whoever runs the session — the console prints each one whole,
task p16-04's job posts them — and the next step begins when that returns.

**While a step runs, the port is the only thing being listened to.** `listening()` says so, and
whoever drives the machine makes no call of its own between the command and the end — not a
progress post, not a lease, not a line (*A session as a job* below, and rail **R25**). The reason
is 21 November: the fifth step of the first session a unit ever ran was
`hist 869.418 869.818 25`, the scanner numbered its sixteen lines `seq` 245 to 260 and its `done`
261, and meshmaster.dk holds 245 to 251 and 261. Nine measurements are not anywhere. The job had
written its *step 5* line and called `progress` the moment the step began — a second or more over
LTE‑M, with the scanner already speaking — and the driver's receive ring was 1 024 bytes, which is
seven of those lines.

**The receive ring follows from the baud rate**, in `core/link/client/port.h` and not at the
driver, because what sizes it is the loop and not the port: 115 200 baud 8N1 carries 11 520 bytes
a second, the reader is away for at most `kListenIdleMs` (100 ms) plus what the loop does between
two reads (50 ms), and that is 1 728 bytes. The ring is **4 096** bytes — 355 ms of this line,
better than twice the absence, sixteen frames of §2 — held by a `static_assert` beside the
numbers, and `kListenIdleMs` is held equal to the figure the arithmetic used. It costs 3 072 bytes
more of the internal heap than the 1 024 it replaced, held for the life of the firmware; the step
room on this board is 64 kB of PSRAM and not the heap's, so that is what pays for it.

**And a loss is counted and said.** The driver is installed with an event queue from this task on
and `read()` drains it: `UART_BUFFER_FULL` is the ring full and `UART_FIFO_OVF` the hardware's own
128‑byte FIFO over, and after either the bytes behind are gone. Neither event carries a size —
`uart_event_t::size` is *UART data size for UART_DATA event* — so what is counted is **the moment
and not the bytes**: what the unit at Bellevue was missing was not a number, it was any word at
all that something had been lost. The count reaches

- `interfaces::ByteStream::losses()`, and through the port the link client's `port_losses`, beside
  `text_truncated` and `text_dropped` — the three ways a line of §9 does not reach whoever wanted
  it, and the only one of them that takes a line whole and silently. `rak text` prints it;
- **the step**: a step that lost anything writes a **second** journal entry at its end — which
  step, how it ended, and the count — and that line goes to the server with the result and is
  printed by `journal`, by `rak session <seconds> <command>` beside the step's ending, and counted
  by `agent jobs`. A step that lost nothing writes no such entry, so an ordinary session's journal
  is exactly what it always was.

### Its room, and what it cost

One buffer, taken from `interfaces::Scratch` when the session begins and given back when the steps
are over. No static storage of that size: every byte of `.bss` moves the heap the main task's stack
comes out of (`tools/heap_at_start.py`, rail R22). A session that cannot have a room at all is
refused `noMemory` before anything is touched.

**It comes from one of two places, and `takeStepRoom()` is the one function that decides** (and the
one `heap room` on the console goes through, so that what a person proves on a cable is byte for
byte what a session gets):

| | bytes | when |
|---|---|---|
| the module's PSRAM | **65 536** (`kStepBytesExternal`) | there is PSRAM, it comes up, and it holds what is written to it |
| the internal heap | **8 192** (`kStepBytes`) | there is none, it does not come up, or it does not hold what is written to it |

**Asking for the external room is what brings the PSRAM up** — nothing else in this firmware ever
does, and an ordinary wake does not (`ARCHITECTURE.md` §14, *The PSRAM, and its one job*; rail
R24). Then, **before the first line goes in**, the whole room is written with a pattern of each
word's own index and read back (`patternHolds()`): a value derived from the index, so that two
addresses which answer for each other — the commonest way a PSRAM is wrong — are caught rather
than read back as each other's value. A room that fails is **given back**, and the session takes
the 8 192 bytes instead. Nothing is kept in the room over a sleep, because a sleep is a boot; and
nothing that runs with the flash cache off, nor any interrupt handler, holds a pointer into it —
the step's lines are read where they lie, by the task that printed them or posted them.

Why 64 kB and no more, with 2 MB to choose from: what bounds the room is what leaves the board. Six
steps of it are 384 kB, inside the 512 kB meshmaster.dk keeps of one session, and every 8 kB of it
is a post over LTE-M. It is also what a minute of `scope` costs — a frame a second at 250 points is
about 30 kB — so a minute fits whole.

Why 8 192 and no more for the small room: the unit in service has said what it has — its report's
`restarts` section, in November, *heap: 27472 bytes free, largest free block 12288, least free
since this boot 25300 (internal, 8-bit)*. A room of 16 kB is not there to be had, and one of 12 kB
would leave nothing whole for anybody else. A minute of `scope` then ends `full` after about
thirteen frames, with every line it kept whole.

And **measured at every session**, through the seam `heap` already uses
(`core/console/heap_report.h`): the free bytes and the largest free block just before the room is
taken; the largest free block just after; and the least free since boot, read when the steps are
over. One journal line, `room`, written **before the switch back**, says them, and `rak session`
and `heap room` print the same line, in one of two shapes:

```
room: 65536 psram of 2097152; internal 27472, block 12288; least 25300
room: 8192 of 27472, block 12288; left block 4096; least free 17108
```

The first says what the room is, where it came from, and what the **internal** heap had while it
was out — which is the figure that says whether anything else on this board was left short. A build
that was handed no heap seam says so in the line instead. **The figures decide nothing**: nothing
here refuses anything because of them, and they exist so that either number can be raised later
from what units say. An entry written before task p17-02 has a 20-byte body and reads back with the
PSRAM's size nought, which is the small room — which is what that line always meant.

### What it costs, and what a person sees

Two flashes of about a quarter of an hour (`kFlashWindowMs`) with the program and three minutes of
proving between them: **about forty minutes off the mesh**, with this board awake for all of it.
The wake that takes a session up is held open for two passes of the machine above and the ceiling
— `kSessionBoundMs`, 60 min 35 s since task p18-04 added the watchdog's wait to it — which is
the number that set `kRaisedBoundMax` in
`core/cycle/wake_guard.h`, and a raised bound is still a bound.

A scanner **nobody opened a session for is left alone**, as it always was: a person typed `rak role
scanner` and means it. `rak watch` says which of the two it sees — *in a session, since …* or
*running the scanner, left alone* (`docs/supervisor.md`).

### A session as a job (task p16-04)

`twinoak.noisescope.session` is the same machine begun by the server instead of by a person at a
port, which is what a unit on a mast has. **Nothing of the session is decided in the job**: the
program comes out of the payload and is judged by the rules above (`core::update::judge()`, so a
prompt and a payload are refused in the same words), `start()` runs the whole check, and the job
drives `step()` in slices inside the wake exactly as an apply drives its machine.

What the job adds is the three things a mast needs and a cable does not:

- **Nothing goes out between a step's command and its end** (task p18-03). The job writes its
  *step N* line and posts it in the moment the machine gives it — after the journal entry, before
  the command — and from the command until the step has ended it makes **no** call: no progress,
  no lease, no lines. The rule is held in two places: the loop's shape, and a guard at each of the
  three methods that can reach the server while a session runs (`extendLease`,
  `postApplyProgress`, `postSomeLines`), which refuses such a call and counts it in
  `calls_in_step` rather than making it. Rail **R25**, and the fixture of every session test fails
  a case in which a request of the API arrives while the machine is listening.
- **The lines go to the server, step by step.** When a step hands its lines over they are posted
  before the next step begins: `POST /agent/v1/jobs/{id}/lines/{from}`, the body the lines as the
  scanner sent them — each ended with `\n` — in bodies of at most **8 192 bytes cut at a line's
  end**, with `{from}` counting the lines handed to posts so far. A post nothing answers is sent
  once more with the same `{from}` and then **counted as unanswered** and left: *they may have
  arrived*, and the radio is never held for a server. A reply that says `full` — the job's room on
  the server is used up — stops the posts, and the remaining steps still run. It is a call of its
  own in the failed-call list and in what a wake was doing (`agent calls`, *scanner lines*).
- **The lease, and the journal as progress.** Extended before the first byte, for the server's
  whole ceiling before **each of the two flashes** (`kLeaseFlashSeconds`, as an apply's flash is)
  and every five minutes otherwise — but **never inside a step**: before a step's command the
  lease must cover the step's seconds, the posting of its lines and the calls that follow
  (`kLeaseAfterStepMs`, three minutes), and it is extended then if it does not. A step of the
  longest kind and that margin are 480 seconds, inside the 900 one ordinary extension gives, which
  is why the five-minute timer never has to fire in the middle of one. One
  `POST /jobs/{id}/progress` per journal line, so the job's
  page says *flashing-scanner since 10:00* rather than nothing for a quarter of an hour. The
  `room` line is one of those entries, so the room's figures reach the server with **every**
  session.
- **A result that outlives a restart.** Before the scanner is flashed the job is journalled as
  **owed** (`kind::kAgentJob`, as this firmware's own apply is). The result says `restored`,
  `refused` with one of the words above, or `stuck`, with a row for every step of the payload —
  how it ended, and what it handed to its posts — the two counts, and the session's journal. A
  board that restarts in the middle leaves the session open; the next wake ends it (rail 2) and
  then posts `restored` or `stuck` from the journal it has, with `notRun` for every step it cannot
  vouch for and counts of nought.

**One attempt, leased an hour from the claim**, and the wake that runs a session is held open for
its whole bound — which is why the backend exchange's own bound is the session's and a minute
(`docs/cycle.md`). A session claimed while an update is in progress or while this firmware is on
trial is `refused` with `inProgress`, and S-7's one-update-step rule counts a session as the
wake's update step.

**S-12.** A scanner's line goes into a post and nowhere else: not the log, not the journal, not a
report, not the job's own `log`. The room is read where it lies and nothing of it is copied by
this firmware — the same rail `rak session`'s printing is held to, and a test searches all five
places for a line only the scanner would have said.

## What is not here (the radio's half)

Telling a scanner what to measure beyond the six words of a step's first column, a session
that spans a sleep, and a faster flash. Deciding *when* to update (phase 3's wake cycle; later the server), fetching a candidate (phase
3's uplink; today `tools/walterctl.py push --as candidate`), and the supervisor that calls `flash
the golden image` as a rung of its ladder (task p2-05). The Walter's own update follows.

---

# The Walter's half: updating the Walter itself

`spec/SYSTEM.md` S-6: "The Walter updates itself A/B with rollback, and a new image is confirmed
only after it has booted, spoken to the RAK and reached the server. A Walter that cannot do all
three goes back." Task p2-04 built it; the code is `core/update/self_update.h`, the platform's
part `platform/own_update.cpp`, the tests `host_test/core/update/test_self_update.cpp` and
`host_test/core/console/test_console_self.cpp`.

## The idea in one paragraph

The Walter has two application slots, `ota_0` and `ota_1` (`partitions.csv`), and ESP-IDF's
bootloader with **rollback enabled**. A new image of this firmware is received into the other slot
through the **same six checks** a radio image passes (`spec/FIRMWARE-MANIFEST.md` §5, with the
descriptor's role `manager` and board `walter`), is made the next to boot, the radio is told, and
the Walter restarts. The new image then has **one boot** - the bootloader marks its slot
*pending-verify* and will abort it at the next reset unless it has confirmed itself - to prove it
can do the unit's job: reach its main loop with the board in the safe state, exchange `HELLO` with
the radio, and complete one exchange with the backend over the uplink. All three inside the **trial
window** and it writes itself *valid*; otherwise it writes itself *invalid* and restarts, and the
bootloader runs the image that worked. Every step is journalled before it is done, so the image
that boots next - whichever of the two it is - reads what was in the air and closes the story:
why it was rolled back, and the new image's hash on the bad list so it is never tried again.

```
self receive ─► received ─► self switch ─► switching ─► restart ─► on trial ─► confirmed
  (the six checks,            (journal, the boot      (the bootloader:        (all three
   into the other slot)        pointer, SLEEP_NOTICE)  new -> pending-verify)  conditions)
                                                                      │
                     the window passes · a condition fails · any reset · self reject
                                                                      ▼
                                                               rolled back
                                      (the hash on the bad list; invalid; restart into the old image)
```

## The trial: three conditions, one boot

| | Condition | Met when | Not applicable when |
|---|---|---|---|
| 1 | **the main loop**, with the board in the safe state | the console's first `tick()` - the main loop turning after `RadioPins` put the seven pins where they belong | never |
| 2 | **`HELLO` exchanged with the radio** | the radio answers `HELLO` (or shows itself as a scanner, task p1-02's text mode) | the radio was **known down before the update**: the switch asked `HELLO` and recorded the answer in its journal entry; a caller that has classified the radio as down - the supervisor (task p2-05), or `self switch radio-down` on the console - says so. A dead radio does not condemn a good Walter image; a radio that answered before the update and is silent after it does. The classifications that count are `core::supervisor::radioKnownDown`: `silent`, `scanner` and `awaiting its first install` - the last because a slot with a bootloader and no application cannot answer until it is flashed, and **not** `unknown`, because a wake that has classified nothing waives nothing and the switch is refused instead |
| 3 | **one exchange with the backend** over the uplink | `UplinkCheck::check()` says *reached* | **no server is configured**: the check says *not configured*. Until phase 3 there is no modem and no server, so this is every unit's state, and the trial confirms on conditions 1 and 2 |

The conditions are tried in order: `HELLO` every `kTrialHelloIntervalMs` once the loop turns; the
uplink once the radio is settled, again every `kTrialUplinkRetryMs` after a failure. All three
settled → `esp_ota_mark_app_valid_cancel_rollback()`, the journal says *confirmed*. The window
passes → the **first missing condition names the rollback**: *the main loop was not reached inside
the trial window*, *the radio answered before the update and not after it*, *the backend was not
reached inside the trial window*; the hash goes on the bad list, the journal says *rolled back*,
and `esp_ota_mark_app_invalid_rollback_and_reboot()` restarts into the previous image.

### A trial exists to ask one question, and nothing may answer it for the trial

With the agent built, condition 3 is **one `POST /agent/v1/heartbeat` answered `ok`**, posted by
the wake and by nothing else (`walter_agent/agent_uplink_check.h`). Three rules, all of them
task p14-05's and all of them written on 15 November 2026, when v0.1.6 rolled itself back for a
server it had never asked:

- **A trial boot owes no wait.** A failed call leaves the agent's library a hold — thirty
  seconds, then more — and the hold is saved across a sleep and put back at the next boot less
  the time the board was away, which is 0 for every boot that is not a timer wake. A restart into
  a trial is not a timer wake, so the whole hold used to come back and the trial's wake sent
  nothing. The wait is now **laid aside** before the state is handed to the library
  (`components/agent/agent_backend.cpp`, `loadState`), and nothing else of the state moves. The
  boot of the **old image after a rollback** lays it aside too while it still owes the server the
  apply's result: that result goes in the first wake that can.
- **A heartbeat that fails is tried again while the window lasts.** The trial's own loop asks the
  backend seam for one more heartbeat — `interfaces::Beat`, and nothing else of a wake — each
  time the library's own wait has run out (`core/cycle/wake_cycle.cpp`, `trialBeat`). One slow
  attach is not an image that cannot reach the server. The library still decides *when*: a wake
  inside its own wait answers `waiting` and sends nothing.
- **Not reached and not asked are two findings.** The image that rejects itself for want of an
  uplink writes a `kind::kTrialUplink` entry beside its *rolled back* entry: how many heartbeats
  went out inside the window and what the last came to. `self status` prints it, and so does the
  apply's result on meshmaster.dk — *3 heartbeat(s) tried in 300 s; the last: the server was not
  reached*, or *no heartbeat was sent in the 300 s the window lasted*.

**A trial is one boot.** The bootloader aborts any slot still *pending-verify* when it runs
(`bootloader_utility.c`, ESP-IDF v5.5.5), so a reset during the trial - a panic, a watchdog, a
power cut, a person's finger - is a rollback whatever its reason. The old image boots, finds *on
trial* (or *switching*, when the new image never got as far as journalling its trial) as the last
self-update entry, and writes *rolled back: the image restarted during its trial* (or *never
reached its main loop*), the hash recomputed from the other slot's bytes and put on the bad list.
One more case is told apart: *switching* with the other slot neither aborted nor invalid means the
restart came between the journal entry and the boot pointer - *the restart came before the other
slot was made the next to boot: nothing was tried* - and nothing goes on the bad list; `self
switch` may be said again.

## The uplink condition, and the server it reaches

Condition 3 is an interface, `core/update/uplink_check.h`, with three implementations: the fake in
the tests (reached, failed, or not configured, as a test says), `UnconfiguredUplink`, and - in the
firmware since task p3-02 - `TlsHandshakeCheck` (`core/update/tls_check.h`). What the condition
**is** is named by `kUplinkCondition`: **reach the configured server's TLS port and complete a
handshake with its chain verified against its root and the host name checked** - the strongest
statement available without an agent (`spec/SYSTEM.md` §8: nothing talks to MeshMaster yet).

**The server is the `server.url` setting** (task p3-03, `docs/console.md`): the console hands the
host and the port over at boot and at every change, an empty one is *not configured* and the
condition does not apply, and a URL with no host this firmware can reach - a port over 65535 -
is the same thing. That rule is what keeps a factory image, which has no server, from rejecting
its own first update. A build that names a host it has no way to reach - a non-empty host with no
modem driver - answers *failed*, so that it rolls its update back rather than confirming it blind.

**The rule that keeps a factory image from rejecting itself**, stated: *a trial runs only for an
image that an update made the next to boot, and condition 3 applies only when a server is
configured.* A factory image is flashed over USB with blank OTA data; at its first boot the
bootloader gives `ota_0` an entry of its own in the state **valid** (`set_actual_ota_seq`), never
*new*, which is the only state it turns into *pending-verify*. So no trial runs, nothing is
journalled, nothing is rejected - and had one run, an unconfigured uplink would have made the third
condition not applicable. Both halves are tested (`test_self_update.cpp`, "a factory image's first
boot" and "no server configured").

## The preconditions

Each a named refusal, printed as `error: self receive: <reason>` or `error: self switch: <reason>`;
nothing is done.

| Refusal | `self receive` | `self switch` | Means |
|---|:-:|:-:|---|
| *the running image is itself on trial: never stack an update on an unconfirmed one* | ✔ | ✔ | the running slot is *pending-verify*, or this boot is a trial |
| *the journal says a radio update is in progress: never both at once (S-7)* | ✔ | ✔ | a radio-update entry that is not terminal |
| *a transfer into the other slot is open* | ✔ | ✔ | |
| *the battery could not be read* / *is below the update threshold* / *is falling fast* | | ✔ | `kBatteryMinMv`, `kBatteryFallingMvPerHour` (the radio's update's). The write itself is harmless if interrupted, so `self receive` does not ask; a brown-out during the first boot wastes the one attempt, so `self switch` does |
| *no image has been received into the other slot* | | ✔ | no *received* entry that nothing has consumed |
| *the other slot does not hold the image that was received* | | ✔ | the slot's bytes hashed again do not match the entry |
| *the radio does not answer HELLO and nobody has said why* | | ✔ | `self switch radio-down` says so, and condition 2 is then not applicable |
| *the platform cannot say which application slot is running* | ✔ | ✔ | no OTA data partition, or a table without two slots |

And what an image is refused for **at the door**, after checks 1, 2 and 3 of `FIRMWARE-MANIFEST`
§5 and before the other slot is erased: *this image failed its trial once and is never tried again*
(the bad list, shared with the radio's candidates), *the file is larger than the other application
slot*, *the file's load_address is not an application slot's offset* (the release writes `ota_0`'s;
either slot's is right, the ESP32-S3 maps whichever the bootloader chose), and the link-major rule
below. A radio image offered as the Walter's fails check 3 (no image with role `manager`, board
`walter`, band `lte-m`); checks 4 and 5 run over the bytes **as they lie in the other slot**, and
`esp_ota_end` checks they are an application image at all.

## The order of a link-version change (S-7)

"When the link's major version changes, the Walter goes first and speaks both majors until the RAK
has followed." With the manifest's fields (`spec/FIRMWARE-MANIFEST.md` §2): the manifest's
**`link.major`** is the major the image speaks - its descriptor's, which check 5 holds it to - and
the image's **`requires.link_major`** is what must already be there: the radio's major. So:

- an image whose `link.major` is the radio's current major is let in;
- an image whose `link.major` is **another** major is let in **only if** its `requires.link_major`
  names the radio's current one - that is how "speaks both" is said: it speaks the new major and
  requires, hence tolerates, the old;
- an image that speaks another major and says nothing of the radio's is refused: *the image speaks
  another link major than the radio and does not say it tolerates the radio's*.

The radio's current major is the session's when `HELLO` has been answered, else the major this
build speaks (the one the radio was last proved against). The radio's own update then follows:
its manifest's `link.major` must be one the Walter speaks (`core/image/choose.cpp`, task p2-01).
The order is enforced here, not left to whoever starts the update.

## The journal

The same ring as the radio's (`core/update/journal.h`), kind *self-update*, 48-byte payloads: the
state, the new image's slot, the reason, flags, the image's size, the first sixteen bytes of its
SHA-256 (enough to know it again; the full hash for the bad list is recomputed from the slot's
bytes and held to these), and the versions switched *from* and *to* as `X.Y.Z`. The states, which
the radio's update also reads for S-7:

| Entry | Written when | Says |
|---|---|---|
| **received** | `self receive` passed the six checks | *received 0.4.0 into slot 1 (12288 bytes, sha256 …)* |
| **switching** | `self switch`, before the boot pointer moves | *switching to 0.4.0, from 0.3.0 (the radio answered)* - or *(the radio known down)* |
| **on trial** | the new image's `begin()`, before its first condition | *on trial: 0.4.0 in slot 1* |
| **confirmed** | all three conditions, or `self confirm` | *confirmed: all three conditions* - *; the radio was known down* / *; no server configured* when one did not apply |
| **rolled back** | the trial image's rejection, or the old image's finding at boot | *rolled back: <reason>* - one of the three conditions, *restarted during its trial*, *never reached its main loop*, *the restart came before the other slot was made the next to boot*, *rejected on the console*, *the platform refused* |

*switching* and *on trial* are "unconfirmed" to the radio's update (`rak update` is refused with
*the Walter's own update is unconfirmed: never both at once*).

## After a power cut

| Last entry | This boot is | What `begin()` does |
|---|---|---|
| **received** | the old image, nothing in the air | nothing; `self switch` takes the received image |
| **switching**, the other slot *aborted* or *invalid* | the old image: the new one was booted and never journalled | *rolled back: never reached its main loop*; the hash on the bad list |
| **switching**, the other slot neither | the old image: the restart came before the boot pointer | *rolled back: the restart came before …: nothing was tried*; nothing on the bad list; the received image still there |
| **switching**, this slot the one named | the new image, *pending-verify* | the trial |
| **on trial**, this slot the one named | the new image again - which the bootloader does not do (pending → aborted); seen only if rollback were off | the trial, again |
| **on trial**, the other slot | the old image: the trial was cut short | *rolled back: restarted during its trial*; the hash on the bad list |
| **confirmed**, **rolled back** | as the word says | nothing |

### What killed it (task p11-03)

A *rolled back* entry the old image wrote at boot says **that** the bootloader gave up on the new
image. Beside it goes a second entry - `kind::kTrialEvidence`, 48 bytes like every entry - that
says **why**, and it can only be written at that boot:

| | |
|---|---|
| the reset reason | `esp_reset_reason()` of this boot, which *is* the reset that ended the trial. On 9 November 2026 it was `panic`. The next wake's reset reason says nothing about the trial, which is why this is recorded once and read back from the journal afterwards |
| the core dump | the `coredump` partition (`0xFF0000`, 64 KB; `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`), read by `core/health/coredump.h`: its state, and - **only when its checksum holds and its application ELF SHA-256 is the trial image's own**, as the other slot's `esp_app_desc_t` carries it - its panic line and the name of the task that crashed. A dump from any other image is never attributed to the trial, and a switch that was never taken records nothing at all: that image was never started |

The entry's copy of the panic line is **cut to 31 characters** and a flag says so - 48 bytes do not
hold a 69-character line beside a reset reason and a task name - so the whole line is read back out
of the dump whenever it is still there and still that image's. **Nothing erases a dump by itself**;
`coredump erase confirm` on the console is the only thing that does, because the dump of 9 November
was readable only because nothing had.

It goes three places: `self status` and the rollback's boot line (`docs/console.md`), the apply's
result the server is sent (`reason` and `lastTrial`), and `twinoak.fw.state`'s `lastTrial`. The
reason the server shows reads, for that trial:

```
rolled back: the image never reached its main loop (the bootloader gave up on it)
 - the reset that ended it: panic
 - panic: assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)
 - in task ipc0
```

(one line; broken here to fit). `coredump` on the console says the same about the partition itself,
and names which of the two slots holds the image that wrote it.

### How hard the backend was asked (task p14-05)

A rollback by *the backend was not reached inside the trial window* gets a second entry of its
own in the same way - `kind::kTrialUplink`, written by the image that **rejected itself**, right
after its *rolled back* entry and read back under the same rule (the last trial's when no
self-update entry is newer than it). It carries three figures: how many heartbeats went out
inside the window, how long the window had lasted, and the verdict of the last one that went.

It is there because *not reached* and *not asked* are two findings and read the same without it.
On 15 November 2026 the server was told *the backend was not reached inside the trial window*
about a unit whose trial never sent a heartbeat at all, and the image was bad-listed for it. The
same rollback now reads, on `self status` and in the apply's result:

```
rolled back: the backend was not reached inside the trial window
 - 3 heartbeat(s) tried in 300 s; the last: the server was not reached
```

or, when nothing went out, `no heartbeat was sent in the 300 s the window lasted`.

## The console

```
> self receive
self receive: this Walter's other application slot (1); this port is now a framed binary transfer (protocol 2)
  (COBS and CRC-16, the box link's own framing). Send manifest.json, then its .sig,
  then the ota file: tools/walterctl.py push --as self
  what will be accepted: role manager, board walter, band lte-m, an ota file that fits the slot,
  and all six checks of FIRMWARE-MANIFEST section 5 run here - the laptop is not trusted
self receive: stored
  52 chunks, 0 sent twice, 0 out of order, 0 frames dropped
  'self status' shows it; 'self switch' makes it the next to boot and restarts
> self switch
self switch: making 0.4.0 (slot 1) the next to boot, from 0.3.0+abc1234; the radio is told and the Walter restarts
```

Then, after the restart, among the boot lines:

```
boot 7: software reset, the Walter's own update, switching to the other slot
self-update: this image (0.4.0, slot 1) is on trial: 300 s to reach the main loop, HELLO the radio
  and reach the backend, or it is rolled back ('self status' watches, 'self confirm' / 'self reject' end it)
...
self-update: on trial -> confirmed: all three conditions hold: confirmed
```

or, for a trial that fails, `self-update: on trial -> rolled back: <reason>` and `restarting into
the previous image`; and after that restart the old image's boot lines say `self-update: rolled
back: <reason>` only when it is the old image that has to write the entry (a reset during the
trial), because a trial image that rejected itself has closed the story already.

- `self status`: `running: slot 1 at 0x310000 (3072 KB): valid (confirmed), version 0.4.0+w1`,
  `other: slot 0 …`, then `on trial for 42 s of 300: main loop met; radio met (1 HELLO); uplink
  failed, trying again (2 sent, the last: the server was not reached)` while one runs — `nothing
  sent yet` where no heartbeat has gone out at all — or `the running image is confirmed` (or
  *pending-verify*, *aborted* … in the bootloader's words), then `received: 0.4.1 in slot 0 (…),
  waiting for 'self switch'` when one is, then the last self-update's story, oldest entry first.
- `self switch [radio-down]`: the preconditions, the entry, the boot pointer, `SLEEP_NOTICE`
  (`kSwitchExpectBackS`), the restart; it does not come back. `radio-down` says the radio is known
  to be down, so the trial's second condition does not apply.
- `self confirm`, `self reject`: end a trial early, for the bench. Both refused when the running
  image is not on trial.

`tools/walterctl.py push --port COM7 --as self <release-dir>` is the laptop's end: it takes the
manifest's `ota` file and says `self receive`.

## The numbers - to be set on the bench

| Constant | Today | What it is |
|---|---:|---|
| `kTrialWindowMs` | 300 000 ms | from the trial image's main loop until all three conditions must hold; long enough for a cold LTE attach, short enough that a bad image is gone before the wake is over |
| `kTrialHelloIntervalMs` | 2 000 ms | between `HELLO` tries during the trial |
| `kTrialUplinkRetryMs` | 30 000 ms | between uplink attempts after a failure, and how often the trial asks the backend seam whether it is ready for another heartbeat. A look that finds nothing sent costs nothing and is neither paced nor counted |
| `kSwitchExpectBackS` | 60 s | what `SLEEP_NOTICE` tells the radio before the restart |
| `kUplinkCondition` | TLS handshake | what condition 3 is when a server exists (`uplink_check.h`); phase 3 implements it |
| `kSelfUpdateServerHost` / `Port` | "" / 443 | the server to reach; empty = not configured, condition 3 not applicable |

The battery thresholds are the radio's update's (`kBatteryMinMv`, `kBatteryFallingMvPerHour`).

## What a download sends (both halves)

Both halves fetch through the same three steps (`core/update/fetch.h`), so the headers are the same
whichever box the image is for. Each request carries **four at most**
(`interfaces::kMaxRequestHeaders`), and `core/net/client.h` adds the first of them itself to every
request this firmware makes:

| Header | On | What it is |
|---|---|---|
| `X-Agent-Version` | the manifest, its signature, **every range** | this firmware's own version (`core::version()`). **meshmaster.dk refuses an `/agent/v1` request that does not carry it with a 400** (`AgentV1.RequireVersionHeaderAsync`), and until v0.1.3 the download sent none, so no release of either box could be fetched over LTE: job 22841, 8 November 2026 ([`flashing.md`](flashing.md), the correction in the LTE walk). It is the client's own now, so no path above it can forget it; a caller that names the header itself keeps its own value (the agent's uplink) |
| `Authorization: Bearer …` | every request to `server.url` | `server.token`, read for the call and wiped; dropped at a redirect that leaves the host. The console's `fetch <base-url>` of a release on github.com carries none |
| `Range: bytes=a-b` | every range | 64 KB at a time (`core/net/download.h`) |
| `If-Range` | every range after the first | the `ETag` (or `Last-Modified`) the first range answered, so a file replaced under the same URL restarts the download rather than splicing |

`User-Agent` (the product and the version) and `Host`, `Accept` and `Connection` are the request
writer's on every request, this firmware's and anybody's (`core/net/http1.h`). Nothing else goes,
and nothing of a response is believed before the six checks.

**A request the server refuses says what it was**: the status, the file, and - when the server
answers RFC 7807 problem details - its `detail`, cut to fit and one printable line:
*http 400 for manifest.json: Every request carries X-Agent-Version: the agent's build.* That
sentence is what a `twinoak.fw.offer` posts as its `reason`, what the console's `fetch` and
`http get` print and what the job's log carries. A `detail` that holds the request's own token is
dropped whole rather than trimmed.

## Which slot a download fills (task p16-02)

A radio's download goes into one of the slot's **two downloadable images**, and the caller says
which. The golden image is reached by a promotion and never by a fetch, and the base is `rak
provision`'s.

| who asks | how it says which |
|---|---|
| the console | `fetch [base-url] [candidate\|scanner]` — the word after the URL ([`console.md`](console.md)) |
| meshmaster.dk | `twinoak.fw.offer`'s `into`: left out or `candidate`, or `scanner` |

The *role* follows the slot and nothing else: a fetch into the candidate chooses the manifest's
image whose role is `repeater`, and one into the scanner slot the image whose role is `scanner`,
each for this board and the band the slot already holds (`core/image/receive.cpp`). So a release
with no scanner image for this board and band is refused at **check 3**, with the reason, and
nothing is stored. A word in `into` that is neither, and `scanner` offered for the Walter's own
product — which has no scanner slot — are answered `stored: false` with a reason before a single
request goes out.

Both slots keep a download's progress the same way, so a wake whose budget ends a scanner
image's download posts no result, leaves the bytes and the store's progress record, has the next
sleep shortened in `plenty` and is taken up again by the next wake's claim of the same job
([`cycle.md`](cycle.md)). And a slot that **already holds the offered file**, its bytes read back
and hashed, is answered `stored` with no range asked for at all: the server's `expectedSha256`
and the store's own record settle it, and on a metered link that is the difference between a few
hundred kilobytes and none.

## What is not here

Downloading the image (phase 3's uplink; today `walterctl.py push --as self`), deciding when to
switch (the wake cycle; the server), the modem's own firmware (the library can; nothing here does),
and the TLS handshake itself (phase 3's HTTPS client fills `UplinkCheck`).
