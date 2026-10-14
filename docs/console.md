# The console

The Walter is looked at and exercised over the module's USB-C, which is the ESP32-S3's own USB
Serial/JTAG port: any baud (it is ignored), one command per line, a `>` prompt. Any terminal will
do (`python -m serial.tools.miniterm COM5 115200`, PuTTY, `screen`). Typed characters are echoed by
the board, so leave local echo off; backspace works; CR, LF and CR LF all end a line.

The parser is a pure function (`components/core/console/parser.cpp`) and every line shape and
every refusal below has a host test (`host_test/core/console/`). The commands run against the same
`RadioPins` the host tests run them against, with fake pins underneath that check the pin rules of
[`ARCHITECTURE.md`](ARCHITECTURE.md) §2 — including a few thousand random command sequences, none
of which may break one. This file grows a command at a time with the tasks; phase 3's task 03
makes it whole.

## What the port prints at boot

```
twinoak-walter 0.0.0-dev
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.0
image store: rakfw at 0x630000 (6144 KB), 4923392 bytes laid out for 2 radio slots x 3 images; 'image list' says what is in it
link: UART1 115200 8N1 on the radio's two pins, open
boot 1: power-on reset
the boot record starts here (a power cut, or the first boot)
type 'help' for the commands
>
```

Before the first line, the seven radio-facing pins have already been put in the safe state:
constructing `RadioPins` is the first thing `app_main` does. The ota state is `valid` on a factory
image from its first boot (the bootloader writes `ota_0`'s entry itself when the OTA data is blank;
task p2-04 read the code and `docs/bench.md` §1 says what to expect), `pending-verify` on the
first boot of an image an update switched to - then a `self-update: … is on trial` line follows
the journal's - and the boot's `software reset` line names a self-update's restart.

## Commands

