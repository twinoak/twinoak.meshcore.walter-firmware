# The console

The Walter is provisioned, looked at and exercised over the module's USB-C, which is the
ESP32-S3's own USB Serial/JTAG port: any baud (it is ignored), one command per line, a `>` prompt.
Any terminal will do (`python -m serial.tools.miniterm COM5 115200`, PuTTY, `screen`). Typed
characters are echoed by the board, so leave local echo off; backspace works; CR, LF and CR LF all
end a line. The value of `set lte.pin` and `set server.token` echoes as asterisks.

Someone who has used the exit node's console should feel at home in this one: the grammar, the
echo, the refusals and the provisioning session are the agent firmware's
([`docs/console.md`](https://github.com/twinoak/meshmaster-agent-firmware/blob/master/docs/console.md)
there).

There is **one parser** (`components/core/console/parser.cpp`), **one command table** with one
line of help each (`components/core/console/commands.h`) and **one table of settings**
(`components/core/settings/keys.h`), and this document is held to all three by a host test
(`host_test/core/console/test_console_grammar.cpp`): every command and every key below is in the
table, and every refusal has a test in `host_test/core/console/`. The commands run against the
same `RadioPins` the host tests run them against, with fake pins underneath that check the pin
rules of [`ARCHITECTURE.md`](ARCHITECTURE.md) §2 — including a few thousand random command
sequences, none of which may break one.

[`provisioning.md`](provisioning.md) is the session that takes a Walter from the factory image to
a provisioned unit, line by line, and it is replayed by a host test so that it cannot rot.

## What the port prints at boot

```
twinoak-walter 0.0.0-dev
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.0
settings: nvs namespace 'walter', 11 keys; 'get' prints them, 'set <key> <value>' changes one
settings: NOT encrypted - flash encryption is the owner's choice and is not assumed: a person with this board and a cable can read lte.pin and server.token out of it (docs/console.md)
uplink: the modem on UART2 behind interfaces::Uplink; not started yet ('lte', 'lte up')
https: 2 TLS roots (keys/tls), the modem's slots are recorded as written
image store: rakfw at 0x630000 (6144 KB), 4923392 bytes laid out for 2 radio slots x 3 images; 'image list' says what is in it
journal: at 0x620000 (64 KB), 0 entries, next seq 1; 'rak update status' tells the story
link: UART1 115200 8N1 on the radio's two pins, open
boot 1: power-on reset
the boot record starts here (a power cut, or the first boot)
settings: 0 of 11 keys set; log.level info, swd 125 kHz ('get')
wake: a power-up: the radio was reset with us and is still booting ('wake', 'power')
wake: a USB host is on the port, so this Walter stays awake while you are here (and for a minute after the cable goes). 'stay' keeps it awake without one
type 'help' for the commands
>
```

Before the first line, the seven radio-facing pins have already been put in the safe state:
constructing `RadioPins` is the first thing `app_main` does. The ota state is `valid` on a factory
image from its first boot (the bootloader writes `ota_0`'s entry itself when the OTA data is blank;
task p2-04 read the code and `docs/bench.md` §1 says what to expect), `pending-verify` on the
first boot of an image an update switched to — then a `self-update: … is on trial` line follows
the journal's — and the boot's `software reset` line names a self-update's restart.

The `uplink:` line is printed **without touching the modem** (task p3-01): all that happens at boot
is that the backoff's record is read out of RTC memory, and a second line follows it when there is
something in that record to say — `uplink: last time: not-registered; 14 wakes counted, 6 attempts;
next try in 3 wake(s)`. The modem itself is started by the first call that needs it.

The lines from `boot 1:` down are the console's, and they are the ones **`log.level` governs**:
at `warn` and `error` a boot says only what has gone wrong, the invitation and the prompt. A
supervisor's stop is printed at every level.

## Commands

`help` prints this table, grouped as it is grouped here, with one line each.

### The unit

| Command | What it does |
|---|---|
| `help` | the commands, one line each, and the settings keys |
| `status` | one screen: this firmware and what its descriptor says it is; the unit's name; why this boot happened, the uptime and the boot count; the radio's session, health and clock; the battery as the radio last reported it; the link's counters; the pins in one line; the images the store holds; this firmware's own slot and trial; the uplink's last attempt and when the next one is allowed; the backend; the next wake. **No secret and no IMEI** — the subscription's identity is `lte id` and nowhere else |
| `get [key]` | one setting, or every setting in the order of the table below. A secret prints `set` or `unset` and never its value; an unset key shows its fallback in brackets (`log.level unset (info)`) |
| `set <key> <value>` | store a setting. The value is the rest of the line, trimmed, so a value with spaces is fine; it is validated against the table below before it is stored, and a refusal stores nothing. The reply is `ok <key>` — the value is never echoed back, which is what makes a secret's reply the same as anything else's |
| `erase confirm` | forget every setting and **only** the settings: the images, the journal, the bad list and the supervisor's counts are in other partitions and in RTC memory and are not touched. Without the word it refuses and says what the word is. The fallbacks are in force at once, and the PIN's generation moves so that a refusal recorded against the PIN just forgotten is not applied to the next one |
| `reboot` | restart the Walter; the pins go to the safe state first and `reboots` after it says *asked for on the console* |
| `reboots` | why this board last restarted and the four before that, newest first, each with how long the run before it lasted. The record is in RTC memory: it survives a `reboot`, a panic, a watchdog and a deep sleep, and a power cut clears it (and the first line says so) |
| `dump` | the last 4 kB this port printed, oldest first, **whole lines only** — a board's answer to a journal, for a unit found quiet with a laptop and no history. The same bytes the port printed and no others. **Nothing the radio sent as text is in it** and nothing the mesh said (S-12): a scanner's NDJSON, a CLI reply and an advert's name are printed and not kept. A secret never reaches a printed line, so there is none in the ring either |

### The radio

| Command | What it does |
|---|---|
| `pins` | the seven pins, one line each: net, D-sub pin, GPIO, mode as this firmware configured it, the level read from the pad, and what it is in use for (`released`, `uart`, `swd`, `reset`, `kill`); then the radio's power |
| `rak reset` | one reset pulse: nRESET open-drain low for 100 ms, then released. If the last reset ended less than a second ago the command says how long it waits and waits; it never pulses early (two resets inside half a second put the radio into its UF2 bootloader). The first reset after a boot waits a full second, because the Walter's own power-up may have reset the radio just before |
| `rak kill <seconds> [confirm]` | the LoRa 3.3 V rail off for that long: every other radio-facing pin released first (the UART and SWD taken back from whoever held them), the kill line driven high, held, released (never driven low), then the settle time, then the safe state. Whole seconds, 1 to 30 — the bounds are placeholders until the bench measures the rail (`bench.md` §2). **Over 5 seconds it wants the word `confirm` after the number**: a repeater off the air for half a minute is a decision, not a typo. The console is busy for the duration |
| `rak hello` | HELLO: the version, role, slot and capabilities the radio answers with |
| `rak ping` | PING, with the round trip |
| `rak status` | STATUS: its uptime, why it booted, its clock, its flags and counters |
| `rak id` | IDENTITY: the node's public key and advert name. The name is printed and not logged (S-12) |
| `rak time [set <epoch>]` | TIME_GET; `rak time set <epoch>` is TIME_SET from a person at this console, which is `spec/LINK-PROTOCOL.md` §5.4's third source of time |
| `rak power` | POWER_GET: the three current-sense channels |
| `rak env` | ENV_GET: temperature, humidity, pressure |
| `rak cli <line>` | CLI: one line run by the radio's own console interpreter, and what it answered. The reply is printed and not logged |
| `rak events` | the events the client has queued |
| `rak text` | the lines a scanner has sent instead of frames (§9), printed and not logged |
| `rak swd connect` | acquire the SWD lines, connect, print DPIDR and the power-up, release |
| `rak swd id` | connect, identify the nRF52840 (FICR), its protection and its core's state, release |
| `rak swd mem <addr> [words]` | read 1 to 32 words at a word-aligned address (hex `0x…` or decimal), read-only |
| `rak flash-test confirm` | a two-page pattern at the top of the application region, only if those pages are blank: written, verified, erased again; the radio restarted. **It wants the word**: it is the one command that writes to a radio's flash outside the update machine |
| `rak update` | flash the candidate, prove it, make it golden — or roll back to the golden image and never try that candidate again (minutes; `docs/update.md`) |
| `rak update --first-install` | the same with no golden image to return to: a person's command only |
| `rak update status` | the journal's story of the last update of the radio |
| `rak role scanner\|repeater` | flash the stored scanner image and prove a scanner is there; or the golden image back, proved as an update would be |
| `rak watch` | the supervisor's view: whether the radio is healthy, when it was last so, today's counts against the daily caps, the last good battery reading, the radio's restarts, a stop in force, and the last three incidents with every rung they tried. It only reads |
| `rak watch reset-caps` | the day's counts to zero and a stop lifted, journalled (the bench) |
| `rak ladder` | climb the supervisor's ladder now — ask, reset, power-cycle, look at the chip, stop — each rung printed as it is tried. Minutes: the waits are the real ones |

### Images, trust and this firmware's own slots

| Command | What it does |
|---|---|
| `image list` | what the image store holds, per radio slot: the layout's arithmetic on the first two lines, then each slot's **golden**, **candidate** and **scanner** image — role, board, band, version, size, load address, the first eight bytes of its SHA-256 and which physical area it is in, then when it was stored, the commit it was built from and the key that signed its manifest — or `empty`, or `empty; a transfer of sha256 … stopped at N of M bytes (it can be resumed)`. Slot 1's areas are listed too, with a line saying this adapter has no radio there |
| `image verify <which>` | read that image back out of flash, hash it as it comes, and hold it to the SHA-256 in its record. `<which>` is `golden`, `candidate` or `scanner`, with an optional `@slot` after it. This is the command that finds flash rot before an update does |
| `image erase <candidate\|scanner>` | invalidate that slot's record and clear its header sector, so the slot reads empty and a half-finished transfer is not resumed into the next image. **Never the golden image** (S-5: a known-good image is always on hand), and the refusal says so. Not to be confused with `erase confirm`, which is the settings |
| `image bad` | the hashes of candidates that failed their health check (task p2-03), up to eight; an image on this list is refused before a sector is erased |
| `image receive <which>` | hand this port over to a framed binary transfer and take `manifest.json`, then its `.sig`, then the image — see below |
| `trust` | (task p2-01) first a self-test of the signature arithmetic on this board — `ed25519 self-test (RFC 8032 test 1, and one bit changed): passed, <n> ms` (TweetNaCl on the ESP32-S3: expect some hundreds of milliseconds; **write the number down**) — then the key ids this build accepts a release manifest signed with: the trust table generated from `keys/release-keys.txt` (`spec/FIRMWARE-MANIFEST.md` §3, room for three). Today it prints `trust: none: no update will be accepted (keys/release-keys.txt names no key; the release key is made once, by a person: spec/SYSTEM.md section 10 item 1)`, which is the safe state until the key exists. A build made with `-DTWINOAK_TRUST_TEST_KEY=1` (`tools/verify.ps1 -TestKey`) lists the specification's public test key `c17969deb0026955` and adds a `WARNING` that it is a test build (`+TESTKEY` in its version) that must never be released; the same warning is printed at every boot, after the descriptor line |
| `trust tls` | the TLS roots this build trusts — name, subject, SHA-256 fingerprint, expiry, source, the date they were checked — which certificate slot and TLS profile of the modem each goes to, whether the modem's slots are recorded as holding them, and which host is held to which root |
| `self status` | (task p2-04) this Walter's two application slots — which runs, at what address, in which of the bootloader's states (*valid*, *pending-verify*, *aborted* …), which version — then the trial if one is running (how far into the window, which of the three conditions are met), the image received and waiting for `self switch` if any, and the last self-update's story from the journal. See *The Walter's own update* |
| `self receive` | hand this port over to the same transfer as `image receive`, with the other application slot as its target: a release of **this** firmware's `ota` file, through the six checks (`tools/walterctl.py push --as self`) |
| `self switch [radio-down]` | make the received image the next to boot: the preconditions, the journal's *switching* entry, the boot pointer, `SLEEP_NOTICE` to the radio, restart. Does not come back. `radio-down` says the radio is known to be down, so the trial's second condition does not apply |
| `self confirm` | end the running image's trial early: valid (the bench) |
| `self reject` | end it early: rejected, on the bad list, restart into the previous image (the bench) |

### The uplink

| Command | What it does |
|---|---|
| `lte` | (task p3-01) the uplink: what the modem is, what the last attempt did and how far it got, when the backoff allows the next one, whether a SIM PIN is set and whether it was refused, the last signal reading, the last time, and what power saving the network granted. **It only reads** — it does not start the modem |
| `lte up` | bring the uplink up now: the whole sequence inside one wake's budget. **Minutes** — an attach from cold is slow — and it goes ahead even when the backoff would have waited, because a person asked |
| `lte down` | the modem's radio off (`AT+CFUN=0`); the context goes with it |
| `lte time` | the network's time, or a refusal in words when the modem answered with its own default |
| `lte id` | the modem's IMEI and the SIM's ICCID and IMSI, for the label on the box. **The only place this firmware prints them**, and they are in no log line, no journal entry and no `status` |
| `http get <url>` | (task p3-02) one GET over TLS whose chain is verified against this build's roots: the status, the headers worth naming (`Content-Type`, `ETag`, `Last-Modified`, `Accept-Ranges`, `Content-Length` or chunked), the first 32 bytes as hex, the size, the time; the response is read to 1 KB and cut there. `https://` only; the uplink must be up (`lte up` first). See *HTTPS* |
| `fetch [base-url] [candidate\|scanner]` | a release from `<base-url>/manifest.json` and `manifest.json.sig` into the store's candidate (default) or scanner image: the three steps, each of the six checks printed as it passes, the image in ranges with every range's line; resumable across wakes. **Minutes.** Never the golden image. With no base URL on the line the `fetch.url` setting is used, and `fetch` says so |

### Sleep and the battery

| Command | What it does |
|---|---|
| `wake` | (task p3-04) why this Walter is awake — the timer, the radio's line with the attention behind it, a spurious wake, a person with a cable, a power-up, a crash, a watchdog — the last ten wakes with their reasons and how long each lasted, the spurious count, and whether the radio's line is armed as a wake source. **It only reads** |
| `power` | the battery's band, the readings behind it (the median and the mean, and every sample kept), the thresholds, what a wake in that band may do, and how long until the next one. It costs one `POWER_GET` on the link. See `docs/power.md` |
| `sleep now` | go to sleep for the interval the battery chose: the journal, `SLEEP_NOTICE` to the radio, the modem down, the pins safe and held, deep sleep. **It does not come back** — the next boot is the next wake |
| `sleep <seconds>` | the same, for a number a person picks (1 to 604800). What the band allows is unchanged; only the timer is |
| `stay [off]` | keep the Walter awake without a cable, for the bench, bounded by twelve hours; `stay off` ends it. Without it, and without a USB host, a wake that has not reached sleep in five minutes is ended by a reset |
| `cycle now` | (task p3-05) run **one whole wake** on request: the ten steps of `docs/cycle.md` in order, each printed with its outcome, its reason and what it spent, and then sleep. **It does not come back** unless the sleep was refused |
| `cycle dry` | what a wake *would* do, doing none of it: every step with its bound, its preconditions as they stand now, the band and the interval, and what is behind the backend seam. Nothing is sent, nothing is written, no byte of the modem or the link is touched |

A sleep is refused, in one `error:` line, when the radio's update has a step in the air, when a
transfer into the other application slot is open, or when **this firmware is on trial** — a deep
sleep during a trial would roll the trial image back, because the bootloader turns a
`pending-verify` slot into `aborted` at every reset and a wake from deep sleep is one. `self
confirm` or `self reject` first. See *Sleep, and what wakes it* below.

### The journal

| Command | What it does |
|---|---|
| `journal` | the last twelve entries of the flash journal in words, newest first, whatever wrote them: the radio's updates (task p2-03), this firmware's own (p2-04), the supervisor's ladder (p2-05), the duty cycle's sleeps (p3-04) and the wake records (p3-05), with the head, the next sequence number and whether an operation's entries are protected from a wrap. `rak update status` and `self status` tell the story of one run; this is the log |

A refusal is one line starting with `error:` and names what was wrong: an unknown command,
subcommand or key, an argument where none is taken, a missing or non-numeric value, a value out of
range, a URL without `https://`, a line over 200 characters, a control character, a missing
`confirm`.

## Settings

Eleven keys, in the order `get` and `help` print them. Everything is stored in NVS under one
namespace, `walter`, in the `nvs` partition — which `partitions.csv` puts **above** the
application slots, so that a factory image cannot reach it
([`ARCHITECTURE.md`](ARCHITECTURE.md) §11); the agent firmware learned that the expensive way.

| Key | Value | Fallback while unset |
|---|---|---|
| `lte.apn` | 1 to 63 printable characters | none: the network's default APN is used, which is what a Danish LTE-M SIM wants |
| `lte.pin` | 4 to 8 digits; **secret**, never printed | none: no PIN is sent, and a card that asks for one is reported `sim-locked` |
| `lte.psm` | `off`, `on` | `off`: the modem's radio is put down between wakes (task p3-01's own default) |
| `server.url` | `https://host[:port][/path]`, the scheme required, at most 128 characters | none: no backend call is made, and a self-update's third condition does not apply |
| `server.token` | printable, at most 160 characters; **secret**, never printed | none: no backend call is made |
| `fetch.url` | `https://host[:port][/path]`, at most 128 characters | none: `fetch` needs a base URL on the line |
| `wake.interval` | `auto`, or 5 to 10080 minutes | `auto`: the battery chooses the interval (task p3-04) |
| `wake.radio` | `on`, `off`: may a start bit from the radio wake the Walter | `on` |
| `swd.khz` | 25 to 1000 | `125`, which is the wire's own default half-period of 4 µs and the slowest clock the nRF52840 is specified for |
| `log.level` | `error`, `warn`, `info`, `debug` | `info` |
| `unit.name` | 1 to 32 printable characters, a label for a person | none: the unit has no label |

