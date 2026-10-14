# Updating the radio

How the Walter replaces the repeater's firmware without ever leaving the unit worse off than it
found it (`spec/SYSTEM.md` S-5 and §6, `docs/ARCHITECTURE.md` §7). Task p2-03 built it; the code
is `components/core/update/` and the tests `host_test/core/update/`. This file is what a person at
the console needs: the diagram, the health check, what each line printed means, and what state a
unit is in after a power cut at each step.

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
           │            │                 │          │
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
| **flashing-candidate** | the flasher (`docs/ARCHITECTURE.md` §5) writes the candidate into the application region, verifies it, writes the bootloader's settings record and resets the radio | the flash |
| **booting** | waiting for the radio to come up: an `EV_BOOT` or a `HELLO` answer, inside the boot window | the wait |
| **proving** | the health check, below | the check |
| **committing** | the store's promotion: the golden pointer moves to the candidate's area (seven flash operations, whatever the size) | the promotion |
| **done** | the candidate is the golden image; the candidate slot is empty | terminal |
| **rolling-back** | the candidate's hash is put on the bad list, then the golden image is flashed back and the radio reset | the rollback, with why and at which step of the check |
| **rolling-back (booting the golden image)** | as booting, for the golden image | the wait |
| **rolling-back (proving the golden image)** | the same health check, held against the golden image's own descriptor | the check |
| **restored** | the golden image is back, proved and still golden; the candidate is in the store but on the bad list | terminal |
| **stuck** | the golden image did not come back either, or there was none (a first install that failed): the radio has no proved application. The supervisor's last rung, a report, and no further attempt by this machine | terminal |
| **refused** | a precondition failed at check; nothing was touched | terminal |

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
| *the link is down and nobody has said why* | `HELLO` is not answered. The supervisor (task p2-05) may pass the update a link it has classified as down for a known reason; a first install implies it (a blank chip answers nothing) |
| *the battery could not be read* | `POWER_GET`'s battery channel did not read (a first install, with no radio to ask, is let through: a person at the console is the gauge) |
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
2. **The clock can be set**: `TIME_SET` accepted - the first thing a healthy repeater must take
   (§5.4). A Walter with no time to give (no modem until phase 3) cannot make this check; it says
   so (*the clock check was skipped*) and asks `TIME_GET` instead, which must answer.
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
though the role advertises it*, *the flasher did not finish*.

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
| **restored**, **stuck**, **refused** | as the word says | nothing: the run is over |

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
- `error: rak update: refused at check: <reason> (nothing was touched)` - a precondition.
- `error: rak update: an update is already running (<state>); 'rak update status' says where` - a
  resumed run is being driven from the main loop; wait for it.
- `update: stopped in <state>: the journal could not be written` - the machine does not act
  without its journal. Nothing more happens until the next boot reads what was written.
- `(the clock check was skipped: this Walter has no time to give until phase 3's modem)` after the
  outcome, when it was.

`rak update --first-install` is the same run with no golden image to return to - a person's
command only: a unit's first repeater image, loaded as a candidate over USB. If it fails, the
unit is *stuck* with *no image to return to*, and the candidate is on the bad list.

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
with a sentence beside it; none has been held against a real radio yet (`docs/bench.md` §8).

| Constant | Today | What it is |
|---|---:|---|
| `kBootWindowMs` | 20 000 ms | from the flasher's reset until the radio must have answered `HELLO` or sent `EV_BOOT` |
| `kBootHelloIntervalMs` | 2 000 ms | the pause between `HELLO` tries inside that window (one try is up to three seconds of §3's retries) |
| `kProvingPeriodMs` | 180 000 ms | how long the link must stay up after a good start, with no restart |
| `kProvingPingIntervalMs` | 3 000 ms | a `PING` this often through the proving period |
| `kScannerWindowMs` | 20 000 ms | how long after the reset NoiseScope must have shown itself |
| `kBatteryMinMv` | 3 700 mV | the battery below which no update starts (a single Li-ion cell: more than half its charge) |
| `kBatteryFallingMvPerHour` | -100 mV/h | a trend steeper than this refuses the update (no trend exists until phase 3) |
| `kStepIdleMs` | 100 ms | what `rak update` sleeps between steps that only waited |
| `kEntriesPerRunMax` | 48 | more journal entries than this in one run means something is looping, and the machine stops writing |

## What is not here

Deciding *when* to update (phase 3's wake cycle; later the server), fetching a candidate (phase
3's uplink; today `tools/walterctl.py push --as candidate`), the Walter's own update (task p2-04),
and the supervisor that calls `flash the golden image` as a rung of its ladder (task p2-05).
