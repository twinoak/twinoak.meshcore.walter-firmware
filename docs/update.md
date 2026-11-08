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
| **node-fault** | the image proved everything an image can and **the node** is the fault: today that is one thing, a radio that has lost its identity (check 1a below). No rollback is spent, nothing goes on the bad list, nothing is promoted - the candidate is on the radio and in the store, the golden image is untouched, and the way out is a person's `erase` and `reboot` at the radio. The same candidate may then be applied again | terminal |

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
| `kBatteryMinMv` | 3 700 mV | the battery below which no update starts (a single Li-ion cell: more than half its charge) |
| `kBatteryFallingMvPerHour` | -100 mV/h | a trend steeper than this refuses the update (no trend exists until phase 3) |
| `kStepIdleMs` | 100 ms | what `rak update` sleeps between steps that only waited |
| `kEntriesPerRunMax` | 48 | more journal entries than this in one run means something is looping, and the machine stops writing |

## What is not here (the radio's half)

Deciding *when* to update (phase 3's wake cycle; later the server), fetching a candidate (phase
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
  failed, trying again (2 attempts)` while one runs, or `the running image is confirmed` (or
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
| `kTrialUplinkRetryMs` | 30 000 ms | between uplink attempts after a failure |
| `kSwitchExpectBackS` | 60 s | what `SLEEP_NOTICE` tells the radio before the restart |
| `kUplinkCondition` | TLS handshake | what condition 3 is when a server exists (`uplink_check.h`); phase 3 implements it |
| `kSelfUpdateServerHost` / `Port` | "" / 443 | the server to reach; empty = not configured, condition 3 not applicable |

The battery thresholds are the radio's update's (`kBatteryMinMv`, `kBatteryFallingMvPerHour`).

## What is not here

Downloading the image (phase 3's uplink; today `walterctl.py push --as self`), deciding when to
switch (the wake cycle; the server), the modem's own firmware (the library can; nothing here does),
and the TLS handshake itself (phase 3's HTTPS client fills `UplinkCheck`).