There is no `unset <key>`: `erase confirm` is what makes a key unset again, because the one thing
a half-provisioned unit must not do is look provisioned.

### What happens when one changes

`set` validates before it stores, and the reply is followed by one line saying what the change
reaches:

| Key | When it takes effect |
|---|---|
| `lte.apn` | the next time the uplink is brought up ('lte up', or the next wake) — the APN is sent when the packet context is defined, and a modem reset now would lose a card that had just been unlocked |
| `lte.pin` | at once: a refusal remembered against the old PIN is forgotten, and the next uplink sends the new one. Typing the *same* PIN again changes nothing, which is the point: three refusals block a card, and a refused PIN is never sent a second time (`core/uplink/policy.h`, *The PIN*) |
| `lte.psm` | the next time the uplink is brought up |
| `server.url` | at once: the host a self-update's trial must reach; the backend call itself is task p3-05's. A URL with no host this firmware can reach — a port over 65535, say — leaves no server configured, and the trial's third condition does not apply rather than failing |
| `server.token` | kept for the backend call (task p3-05); nothing else reads it |
| `fetch.url` | the next `fetch` typed with no base URL |
| `wake.interval` | the next time the interval is chosen, at the end of a wake (task p3-04) |
| `wake.radio` | the next time the Walter goes to sleep (task p3-04) |
| `swd.khz` | at once: every SWD command and every flashing from here on — the console's own wire, the update machine's flasher and the supervisor's |
| `log.level` | at once: a reply is always printed, the lines nobody asked for are not |
| `unit.name` | at once: what `status` and the boot line call this unit |