| Command | What it does |
|---|---|
| `help` | the commands, one line each |
| `status` | the version; the uptime and the boot count; why this boot happened; what is believed about the radio's power; whether the UART, SWD, nRESET and the kill line are in use; how long until the next reset is allowed; the state of the box link and its counters (task p1-02); every pin's mode and level on one line |
| `reboots` | why this board last restarted and the four before that, newest first, each with how long the run before it lasted. The record is in RTC memory: it survives a `reboot`, a panic, a watchdog and a deep sleep, and a power cut clears it (and the first line says so) |
| `pins` | the seven pins, one line each: net, D-sub pin, GPIO, mode as this firmware configured it, the level read from the pad, and what it is in use for (`released`, `uart`, `swd`, `reset`, `kill`); then the radio's power |
| `rak reset` | one reset pulse: nRESET open-drain low for 100 ms, then released. If the last reset ended less than a second ago the command says how long it waits and waits; it never pulses early (two resets inside half a second put the radio into its UF2 bootloader). The first reset after a boot waits a full second, because the Walter's own power-up may have reset the radio just before |
| `rak kill <seconds>` | the LoRa 3.3 V rail off for that long: every other radio-facing pin released first (the UART and SWD taken back from whoever held them), the kill line driven high, held, released (never driven low), then the settle time, then the safe state. Whole seconds, 1 to 30 — the bounds are placeholders until the bench measures the rail (`bench.md` §2). The console is busy for the duration |
| `reboot` | restart the Walter; the pins go to the safe state first and `reboots` after it says *asked for on the console* |
| `trust` | (task p2-01) first a self-test of the signature arithmetic on this board — `ed25519 self-test (RFC 8032 test 1, and one bit changed): passed, <n> ms` (TweetNaCl on the ESP32-S3: expect some hundreds of milliseconds; **write the number down**) — then the key ids this build accepts a release manifest signed with: the trust table generated from `keys/release-keys.txt` (`spec/FIRMWARE-MANIFEST.md` §3, room for three). Today it prints `trust: none: no update will be accepted (keys/release-keys.txt names no key; the release key is made once, by a person: spec/SYSTEM.md section 10 item 1)`, which is the safe state until the key exists. A build made with `-DTWINOAK_TRUST_TEST_KEY=1` (`tools/verify.ps1 -TestKey`) lists the specification's public test key `c17969deb0026955` and adds a `WARNING` that it is a test build (`+TESTKEY` in its version) that must never be released; the same warning is printed at every boot, after the descriptor line |
| `image list` | (task p2-02) what the image store holds, per radio slot: the layout's arithmetic on the first two lines, then each slot's **golden**, **candidate** and **scanner** image — role, board, band, version, size, load address, the first eight bytes of its SHA-256 and which physical area it is in, then when it was stored, the commit it was built from and the key that signed its manifest — or `empty`, or `empty; a transfer of sha256 … stopped at N of M bytes (it can be resumed)`. Slot 1's areas are listed too, with a line saying this adapter has no radio there |
| `image verify <which>` | read that image back out of flash, hash it as it comes, and hold it to the SHA-256 in its record. `<which>` is `golden`, `candidate` or `scanner`, with an optional `@slot` after it. This is the command that finds flash rot before an update does |
| `image erase <candidate\|scanner>` | invalidate that slot's record and clear its header sector, so the slot reads empty and a half-finished transfer is not resumed into the next image. **Never the golden image** (S-5: a known-good image is always on hand), and the refusal says so |
| `image bad` | the hashes of candidates that failed their health check (task p2-03), up to eight; an image on this list is refused before a sector is erased |
| `image receive <which>` | hand this port over to a framed binary transfer and take `manifest.json`, then its `.sig`, then the image — see below |
| `self status` | (task p2-04) this Walter's two application slots — which runs, at what address, in which of the bootloader's states (*valid*, *pending-verify*, *aborted* …), which version — then the trial if one is running (how far into the window, which of the three conditions are met), the image received and waiting for `self switch` if any, and the last self-update's story from the journal. See *The Walter's own update* |
| `self receive` | hand this port over to the same transfer as `image receive`, with the other application slot as its target: a release of **this** firmware's `ota` file, through the six checks (`tools/walterctl.py push --as self`) |
| `self switch [radio-down]` | make the received image the next to boot: the preconditions, the journal's *switching* entry, the boot pointer, `SLEEP_NOTICE` to the radio, restart. Does not come back. `radio-down` says the radio is known to be down, so the trial's second condition does not apply |
| `self confirm` | end the running image's trial early: valid (the bench) |
| `self reject` | end it early: rejected, on the bad list, restart into the previous image (the bench) |

A refusal is one line starting with `error:` and names what was wrong: an unknown command or
subcommand, an argument where none is taken, a missing or non-numeric duration, a line over 200
characters, a control character.

### Which radio: `rak @<slot> …`

Every `rak …` command takes an optional slot, written directly after the word `rak`:

```
> rak @1 hello
error: slot 1: this adapter has 1 radio, so slot 0 is the only one
```

Without one it is `@0`, and on a unit as built that is the only radio there is
(`spec/SYSTEM.md` S-11: "The Walter addresses radios by *slot*; there is one slot today"), so every
line in this document behaves exactly as it reads. The slot goes in *front* of the subcommand
rather than at the end of the line because `rak cli <line>` takes the rest of the line verbatim —
`rak cli get radio @1` sends `get radio @1` to slot 0's radio, which is what it looks like.

`@2` and anything that is not `@0` or `@1` is refused by the parser (*not a slot (@0 or @1)*); `@1`
parses and is then refused by the console, which is the one that knows how many radios the adapter
has. A dual adapter — designed and not built (`docs/ARCHITECTURE.md` §2 rule 6) — would answer
both; nothing on this unit's wires changes either way.

## The radio, over the box link