### Secrets

`lte.pin` and `server.token` are typed once and printed by nothing afterwards:

* `get` says `set` or `unset` and never the value;
* `set` echoes the value as asterisks as it is typed, and answers `ok <key>` — the same reply
  every other key gets;
* no log line, no journal entry, no `status`, no `dump` and no refusal contains one. A refusal
  about a PIN says *digits only*, not which digits.

A host test (`host_test/core/console/test_console_secrets.cpp`) sets both and then runs **every
command of the table**, greps everything the port printed — replies, refusals, boot lines, the
ring `dump` prints — and the journal's own bytes for both values.

**NVS is not encrypted** unless the owner turns flash encryption on, and this firmware does not
assume it: without it, a person with the board in hand and a cable can read `lte.pin` and
`server.token` out of the `nvs` partition. That is the threat model of a unit on a mast in a
locked cabinet, and it is written down here rather than hidden. The boot says so in one line.

### What protects the hardware is not a setting

The thresholds that keep a radio alive are build constants and stay constants — a typo at a
console must not be able to remove a safety margin:

* the battery floors for a power cycle and for a flash (`core/update/radio_update.h`,
  `core/supervisor/supervisor.h`);
* the supervisor's daily caps, its quiet period and its boot-loop window;
* the proving period after a flash, and the Walter's own trial window and its three conditions;
* a wake's budget on the modem, the deadline of every step, and the backoff's own arithmetic;
* the reset spacing, the kill bounds and the settle time (`docs/ARCHITECTURE.md` §2).

`swd.khz` is the one hardware-facing number that *is* a setting, because a long cable on a bench
is a reason to slow the debug clock down, and the wire bounds it on both sides whatever is typed.

### A store that is not there, or not ours

A `nvs` partition that is missing or would not open is one line at boot and every key falling
back; a `set` is then refused rather than lost. A *stored value this firmware will not have* — a
store written by another firmware, a value a person put there with `nvs_set_str`, a flipped bit —
is ignored key by key: that key reads as unset and falls back, the boot says how many were
ignored and names the first, and the bytes are left alone until a `set` replaces them. A unit that
has never been provisioned and a unit whose settings are rubbish behave the same way, which is
the state this firmware has to survive anyway.

## Which radio: `rak @<slot> …`

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

## The supervisor

From task p2-05 (`docs/supervisor.md` has the whole of it: the ladder, the four kinds of silence,
the waits and the caps as a table, and what a person should do when it has stopped). All three take
the slot in front (`rak @0 watch`).

| Command | What it does |
|---|---|
| `rak watch` | the supervisor's view and nothing else: whether the radio is healthy, silent or running the scanner; healthy since when, when it last answered, how many pings, its uptime and `boot_id`, its last reset reason and whether its clock was set; **today's counts against the caps** and when the day began; the last good battery reading with the thresholds a power cycle and a flash need; how many times the radio has restarted and what a boot loop is; a stop in force with the rungs that were tried and how long is left of the quiet period; any attention it is keeping (§8); and the last three incidents, newest first, each with every rung it tried and what came of it. **It reads and nothing more** - looking at the supervisor can never reset a radio |
| `rak watch reset-caps` | the day's counts to zero and a stop lifted, journalled. For the bench: `rak watch reset-caps: the day's counts are empty again and the stop is lifted; journalled` |
| `rak ladder` | climb now. It asks the radio; if it answers, `rak ladder: the radio is healthy: nothing to climb` and nothing is done. If it is silent, what the silence means is worked out and the ladder climbed, one line per rung as it is tried - `supervisor: rung 2, a reset pulse: done; the radio did not come back - nRESET pulsed; waiting for it to boot` - with the flasher's progress lines inside rung 4. It takes **minutes**: the waits are the real ones. It ends `rak ladder: the radio is back after rung 3, a power cycle`, or `rak ladder: STOPPED: <reason>` with the rungs that were tried, or - for a scanner - *not a fault, left alone*. A stop in force is lifted first: a person is the other way out of rung 5 |

At boot the supervisor says what it has been doing only when there is something to say:
`supervisor: STOPPED: <reason>; 258788 s of the quiet period left ('rak watch')`, or
`supervisor: today already: 2 resets, 1 power cycles, 0 flashes ('rak watch')` - and, after a power
cut, `, read back from the journal`, because RTC memory went with the power and flash did not.

**Nothing drives the supervisor from the main loop.** When it runs is the wake cycle's business
(phase 3); in this build `rak ladder` is the only thing that climbs, and `begin()` at every boot is
what reads the record.

## The uplink

From task p3-01 (`docs/ARCHITECTURE.md` §9; `components/core/uplink/policy.h` has the sequence, the
deadlines and the backoff with a sentence on each number). None of the five takes a slot: a modem is
not a radio.

```
> lte up
lte up: bringing the uplink up; the budget is 240 s and an attach from cold can take
  most of it. The backoff would have allowed this wake