From task p1-02. Each of these is one request of
[`spec/LINK-PROTOCOL.md`](../spec/LINK-PROTOCOL.md) §5, sent over the UART on GPIO7/8, and prints
the decoded answer in words — or one `error:` line saying why there is none: *no answer to three
attempts: the link is down* (§3's three tries of a second each, so the command can take three
seconds), *refused:* with the radio's own status, *the radio does not advertise the capability*
(§4: nothing was sent), *no session: say hello first*, *the radio is running the scanner: no frames
are sent* (§9), or *the UART is not ours* (the rail is off, or a `rak kill` took the pins).

| Command | What it does |
|---|---|
| `rak hello` | `HELLO`: the protocol version agreed, the role, the slot, the largest payload, `boot_id`, the firmware, board and upstream versions, and the capability bits by name. Says so when the radio's clock has not been set since it booted |
| `rak ping` | `PING` with a nonce made from this board's clock; prints whether it came back, the radio's uptime, the round trip and how many attempts it took |
| `rak status` | `STATUS`: uptime, why it booted, its clock and time state, its flags by name, the observations waiting and lost, the frames its receiver dropped, and any pending attention |
| `rak id` | `IDENTITY`: the node's MeshCore public key and its advert name |
| `rak time` | `TIME_GET`: the radio's clock and where it came from |
| `rak time set <epoch>` | `TIME_SET` with source *a person at a console* (§5.4's source 3) and never *force*: a clock that goes backwards makes a MeshCore node's own traffic look replayed, and the radio refuses a backwards step — which is the right answer to a typed mistake. Prints the clock before and after |
| `rak power` | `POWER_GET`: the three current-sense channels — panel, battery (positive = charging), total load — in millivolts and milliamps, with the shunts already applied by the radio, and which of them were read |
| `rak env` | `ENV_GET`: temperature, humidity and pressure, and which of them were read |
| `rak cli <line>` | `CLI`: the rest of the line, 1 to 160 bytes, run by the radio's own console interpreter as if typed on its serial port, and its reply. A line that restarts the radio (`reboot`) never answers, and the console says that is expected |
| `rak events` | the events the radio has sent and the client has queued — `EV_BOOT`, `EV_MESH_REPLY`, `EV_ATTENTION` — decoded, newest last, and how many have been seen, dropped for want of room, or lost to a gap in their sequence numbers. Printing them takes them out of the queue |
| `rak text` | the lines a scanner has sent instead of frames (§9), and how many were seen, dropped or truncated. Printing them clears them |

`rak reset` and `rak kill` also tell the link client that the radio is starting over: the session,
the `boot_id` and — §9 — whatever firmware it was running are forgotten, and the next `rak hello`
finds out again. `status` has a line about the link: what the client believes it is talking to,
whether a session is up, and the retries, timeouts, dropped frames and events since boot.

## Loading a radio image over USB

From task p2-02 (`docs/ARCHITECTURE.md` §6). Until phase 3 gives the Walter an uplink, a laptop is
how an image gets into the store, and `tools/walterctl.py` is the laptop's end:

```
python tools/walterctl.py push --port COM7 --as candidate <release-dir>
```

A `<release-dir>` is what `tools/release/build_release.py` writes and a GitHub Release carries:
`manifest.json`, `manifest.json.sig` and the files the manifest names. `--as` is `golden`,
`candidate` or `scanner`; `--image <id>` picks one of several images in a manifest; `--slot N`
names the radio (0 today). It needs `pyserial` (`pip install pyserial==3.5`).

What happens on the Walter, in this order:

1. `image receive candidate` prints what the transfer will accept — **role, board and band**, so
   that a mistake is visible before any bytes move — and then **the port is no longer a console**:
   the bytes are frames, nothing is echoed and there is no prompt.
2. The framing is the box link's own (`spec/LINK-PROTOCOL.md` §2: COBS, CRC-16, a type, a sequence
   number, one `0x00` ending a frame). One chunk, one acknowledgement. A chunk that arrives twice is
   acknowledged again and written once; one out of order is answered with the number wanted; a frame
   whose checksum fails is dropped, counted, and not answered at all — the sender's timeout is the
   recovery.