lte up: up in 7412 ms (up)
  signal: RSRP -92 dBm, RSRQ -10.5 dB, RSSI -71 dBm, operator TDC, band 20, LTE-M
  time: 1791936086 unix seconds, from the network
  power saving: asked for off (the modem is switched off between wakes); the network has granted nothing (it reports PSM timers when it feels like it)
```

A failure says which of the five it was, what that means, and how far the sequence got:

```
> lte up
lte up: no-sim - no card, or it does not answer
  reached 'the SIM' in 10041 ms; the modem is down again
  try again: next wake; not sooner (a person has to act)
    1 failures in a row; every 1 wake(s); 0 wake(s) until the next is allowed
```

The five failures and when the firmware tries again by itself:

| | Means | Try again |
|---|---|---|
| `modem-silent` | no answer to the first `AT` | after a modem reset, **once** per wake; then the next wake |
| `no-sim` | no card, or it does not answer | next wake, and not sooner: a person has to act for it to change |
| `sim-locked` | a PIN is wanted and none is set, or the one set was refused | **never automatically.** Three refusals block a card; `lte up` is a person's way past it, and a PIN that was refused is still never sent again |
| `not-registered` | searching, denied, or no network inside the deadline | growing: next wake, then every second, fourth … up to the cap |
| `no-data` | registered, and no packet data | as `not-registered` |

The backoff is counted in **wakes**, not in seconds (a Walter has no clock it can trust across a
deep sleep), and lives in RTC memory: a reset and a deep sleep keep it, a power cut starts it again
and the `lte` line says so. `no-time` is not a failure: an uplink that came up without a time is up,
and `lte` reports the time beside the state.

`lte time` refuses a time that is not the network's rather than passing it on, because this is the
time that would set the radio's clock (`spec/LINK-PROTOCOL.md` §5.4):

```
> lte time
error: lte time: the modem answered with its own default, not a time the network
  gave it. This firmware refuses it: a wrong clock is worse than none
  (spec/LINK-PROTOCOL.md section 5.4)
```

`lte id` prints the three numbers that identify a subscription. They are printed **because a person
asked** and are written to no log and sent nowhere by this firmware; a host test greps the console's
whole transcript for them (`host_test/core/uplink/test_uplink_privacy.cpp`).

## HTTPS

From task p3-02 (`docs/ARCHITECTURE.md` §9; `keys/tls/README.md`). Three lines, none with a slot;
`http get` and `fetch` need the uplink up (`lte up` first) and are refused in one line otherwise.
Every connection verifies the server's chain against one of this build's roots **and** the host
name; there is no command and no setting that asks for less, and a host whose chain ends at a root
this firmware does not carry is refused.

```
> http get https://meshmaster.dk/
http get: https://meshmaster.dk/ (the chain is verified against this build's roots)
  GET meshmaster.dk/ -> 200 ok, 1024 bytes, 2831 ms
http get: 200 the response is larger than the bound: cut off there
  Content-Type: text/html; charset=utf-8
  Transfer-Encoding: chunked
  first 32 bytes: 3c21444f43545950452068746d6c3e0a3c68746d6c206c616e673d22656e223e
  1024 bytes (cut off: a console line reads 1 KB at most) in 2831 ms
```

A redirect is followed (at most three, only to `https://`) and the last host named: `http get: 200
ok after a redirect to release-assets.githubusercontent.com`. A refusal is one `error:` line: *only
https URLs are accepted*, *the uplink is down: 'lte up' first*, or - when the modem would not
connect - *no response: the modem refused the connection: the host was unreachable or the TLS
handshake failed (the library does not say which)*. That last wording is honest about the modem
library at 1.5.0, which answers one `ERROR` for a name that does not resolve and for a chain it
cannot verify; `docs/bench.md` §12 is where the two are told apart on a desk, by choosing hosts.

```
> fetch https://github.com/twinoak/twinoak.meshcore.repeater-firmware/releases/download/v0.3.0
fetch: https://github.com/.../v0.3.0 into the candidate image of slot 0: manifest.json, its .sig, the six checks, the image
  GET github.com/twinoak/twinoak.meshcore.repeater-firm... -> 302 ok, 0 bytes, 2210 ms
  GET release-assets.githubusercontent.com/github-produc... -> 200 ok, 1558 bytes, 2644 ms