3. `manifest.json` and its `.sig` arrive first, and the moment the image's own header frame comes,
   **checks 1, 2, 3 and the manifest's half of 6** run (`spec/FIRMWARE-MANIFEST.md` §5). A refusal
   here costs nothing: not one sector has been erased.
4. Then the image's bytes go straight into the store's area for that slot, a sector at a time, with
   a progress record every 64 KB. **Nothing is an image yet**: there is no record, so `image list`
   says the slot is empty.
5. The final frame asks the Walter to commit, and **checks 4, 5 and the rest of 6** run over the
   bytes *as they now lie in the Walter's flash* — not over what the laptop said it sent. Only then
   is the record written, by one small final write.
6. The console comes back with one line saying what happened — `image receive: stored`, or the
   number of the check that refused it and why — the chunk counters, and a prompt.

A transfer whose other end goes away ends by itself after 20 seconds of silence, so a Walter whose
cable was pulled mid-push is a console again rather than mute. A push that was cut short is
**resumed**: ask again, and the Walter answers with how many bytes it already has (its last
progress record) and takes the rest.

The refusals, each one line on the console and in `walterctl`'s output: the verdict's own words
(*check 1, the signature: the key id names no key this build trusts* …); *the golden image is
already there: it is replaced by a promotion, not by a load* (a golden image is loaded directly
only on a unit's first provisioning); *this image failed a health check once and is never tried
again*; *the manifest is larger than this firmware reads* (8 KB); *more of the manifest than was
announced*; *that file does not come next*. `walterctl` adds its own, before anything is sent: a
file whose size or SHA-256 is not the manifest's, a manifest with no `swd` file, a directory that
is not a release.

## The radio's debug port, over SWD

From task p1-03 (`rak swd connect`) and p1-04 (`rak swd id`, `rak swd mem`). The two SWD lines
(GPIO2 SWDCLK, GPIO4 SWDIO) are acquired for the length of the command and released on every exit
path; the clock is 125 kHz (a half-period of 4 µs, the
slowest the nRF52840 is specified for, and four time constants of the filtered lines). What the
host tests proved is in `tasks/phase-1/reports/03.md`; the protocol is Arm's ADIv5, cited in
`components/core/swd/`.

| Command | What it does |
|---|---|
| `rak swd connect` | line reset, the JTAG-to-SWD switch, line reset, two idle clocks, a read of `DPIDR` — printed with its fields decoded (revision, part, DP version, designer) and whether it is the nRF52840's `0x2ba01477`; then the debug and system power-up requested through `CTRL/STAT` and the acknowledges waited for (100 ms at most); then the request withdrawn, which is what takes the radio out of debug interface mode; then the pins released, with a line of counters. Nothing else is touched: no memory, no flash |

Its refusals, each one `error:` line and the pins released: *refused: the radio's rail is off*
(inside a `rak kill`), *refused: nRESET is asserted*, *the pin driver refused*, *no target:
nothing answered on the line* (the radio unpowered or absent — the acknowledge read all ones),
*protocol error: an acknowledge that is none of OK, WAIT or FAULT* (the line is confused: a short,
a wrong pin), *parity error on the data read*, *the target kept answering WAIT*. A power-up that
is not acknowledged in time is printed and does not stop the command.

## The chip behind the debug port

From task p1-04. A session — connect, power-up, the control access port, the memory access port,
`FICR` — opened and closed inside the command; both commands are **read-only**: nothing is halted,
nothing is written, no flash controller register is touched. What the host tests proved is in
`tasks/phase-1/reports/04.md`; the registers and their chapters are in
`components/core/include/core/nrf52/regs.h`.

| Command | What it does |
|---|---|
| `rak swd id` | the DPIDR; the control access port's `IDR` and whether access-port protection is **open** or **PROTECTED**; the memory access port's `IDR`; then, on an open chip, `FICR`: part, variant (the four-character build code — **write it down**, it decides whether production modules arrive protected), flash size in pages of 4096 bytes, `INFO.FLASH`, the device id; the core's state (`running`, `halted`, `sleeping`, `locked up`); the power-up withdrawn, the pins released, the counters. A protected chip gets no memory access at all: the command says so, says that opening a module is a person's bench procedure, and stops. A part other than an nRF52840 with 4096-byte pages is printed as what it is and refused |
| `rak swd mem <addr> [words]` | 1 to 32 words from a word-aligned address — hexadecimal with `0x`, or decimal — four a line: `rak swd mem 0x10000100 2` prints the part and the variant, `rak swd mem 0x00026000 4` the start of the application's vector table, `rak swd mem 0xE000EDF0` the core's `DHCSR`. Refused on a protected chip and on another part, like everything below identification |

Their refusals: the debug port's (above); *protected: access-port protection is on, memory access
not attempted*; *not an nRF52840 with 4096-byte pages: refused*; *no Nordic control access port
at AP 1*; *no memory access port at AP 0*; and for `mem`, a `FAULT ... at 0x...` when the address
is nothing (the access port's error response, read and cleared), *the address must be word-aligned*,
*words is 1 to 32*.

## The flasher

From task p1-05. The flasher itself (`components/core/flasher/`, `docs/ARCHITECTURE.md` §5) is
what phase 2's `rak flash <slot>` and the radio's update will drive; today the console has one
command that exercises its erase, write, verify and reset on real silicon without putting the
firmware the radio runs at risk. What the host tests proved — above all the power cut at every
flash operation — is in `tasks/phase-1/reports/05.md`.

| Command | What it does |
|---|---|
| `rak flash-test` | *(`rak @0 flash-test`; the board profile comes from the slot)* the session of `rak swd id` (connect, the control port's protection, FICR), then the SoftDevice's own information structure checked against the board profile (it must end at `0x26000`, where the application begins); then the **last two pages of the application region** (`0xEB000`–`0xED000`) read: **they must be blank**, or the command refuses and touches nothing. Then the core is halted, a two-page pattern written and verified page by page, the whole of it read back and its SHA-256 checked, the two pages erased again and read back blank, the radio reset through the debug port, the pins released. One line per page per pass, then a summary with the counts and the time; `rak reset` afterwards waits out the spacing, because the radio was just reset. The bootloader's settings page is **not** touched: the pattern is not the application, and the firmware that was running starts again after the reset |

Its refusals, each one `error:` line naming the step and the pins released: the debug port's and
the chip's (above); *the SoftDevice does not end where the profile's application region begins*
(a chip with another SoftDevice, or none — the profile is wrong for it); *the last two pages of the
application region are not blank: refused* with the address of the first word that is not. A
failure after the halt — a page that will not verify after a retry, a target that stops answering —
is reported with its step and address; where the debug port still answers, the radio is reset on
the way out so that it runs its bootloader rather than sitting halted.

## The radio's update

From task p2-03 (`docs/update.md` has the whole of it: the states, the health check, every line,
and what a unit is in after a power cut at each step). Each of these drives
`core::update::RadioUpdate` to its end inside the command, printing a line per transition as the
journal takes it, with the flasher's progress lines between; the console is busy for the flash and
then for the proving period (minutes). All four take the slot in front (`rak @0 update`).

| Command | What it does |
|---|---|
| `rak update` | the candidate (`image receive candidate`) flashed into the radio, reset into, and put through the health check; then **promoted to golden** - or, if it fails, its hash on the bad list **first**, the golden image flashed back and proved by the same check. Ends `rak update: done: <version> (sha256 …) is the golden image`, or `rak update: restored: the candidate failed its health check (<reason> at <step>) and is on the bad list; the golden image <version> is back and proved`, or `rak update: STUCK: <reason> - the radio has no proved application; a person's business now`. Refused in one `error:` line when a precondition fails - no candidate, on the bad list, the wrong role, board or band, the link down, the battery low or unreadable, **no golden image**, another run journalled as in progress, the Walter's own update unconfirmed (S-7) - with nothing touched |
| `rak update --first-install` | the same with no golden image to return to: a unit's first repeater image, a person's command only. A failure ends *stuck: no image to return to* |
| `rak update status` | the journal's story of the last run, oldest entry first, each with its sequence number, state, and - where it has one - the reason and the step; then `the last update ended: <state>`, or where a resumed run is now, or that the journal says one did not end and it is resumed at the next boot. `no update has been journalled on this unit` on a fresh one |
| `rak role scanner` | flash the stored scanner image (`image receive scanner`) and prove only that something that calls itself a scanner is there (p1-02's text-mode detection); golden and candidate untouched, nothing promoted. Afterwards `rak hello` says *the radio is running the scanner* and `rak text` shows its lines |
| `rak role repeater` | the golden image flashed back and proved as an update would prove it (through the rolling-back states), then *done*; the node's identity and name are what they were, because the file system above the application region is never touched |

At boot, a run the journal says did not end is announced - `update: the journal says a run was in
progress (update); resuming at flashing-candidate` - and driven from the main loop one step a turn,
each transition printed; `rak update` meanwhile answers *an update is already running*.

## The Walter's own update

From task p2-04 (`docs/update.md`, *The Walter's half*). Not `rak` lines: they are about this
board's two application slots and take no slot.

| Command | What it does |
|---|---|
| `self status` | `running: slot 1 at 0x310000 (3072 KB): valid (confirmed), version 0.4.0+w1` and the same for the other slot; then `on trial for 42 s of 300: main loop met; radio met (1 HELLO); uplink failed, trying again (2 attempts)` while a trial runs, or `the running image is confirmed` (or the bootloader's word for its state); then `received: <version> in slot N (<bytes>), waiting for 'self switch'` when an image is; then `the last self-update's story (N entries):` with one line per journal entry — *received*, *switching to … from …*, *on trial*, *confirmed*, *rolled back: <reason>*. On a fresh unit: `self status: no self-update has been journalled on this unit` |
| `self receive` | `self receive: this Walter's other application slot (1); this port is now a framed binary transfer` and what will be accepted (role manager, board walter, band lte-m, an ota file that fits the slot); the transfer ends with `self receive: stored` or `self receive: <refusal>`. Refused in one `error:` line when the running image is on trial, a radio update is in progress (S-7) or a transfer is open |
| `self switch [radio-down]` | `self switch: making <version> (slot N) the next to boot, from <version>; the radio is told and the Walter restarts`, then the restart. Refused in one `error:` line for the battery, a radio update in progress, the running image unconfirmed, nothing received, the other slot changed, or the radio silent without `radio-down` |
| `self confirm` | `self confirm: the running image is confirmed (valid); the trial is over` |
| `self reject` | `self reject: the running image is rejected; restarting into the previous image` |

At boot, a trial announces itself - `self-update: this image (0.4.0, slot 1) is on trial: 300 s to
reach the main loop, HELLO the radio and reach the backend, or it is rolled back` - and is run from
the main loop; its end is one line, `self-update: on trial -> confirmed: all three conditions hold:
confirmed` or `self-update: on trial -> rolled back: <reason>`. The old image, back after a trial
that a reset cut short, says `self-update: rolled back: <reason>` and that the image is on the bad
list. `reboots` names both restarts: *the Walter's own update, switching to the other slot* and
*the Walter's own update, the trial image rejected itself*.

## The radio's power, as `status` says it

| | Means |
|---|---|
| `on` | the kill line is not driven and the settle time has passed since it was last released |
| `off (the kill line is driven)` | inside a `rak kill` |
| `unknown (the rail is settling)` | within the settle time after a kill was released or after boot (a Walter that reset in the middle of a kill left the line to float back on just before). The UART and SWD wait it out |

## What the port prints besides replies

Nothing by itself. The radio's events are collected whenever the main loop turns - every 100 ms or
less - and wait in the client's queue until `rak events` prints them; nothing is printed as it
arrives, because a Walter on a mast has nobody reading. ESP-IDF's own boot messages come before the
banner at the bootloader's log level.