fetch: manifest.json, 1558 bytes
  ...
fetch: manifest.json.sig, 114 bytes
fetch: check 1, the signature: passed (key this build trusts)
fetch: check 2, the manifest: passed
fetch: check 3, the image for this slot: passed - twinoak-repeater-rak4630-single-868-v0.3.0.bin, 212992 bytes, version 0.3.0+abc1234
fetch: check 6, the manifest's region against this build's: passed
  range 0-65535: ok, 206 (attempt 1)
  range 65536-131071: ok, 206 (attempt 1)
  range 131072-196607: failed, the connection closed before the body was complete (attempt 1)
  range 150000-212991: ok, 206 (attempt 2)
fetch: 212992 bytes in 3 range(s), 1 retry, 0 restart(s), 61234 ms
fetch: check 4, the file's size and hash: passed
fetch: check 5, the descriptor inside the file: passed
fetch: check 6, the region: passed
fetch: stored: 0.3.0+abc1234 (twinoak-repeater-rak4630-single-868-v0.3.0.bin, 212992 bytes) is the candidate image; 'image list' shows it
```

A refusal at a check is one line - `fetch: refused at check 1, the signature: the key id names no
key this build trusts; nothing stored, the slot is empty` - and until the release key exists
(`keys/README.md`) that is what every normal build says, before a byte of the image is asked for.
A fetch that runs out of its budget says so with the bytes in flash - `fetch: stopped on the budget
with 131072 of 212992 bytes in flash` - and the next `fetch` of the same release resumes there
(`fetch: resuming at 131072 of 212992 bytes already in flash`). A file whose hash is not the
manifest's is refused at check 4 and the slot emptied, so nothing resumes into it.

```
> trust tls
trust tls: 2 roots; the modem's certificate slots are tracked in retained storage
  0. ISRG Root X1
     subject C=US, O=Internet Security Research Group, CN=ISRG Root X1
     sha256 96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6
     expires 2035-06-04; checked 2026-10-14 from https://letsencrypt.org/certs/isrgrootx1.pem
     certificate slot 12, TLS profile 2: written, matches this build's root
  1. USERTrust ECC Certification Authority
     ...
     certificate slot 13, TLS profile 3: written, matches this build's root
  hosts: github.com -> root 1; every other host -> root 0
  2 root(s) written in this boot
```

The slots are written at the first request of a boot whose record says they differ - a first
boot, a power cut (retained storage does not survive one), a release with another root - and not
otherwise; `trust tls` before that says *not written yet: written at the next request*.

## Sleep, and what wakes it

Task p3-04; `docs/power.md` for the band and the interval, `docs/ARCHITECTURE.md` §10 for the wake
a sleep ends.

**Three things wake a Walter, and one of them is not a wake source.**

| | |
|---|---|
| the timer | the check-in. The interval is the battery's (`docs/power.md`) unless `wake.interval` says otherwise |
| the radio's line | a start bit on `WALTER_RX` (GPIO8, an RTC pin), low level, with the chip's pull-up held through the sleep so that a radio which is off, resetting or being flashed cannot hold the Walter awake. `wake.radio off` turns it off |
| **USB: no** | this chip cannot wake from deep sleep when a cable is plugged in — `esp_sleep_enable_usb_wakeup()` answers `ESP_ERR_NOT_SUPPORTED` on the ESP32-S3 and the one USB wake ESP-IDF has is light sleep's. So the rule is: **a person presses reset, and a boot that finds a USB host stays awake** — while the host is there and for a minute after the cable goes. The boot says so in a line of its own, and `wake` and `status` repeat it |

A wake on the radio's line listens for up to §8's **whole repeat window** — about eleven seconds:
the radio sends `0x00 0x00`, waits 300 ms and then repeats the event every second, ten times, and
the bytes that woke us are gone by the time the UART is listening. A valid `EV_ATTENTION` inside
the window gives the wake its reason; nothing valid is a **spurious wake**, counted. After three
spurious wakes the line is **disarmed** as a wake source until the next timer wake, and that is
said at the boot, by `wake` and in the journal: a chattering line — a loose UART, a radio being
flashed, a scanner talking to nobody — must not be able to keep the Walter awake.

**The bound on a wake.** Without a USB host and without `stay`, a wake that has not reached sleep
within five minutes is ended by a reset; the next boot's record says who asked, the wake after it
reads that as a watchdog, and the interval policy lengthens the next sleep rather than shortening
it. A reset and not a straight sleep, because a wake that has overrun is a wake in an unknown
state, and the journal is what carries unfinished work across a boot. `main/` arms it once a boot
(task p3-05), so a wake that hangs anywhere ends in a reset rather than in a flat battery.

Two things may **raise** that bound for the wake in hand, and nothing else can: the supervisor's
ladder, whose waits add up to four minutes before it flashes anything, and an update, which the
task's own words allow to hold a wake open "up to its own bound" — a megabyte over a 100 kHz debug
port does not fit in five minutes. A raised bound is still a bound: a wake that overruns it is
reset, and the journal is what takes the work up again at the next boot (`docs/cycle.md`).

**What is written down.** Not every sleep. The journal is 1024 entries and it exists so that an
*update* is never guessed at; a unit checking in every quarter of an hour would wrap the whole log
in ten days. So an unremarkable sleep — the same band and the same interval as the last one — is
recorded in RTC memory only, which is where `wake` reads the last ten from, and the journal gets a
line when there is news: the band changed, the interval was lengthened by a fault, the radio's line
was disarmed or armed again, the wake was a crash or a watchdog, a person typed `sleep now`, or the
sleep was refused.

## One whole wake

Task p3-05; `docs/cycle.md` is the page — the ten steps, the dependency table, the budgets, what a
wake record's letters mean, and what an implementer of the backend seam has to provide.

**On a unit on a mast the console is not running.** A boot that finds no USB host runs one pass of
the wake cycle and sleeps; a boot that finds one stays awake, the console runs, and nothing sleeps
by itself. That is the same rule as before — this chip cannot wake from deep sleep on a cable — and
it is why `cycle now` and `cycle dry` exist: they are how a person with the board in hand sees
what a mast does by itself.

```
> cycle dry
what a wake would do, doing none of it - nothing is sent and nothing is written
   1 why      y done                       the timer: the check-in [up to 11300 ms]
   2 radio    y it would run               [up to 40000 ms]
   ...
  10 sleep    y it would run               [up to 15000 ms]
wake 12 timer: radio healthy, band normal, steps yyyyyyyyyy, 0 s awake, next 60 min
the wake: a dry run: nothing was done; the budget 240000 ms, 240000 left, 0 outside it; the bound 300000 ms
the backend: the exchange failed; 3 reading(s) offered
```

The last line is the seam, and on this firmware it is honest: **the backend does nothing.** The
implementation in this repository answers *nothing to do* every wake, for ever
(`core/cycle/null_backend.h`), because the server side is three accepted proposals and a decision
about where it will be built, not a protocol (`spec/SYSTEM.md` §8). The readings of every wake are
kept in a bounded ring and offered to it; nobody takes them, so the oldest are dropped and the
dropped count is kept.

## The radio's power, as `status` and `pins` say it

| | Means |
|---|---|
| `on` | the kill line is not driven and the settle time has passed since it was last released |
| `off (the kill line is driven)` | inside a `rak kill` |
| `unknown (the rail is settling)` | within the settle time after a kill was released or after boot (a Walter that reset in the middle of a kill left the line to float back on just before). The UART and SWD wait it out |

## What the port prints besides replies

**Almost nothing by itself.** The radio's events are collected whenever the main loop turns -
every 100 ms or less - and wait in the client's queue until `rak events` prints them; nothing is
printed as it arrives, because a Walter on a mast has nobody reading. ESP-IDF's own boot messages
come before the banner at the bootloader's log level.

The lines that *are* printed without being asked for are these, and `log.level` is what decides
which of them a port sees:

| At | What |
|---|---|
| `error` and above | a supervisor's stop (`supervisor: STOPPED: ...`), which is a unit that has given up on its radio: said at every level |
| `warn` and above | a settings store that would not open, or stored values this firmware will not have; an update the journal says was in progress and is being resumed |
| `warn` and above | ...and the radio's line disarmed by spurious wakes, and a run of crash or watchdog wakes (task p3-04): both are a unit behaving differently from the way it was provisioned to |
| `info` (the default) | the boot's own lines - the reset reason, the boot count, the settings summary, what this wake means and whether a USB host is holding it awake - and the supervisor's "today already" line |
| `debug` | nothing yet. The key exists because the agent firmware's console has it, and because the first thing phase 3's wake cycle will want is somewhere to put a wake's detail |

A resumed update's transitions and a self-update trial's steps are printed as they happen, from
the main loop, whatever the level: they are the answer to a command somebody typed before the
reset. The prompt and `type 'help' for the commands` are always printed.

Everything above goes into the 4 kB ring that `dump` prints, and so does every reply. What does
**not** go into it is anything the radio sent as text - a scanner's NDJSON line, a CLI reply, an
advert's name (S-12: "Nothing the mesh says is logged as text") - and the bytes of a binary
transfer. There is no secret in it, because there is no secret in any printed line.
