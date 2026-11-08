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
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.2
settings: nvs namespace 'walter', 11 keys; 'get' prints them, 'set <key> <value>' changes one
settings: NOT encrypted - flash encryption is the owner's choice and is not assumed: a person with this board and a cable can read lte.pin and server.token out of it (docs/console.md)
uplink: the modem on UART2 behind interfaces::Uplink; not started yet ('lte', 'lte up')
https: 2 TLS roots (keys/tls), the modem's slots are recorded as written
image store: rakfw at 0x630000 (6144 KB), 6242304 bytes laid out for 2 radio slots x 3 images and a base; 'image list' says what is in it
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
| `status` | one screen: this firmware and what its descriptor says it is; the unit's name; why this boot happened, the uptime and the boot count; the radio's session - its role, version, slot, **band** (`not said` from a 1.0 radio) and link version - its health and clock; the battery as the radio last reported it; the link's counters; the pins in one line; the images the store holds; this firmware's own slot and trial; the uplink's last attempt and when the next one is allowed; the backend; the next wake. **No secret and no IMEI** — the subscription's identity is `lte id` and nowhere else |
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
| `rak kill <seconds> [confirm]` | the LoRa 3.3 V rail off for that long: every other radio-facing pin released first (the UART and SWD taken back from whoever held them), the kill line driven high, held, released (never driven low), then the settle time, then the safe state. Whole seconds, 1 to 30 — the bounds are placeholders until the bench measures the rail (`bench.md` §6). **Over 5 seconds it wants the word `confirm` after the number**: a repeater off the air for half a minute is a decision, not a typo. The console is busy for the duration |
| `rak hello` | HELLO: the version, role, slot, band and capabilities the radio answers with |
| `rak ping` | PING, with the round trip |
| `rak status` | STATUS: its uptime, why it booted, its clock, its flags and counters |
| `rak id` | IDENTITY: the node's public key and advert name. The name is printed and not logged (S-12) |
| `rak time [set <epoch>]` | TIME_GET; `rak time set <epoch>` is TIME_SET from a person at this console, which is `spec/LINK-PROTOCOL.md` §5.4's third source of time |
| `rak power` | POWER_GET: the three current-sense channels |
| `rak env` | ENV_GET: temperature, humidity, pressure |
| `rak cli <line>` | CLI: one line run by the radio's own console interpreter, and what it answered. The reply is printed and not logged |
| `rak request <type> [hex data]` | LOCAL_REQUEST (`spec/LINK-PROTOCOL.md` §5.9, link 1.2): one request of the radio's own handler, answered as it answers an admin over the mesh and with nothing on the air — `01` status, `05` access list, `06` neighbours, `07` owner info. The answer is printed as hex, and a status also field by field |
| `rak events` | the events the client has queued |
| `rak text` | the lines a scanner has sent instead of frames (§9), printed and not logged |
| `rak swd connect` | acquire the SWD lines, connect, print DPIDR and the power-up, release |
| `rak swd id` | connect, identify the nRF52840 (FICR), its protection and its core's state, release |
| `rak swd mem <addr> [words]` | read 1 to 32 words at a word-aligned address (hex `0x…` or decimal), read-only |
| `rak flash-test confirm` | a two-page pattern at the top of the application region, only if those pages are blank: written, verified, erased again; the radio restarted. **It wants the word**: it is the one command that writes to a radio's flash outside the update machine |
| `rak provision check` | no write, ever: attach to the radio and say whether its access port is open (and its device id) or closed (and whether an application answers on the link), what the variant table would do, the base in the store, and the exact line to type next |
| `rak provision <DEVICEID>\|locked\|locked:<key16> confirm` | give a factory radio its base (`spec/SYSTEM.md` S-5a): a chip erase through the CTRL-AP, the base's flash and UICR words through the one door, every word read back, one pin reset. **It erases the radio whole** — RUI3, any identity, UICR — so it wants the radio named as `rak provision check` says and the word; the slot then awaits its first install (minutes; *The radio's base* below, `provisioning.md`) |
| `rak update` | flash the candidate, prove it, make it golden — or roll back to the golden image and never try that candidate again (minutes; `docs/update.md`) |
| `rak update --first-install` | the same with no golden image to return to: a person's command only. A committed first install is also what ends a slot's *awaiting its first install* after `rak provision` |
| `rak update status` | the journal's story of the last update of the radio |
| `rak role scanner\|repeater` | flash the stored scanner image and prove a scanner is there; or the golden image back, proved as an update would be |
| `rak watch` | the supervisor's view: whether the radio is healthy, when it was last so, today's counts against the daily caps, the last good battery reading, the radio's restarts, a stop in force, and the last three incidents with every rung they tried. It only reads |
| `rak watch reset-caps` | the day's counts to zero and a stop lifted, journalled (the bench) |
| `rak ladder` | climb the supervisor's ladder now — ask, reset, power-cycle, look at the chip, stop — each rung printed as it is tried. Minutes: the waits are the real ones |
| `rak mesh status <key>` | one status request to a neighbouring repeater over the mesh client (task p4-02): begin a job with its key, send, print the reply's numbers, and the radio's `client_tx` before and after — the bench's *one packet, one reply, up by one*. Up to sixteen seconds on silence |

### Images, trust and this firmware's own slots

| Command | What it does |
|---|---|
| `image list` | what the image store holds, per radio slot: the layout's arithmetic on the first three lines, then each slot's **golden**, **candidate** and **scanner** image — role, board, band, version, size, load address, the first eight bytes of its SHA-256 and which physical area it is in, then when it was stored, the commit it was built from and the key that signed its manifest — or `empty`, or `empty; a transfer of sha256 … stopped at N of M bytes (it can be resumed)`. Then the slot's **base** (see below), on two lines of its own. Slot 1's areas are listed too, with a line saying this adapter has no radio there |
| `image verify <which>` | read that image back out of flash, hash it as it comes, and hold it to the SHA-256 in its record. `<which>` is `golden`, `candidate`, `scanner` or `base`, with an optional `@slot` after it. This is the command that finds flash rot before an update does. For `base` it also re-runs **check 5p** — `spec/FIRMWARE-MANIFEST.md` §4a's seven rules — over the stored bytes, against this build's own region for the board |
| `image erase <candidate\|scanner\|base>` | invalidate that slot's record and clear its header sector, so the slot reads empty and a half-finished transfer is not resumed into the next image. **Never the golden image** (S-5: a known-good image is always on hand), and the refusal says so. The base may be erased: nothing depends on its being there. Not to be confused with `erase confirm`, which is the settings |
| `image bad` | the hashes of candidates that failed their health check (task p2-03), up to eight; an image on this list is refused before a sector is erased. It is about images: a base is not health-checked (S-5a reads every word back instead) and is not held to this list |
| `image receive <which>` | hand this port over to a framed binary transfer and take `manifest.json`, then its `.sig`, then the image — or, with `base`, the release's `provision` file — see below |
| `trust` | (task p2-01) first a self-test of the signature arithmetic on this board — `ed25519 self-test (RFC 8032 test 1, and one bit changed): passed, <n> ms` (TweetNaCl on the ESP32-S3: expect some hundreds of milliseconds; **write the number down**) — then the key ids this build accepts a release manifest signed with: the trust table generated from `keys/release-keys.txt` (`spec/FIRMWARE-MANIFEST.md` §3, room for three). Since the release key was made (2026-10-20, `spec/SYSTEM.md` section 10 item 1) it prints `trust: 1 key this build accepts a release signed with: 73efdd383a957596`. While `keys/release-keys.txt` names no key it prints `trust: none: no update will be accepted (keys/release-keys.txt names no key; the release key is made once, by a person: spec/SYSTEM.md section 10 item 1)`, which is the safe state until the key exists. A build made with `-DTWINOAK_TRUST_TEST_KEY=1` (`tools/verify.ps1 -TestKey`) lists the specification's public test key `c17969deb0026955` and adds a `WARNING` that it is a test build (`+TESTKEY` in its version) that must never be released; the same warning is printed at every boot, after the descriptor line |
| `trust tls` | the TLS roots this build trusts — name, subject, SHA-256 fingerprint, expiry, source, the date they were checked — which certificate slot and TLS profile of the modem each goes to, whether the modem's slots are recorded as holding them, and which host is held to which root |
| `self status` | (task p2-04) this Walter's two application slots — which runs, at what address, in which of the bootloader's states (*valid*, *pending-verify*, *aborted* …), which version — then the trial if one is running (how far into the window, which of the three conditions are met), the image received and waiting for `self switch` if any, and the last self-update's story from the journal. See *The Walter's own update* |
| `self receive` | hand this port over to the same transfer as `image receive`, with the other application slot as its target: a release of **this** firmware's `ota` file, through the six checks (`tools/walterctl.py push --as self`) |
| `self switch [radio-down]` | make the received image the next to boot: the preconditions, the journal's *switching* entry, the boot pointer, `SLEEP_NOTICE` to the radio, restart. Does not come back. `radio-down` says the radio is known to be down, so the trial's second condition does not apply |
| `self confirm` | end the running image's trial early: valid (the bench) |
| `self reject` | end it early: rejected, on the bad list, restart into the previous image (the bench) |

### The uplink

| Command | What it does |
|---|---|
| `lte` | (task p3-01) the uplink: what the modem is, what the last attempt did and how far it got, when the backoff allows the next one, whether a SIM PIN is set and whether it was refused, the last signal reading, the last time, and what power saving the network granted. Since task p9-02 it ends with the socket rotation's own lines: the two socket ids the HTTPS connections take turn and turn about, how many connections this boot has made and which id the last few were on, and the close accounting — see *HTTPS*. **It only reads** — it does not start the modem |
| `lte up` | bring the uplink up now: the whole sequence inside one wake's budget. **Minutes** — an attach from cold is slow — and it goes ahead even when the backoff would have waited, because a person asked. The network's time, when it gave one, becomes **this Walter's own clock** (task p9-04), as a wake's step 6 does with it: so the link has a time to give the radio (§5.4) and `rak update`'s clock check can be made |
| `lte down` | the modem's radio off (`AT+CFUN=0`); the context goes with it |
| `lte time` | the network's time, or a refusal in words when the modem answered with its own default |
| `lte id` | the modem's IMEI and the SIM's ICCID and IMSI, for the label on the box. **The only place this firmware prints them**, and they are in no log line, no journal entry and no `status` |
| `http get <url>` | (task p3-02) one GET over TLS whose chain is verified against this build's roots: the status, the headers worth naming (`Content-Type`, `ETag`, `Last-Modified`, `Accept-Ranges`, `Content-Length` or chunked), the first 32 bytes as hex, the size, the time, and (task p9-02) the socket id each of this request's connections took and what the close accounting did. The response is read to 1 KB and cut there. `https://` only; the uplink must be up (`lte up` first). See *HTTPS* |
| `fetch [base-url] [candidate\|scanner]` | a release from `<base-url>/manifest.json` and `manifest.json.sig` into the store's candidate (default) or scanner image: the three steps, each of the six checks printed as it passes, the image in ranges with every range's line; resumable across wakes. **Minutes.** Never the golden image. With no base URL on the line the `fetch.url` setting is used, and `fetch` says so |
| `agent url` | (task p4-03) the server the agent's uplink is configured for (`server.url` as it is used, a trailing slash off), whether `server.token` is set, and the `X-Agent-Version` header every call to it carries - this firmware's own version. It reads; it does not call |
| `agent probe` | (task p4-03) one `GET /agent/v1/config` now, with the token: the status, `Retry-After` and `X-RateLimit-Remaining` when the answer carried them, and the config the server answered (the agent's id and label, its transport, the budget, the poll interval, the server's time). The bench's first look at a server; the uplink must be up (`lte up` first). See *The agent's uplink* |
| `agent id` | (task p4-04) who this exit node says it is: the repeater's key and advert name from `IDENTITY`, its firmware and board from `HELLO`, the radio block from `get radio` over the link's `CLI`, the job types the heartbeat advertises, the local door's four counters (task p9-04) and which of §5.9's four requests it answers (task p9-06), what this wake promises the server, the clock and the heap. **Nothing of it is typed by a person**, and the token is *set* or *unset* as everywhere else. One `IDENTITY` and one `CLI` request at most, and none when nothing has changed. See *The agent's identity* |
| `agent heartbeat` | (task p4-04) the body `POST /agent/v1/heartbeat` would carry, built and **not sent**: the bench's way to compare it with meshmaster.dk's contract fixture by eye. It asks the radio nothing |
| `agent obs [pull]` | (task p4-04) the observation ring the agent posts from: how many adverts wait, how many were heard, how many were dropped (the figure the heartbeat carries), what the radio's own ring has lost and how much is still in it, and the last five - the key, how long ago, the hops, the signal and the role, and **never the name** a stranger chose (`spec/LINK-PROTOCOL.md` §7). `agent obs pull` drains the radio's ring into it first (§7's `OBS_PULL`, as many pages as it answers) |
| `agent wake` | (task p4-05) one wake of the agent's loop now, through the backend seam the wake cycle's step 8 runs: **the radio asked who it is first** - a `HELLO` when there is no session and then `IDENTITY`, so that a job for the unit's own repeater can be told from a neighbour's (task p8-04) - then the offline buffer flushed, one heartbeat with this wake's intent, the claim, and the jobs the claim handed over: a neighbour's over the mesh, the unit's own through the local door, a firmware job into a slot or onto the chip. It restores the loop's retained state before the wake and saves it after, as a real wake across a sleep does. The uplink must be up (`lte up` first); **no `rak hello` is needed before it**, and a radio that does not answer one leaves the wake claiming only this unit's own `twinoak.*` job types, which one line says. See *The agent's identity* |
| `agent jobs` | (task p4-05) the last ten jobs as the agent's library remembers them: the id, the type, the outcome, how many transactions each took and how long, and whether a result is still waiting in the offline buffer for the next wake to post. Then (task p9-04) the two blocks the library cannot write: `local`, the door's counters and this radio's key shortened, and `begun`, the last ten jobs **by the repeater they named** - the local door, or the mesh and whose key it was. **A job the local door answered shows `0` transactions** (task p9-05): it spends no airtime and waits for none, so there is nothing of the budget to count, and the whole of a 34-line `config.read` of this unit's own repeater now takes seconds. It reads; it runs nothing |
| `agent state` | (task p4-05) the loop's retained state as it stands: whether it has ever heartbeated, the poll interval it will sleep by, any hold a server asked for, the counters the heartbeat carries, and whether a kept image is waiting in the agent's flash window. It reads; it runs nothing |

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
| `server.url` | `https://host[:port][/path]`, the scheme required, at most 128 characters | none: the agent makes no call, and a self-update's third condition does not apply |
| `server.token` | printable, at most 160 characters; **secret**, never printed | none: the agent makes no call (one comes from `/exit-nodes/new` on meshmaster.dk) |
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
| `server.url` | at once: the host the agent's calls and a self-update's trial must reach. A URL with no host this firmware can reach — a port over 65535, say — leaves no server configured, and the trial's third condition does not apply rather than failing |
| `server.token` | kept for the agent's calls ('agent probe', and every wake's heartbeat); nothing else reads it |
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
| `rak hello` | `HELLO`: the protocol version agreed, the role, the slot, the largest payload, `boot_id`, the firmware, board and upstream versions, then the **band** on a line of its own - `band 868` from a radio that speaks link 1.1, `band: not said (a 1.0 radio)` from one that does not (`spec/LINK-PROTOCOL.md` §5.1; the band is the third of the three words an image is chosen by) - and the capability bits by name. Says so when the radio's clock has not been set since it booted |
| `rak ping` | `PING` with a nonce made from this board's clock; prints whether it came back, the radio's uptime, the round trip and how many attempts it took |
| `rak status` | `STATUS`: uptime, why it booted, its clock and time state, its flags by name, the observations waiting and lost, the frames its receiver dropped, and any pending attention |
| `rak id` | `IDENTITY`: the node's MeshCore public key and its advert name |
| `rak time` | `TIME_GET`: the radio's clock and where it came from |
| `rak time set <epoch>` | `TIME_SET` with source *a person at a console* (§5.4's source 3) and never *force*: a clock that goes backwards makes a MeshCore node's own traffic look replayed, and the radio refuses a backwards step — which is the right answer to a typed mistake. Prints the clock before and after |
| `rak power` | `POWER_GET`: the three current-sense channels — panel, battery (positive = charging), total load — in millivolts and milliamps, with the shunts already applied by the radio, and which of them were read |
| `rak env` | `ENV_GET`: temperature, humidity and pressure, and which of them were read |
| `rak cli <line>` | `CLI`: the rest of the line, 1 to 160 bytes, run by the radio's own console interpreter as if typed on its serial port, and its reply. A line that restarts the radio (`reboot`) never answers, and the console says that is expected |
| `rak request <type> [hex data]` | `LOCAL_REQUEST` (§5.9, link 1.2): one request of the radio's **own request handler**, run as it runs one that arrived over the mesh from a client logged in as admin, with nothing on the air and no job needed. The type is two hex digits — `01` the status, `05` the access list, `06` the neighbours, `07` the owner info — and the data an even number of hex digits, at most 32 bytes (`06`'s page is chosen by it: version, count, offset, order, prefix length). Prints the status in words and the answer as hex, and for a status the fields by name: battery, uptime, transmit queue, noise floor, last RSSI and SNR, packets in and out by flood and direct, airtime, duplicates and errors. *the handler answered nothing* is §5.9's `ERR_RANGE`, which over the mesh would have been silence; *the radio does not advertise the capability* is a radio older than link 1.2, and then only the mesh yields these four answers |
| `rak events` | the events the radio has sent and the client has queued — `EV_BOOT`, `EV_MESH_REPLY`, `EV_ATTENTION` — decoded, newest last, and how many have been seen, dropped for want of room, or lost to a gap in their sequence numbers. Printing them takes them out of the queue |
| `rak text` | the lines a scanner has sent instead of frames (§9), and how many were seen, dropped or truncated. Printing them clears them |

| `rak mesh status <key>` | the mesh client (§6), through `components/agent/`'s `LinkMesh` (task p4-02): `JOB_BEGIN` with the neighbour's key and no path (the send floods, and the reply teaches the route), `SEND_REQUEST` of a status, the wait for `EV_MESH_REPLY` with `REPLY_GET` when it does not come, `JOB_END`, and `MESH_COUNTERS` before and after. Prints how the packet went and its tag; the reply's own fields (§6.3: after how long, via a returned path or direct, SNR and RSSI); the repeater's stats as the agent's codec reads them — battery, uptime, noise floor, last RSSI and SNR, packets, airtime — or that the reply was not a status reply it reads; and `client_tx` before and after. The neighbour answers a bare status request only if it holds this unit's key in its ACL (`setperm` on its console): a login is the agent's business (task p4-05), not this command's. On silence the console is busy for the runner's flood wait, sixteen seconds, and `rak events` afterwards shows nothing of it: the reply events are the mesh client's and are taken out of the queue |

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
`candidate`, `scanner` or `base`; `--image <id>` picks one of several images in a manifest;
`--slot N` names the radio (0 today). It needs `pyserial` (`pip install pyserial==3.5`).

What happens on the Walter, in this order:

1. `image receive candidate` prints what the transfer will accept — **role, board and band**, so
   that a mistake is visible before any bytes move — and **which version of the transfer it
   speaks**, `(protocol 2)`, and then **the port is no longer a console**: the bytes are frames,
   nothing is echoed and there is no prompt.
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
   is the record written, by one small final write. For a base it is **checks 4 and 5p**: a base
   carries no descriptor, so §4a's seven rules stand in place of checks 5 and 6.
6. The console comes back with one line saying what happened — `image receive: stored`, or the
   number of the check that refused it and why — the chunk counters, and a prompt. A second
   counting line appears only when something went wrong with the port itself: *`N` answers the
   port would not take whole*, which to the laptop looked like answers that never came.

**The transfer's version, and a Walter at 0.1.0** (task p8-02). The answers are frames and go out
as bytes, verbatim: that is **protocol 2**, which the ready line and the transfer's hello both say
(`walter image receive 2`). Up to 0.1.0 they went out through the console's text output, which
turns every `0x0A` into CR LF for a terminal — and the answer that names chunk 10 carries `0x0A`,
as does a byte count of 2,640 and one CRC in 128. So a push of more than nine chunks into a Walter
at 0.1.0 could not finish: `no answer to type 0x03 seq 9`, for ever
([`tasks/phase-6/FOLLOWUPS.md`](../tasks/phase-6/FOLLOWUPS.md), 6 November). `walterctl` reads the
version off the ready line, before it sends a frame, and puts that conversion back out of the
answers when — and only when — the Walter says `protocol 1` over a serial port; it then holds the
hello's own version word against that and stops if the two disagree. **Nothing needs to be typed**:
`--undo-crlf` forces the same thing for a Walter whose ready line cannot be read, and the hello's
word stops it being used against a Walter that does not need it.

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
file whose size or SHA-256 is not the manifest's, a manifest with no `swd` file (or no `provision` file, with `--as base`), a directory that
is not a release.

### The board's base

A factory RAK4630 arrives with RAK's own firmware, not the MBR, SoftDevice S140 6.1.1 and
bootloader a TwinOak application is linked against. Those three, and the UICR words they need, are
the board's **base**: one Intel HEX file per board, carried by a release of the repeater firmware
as its image's `provision` file (`spec/FIRMWARE-MANIFEST.md` §4a).

The Walter keeps one base per radio slot, beside the three images, and **storing one writes
nothing to a radio**: giving a radio its base is `rak provision <device id> confirm`, typed by a
person (`spec/SYSTEM.md` S-5a), and nothing automatic can reach it.

```
> image list
...
  base       the board's base, 471028 bytes, sha256 a3f1c0d4e5b60718..., bootloader 0x000f4000
             S140 6.1.1, 192512 bytes of flash, 2 UICR words, the MBR's parameter page at 0x000fe000
             stored at 1792502400 (unix), from commit 1a2b3c4…, signed by 73efdd383a957596

> image verify base
base: 471028 bytes read back and hashed in 142 ms: the SHA-256 is the one in its record
base: check 5p over the stored bytes: a base for rak4630-single (0x00026000 to 0x000ed000), bootloader 0x000f4000,
  192512 bytes of flash in 5 runs, 2 UICR words - the same as its record says
```

What a base may hold is §4a's seven rules, and the Walter applies them against **its own** region
for the board (`0x26000`–`0xED000` for `rak4630-single`) and not against the manifest's: a release
cannot widen what a radio's flash is. A file that breaks a rule is refused with that rule's own
words — the same words the specification's reference gives, so the three readers of a base (the
release tooling, the Walter, MeshMaster's catalogue) cannot disagree:

```
> image receive base
...
image receive: refused: check 5p, the base: 0x00026000 lies between the SoftDevice and the bootloader: the application and the file system are not the base's
```

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

## The radio's base: `rak provision`

From task p5-03 (`spec/SYSTEM.md` S-5a; `provisioning.md`, *A factory radio*, is the session,
replayed by a host test; `ARCHITECTURE.md` §4 is where the sequence sits). A factory RAK4630 runs
RUI3 and has none of the MBR, SoftDevice and bootloader our images are linked against. The base
(§4a) is in the store (`image receive base`, *The board's base* above); this command puts it on
the radio, and it is **the only thing in this firmware that erases a chip or writes UICR** —
through one door that nothing automatic can reach (`README.md`, the rails).

| Command | What it does |
|---|---|
| `rak provision check` | *(`rak @0 provision check`)* the base in the store, with check 5p run again over the stored bytes; the battery as the radio reports it, or *unknown* (accepted, as a first install accepts it: a factory radio answers nothing); HELLO on the link; then the debug port: `DPIDR`, the control port's `IDR`, and the access port **open** — `FICR`: part, variant and what the table makes of it, the device id — or **CLOSED**, with whether an application answers on the link. The last line is the exact line to type. Anything the command would refuse is said: *the command would be refused at step 1: no base in the store*, *… at step 3: INFO.VARIANT is not in the table*. **Nothing is written** |
| `rak provision <radio> confirm` | `<radio>` is the device id as `rak swd id` prints it (16 hex digits, read without regard to case) for a radio whose port is open; `locked` for a closed one that runs no application; `locked:<key16>` — the first 16 hex digits of the public key `rak id` prints — for a closed one whose application answers, so that a radio with an identity is named before it loses it. Then the seven steps below, each printed as it ends, and one outcome line. Without the word it prints what it would do and the line to type; a word that is none of the three is refused before a register is read |

The seven steps, each a name in the outcome so that a refusal says where it ended:

1. **check** — the base: in the store for this slot, passing check 5p again (the store is flash),
   its bytes its record's; the radio not held by an update with a run in hand or by the
   supervisor's ladder; the kill line not driven; HELLO on the link; the battery above the
   update's threshold and not falling (an unknown reading is accepted). The slot is taken and
   held for the whole command. **Refused here: nothing touched.**
2. **attach** — `DPIDR`, the control port's `IDR`, `APPROTECTSTATUS`. Open: `FICR` — refused if the
   part is not an nRF52840, the typed id is not this radio's, or `locked` was typed. Closed: no
   answer on the link → only `locked`; an answer → only `locked:<key16>` with that key. The door
   then checks the port half of this again against the chip itself. **Refused here: nothing
   touched.**
3. **variant** — the table (`components/core/provision/variant.h`, with its sources): build code
   A–D legacy (`UICR.APPROTECT` left erased), F improved (revision 3; `UICR.APPROTECT = 0x5A`),
   anything else refused. On an open radio this comes before the erase, with nothing touched;
   on a closed one the identity is readable only after the erase, and a variant outside the table
   then **stops the command with the radio erased** — it has lost nothing it had not lost
   already; extend the table, run the same command again.
4. **erase** — CTRL-AP `ERASEALL`; the port read back; closed → **one** CTRL-AP soft reset (the
   only reset until step 7), attach again; still closed → stop. Blank check: flash word 0,
   `NRFFW[0]` and `UICR.APPROTECT` read `0xffffffff`. The core halted.
5. **write** — the base's words through the door, in runs: flash first, then UICR, then
   `UICR.APPROTECT` with the table's kind. No pin, power-on or brownout reset between the erase
   and this last write: on a revision 3 part the port stays open only until one.
6. **verify** — every word read back and compared; `UICR.APPROTECT` against the table's value. A
   difference stops the command and names the first address, the base's word and the radio's;
   the radio is then safe to provision again.
7. **reset** — the halt released, one pin reset through the pins' own rules (open drain; at least
   a second since the last pulse, waited for, never skipped — `rak reset` just before it delays
   it), the bootloader given 1500 ms, attach again: the port **open** (with `NRFFW[0]`) or
   **CLOSED** after it. Closed on a revision 3 part is `spec/SYSTEM.md` §10.2 — the bootloader
   does not copy `UICR.APPROTECT` into `APPROTECT.DISABLE` — reported, not a failure.

The outcome: `rak provision: PROVISIONED slot 0: device id …, variant …, base sha256 …, port
after the reset open; N s in all`, then that the slot awaits its first install, then the two
lines to type next — `python tools/walterctl.py push --port <port> --as candidate <release-dir>`
and `rak update --first-install`: a first install flashes the **candidate**, and the image that
passes its health check becomes golden. A refusal is one line, `error: rak provision: refused at
step N (<name>): <why>`, followed by *nothing was erased* or *the radio is erased: run the command
again from the top*. There is no journal: a failure after step 4 leaves the radio erased, which
is what the command starts from, so the same command (as `rak provision check` says it — after a
power cut a revision 3 part with something in flash reads closed again, and the line becomes
`locked`) finishes it. The whole of it goes to the log ring `dump` prints.

**Awaiting its first install.** From the moment before the erase until a `rak update
--first-install` commits, the slot is marked so in the supervisor's retained record and in the
journal (a Walter restart and a power cut keep it): a radio with a bootloader and no application
is silent on the link by design, and the ladder — a reset, a power cycle, a golden image — is not
climbed for it. `rak watch` and `status` say *awaiting its first install*. Nothing else clears
the mark; a new `rak provision` sets it again.

What the host tests proved — every factory state (legacy open and locked, revision 3 open and
locked, with a bootloader that opens the port and one that does not) provisioned word for word
with one pin reset; every refusal with the chip's write and erase counters at zero; a power cut
at every flash and UICR operation and at the transfers between them, each followed by the line
`rak provision check` says and a finished radio; and the supervisor, an update and a simulated
week of wakes against a factory radio with the simulated chip's tripwire armed — is in
`tasks/phase-5/reports/03.md`.

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
| `self receive` | `self receive: this Walter's other application slot (1); this port is now a framed binary transfer (protocol 2)` and what will be accepted (role manager, board walter, band lte-m, an ota file that fits the slot); the transfer ends with `self receive: stored` or `self receive: <refusal>`. Refused in one `error:` line when the running image is on trial, a radio update is in progress (S-7) or a transfer is open |
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
  time: 1791936086 unix seconds, from the network; this Walter's clock is set from it, so the radio can be given it too
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

Since v0.1.3 (task p10-05) **every request either line makes carries `X-Agent-Version`**, this
firmware's own version, whatever the host: meshmaster.dk refuses an `/agent/v1` request without it
with a 400, and the download sent none until this release. It is not a credential — the
`User-Agent` has carried the same product and version since task p3-02 — so there is no host it is
withheld from, and `fetch` of a release on github.com still carries **no token**, as before. A
request a server refuses is named by its status and its file, with the server's own sentence after
it when it answered problem details: `error: fetch: a request failed: http 400 for manifest.json:
Every request carries X-Agent-Version: the agent's build.` A `detail` that holds the token is
dropped whole.

```
> http get https://meshmaster.dk/
http get: https://meshmaster.dk/ (the chain is verified against this build's roots)
  GET meshmaster.dk/ -> 200 ok, 1024 bytes, 2831 ms
http get: 200 the response is larger than the bound: cut off there
  Content-Type: text/html; charset=utf-8
  Transfer-Encoding: chunked
  first 32 bytes: 3c21444f43545950452068746d6c3e0a3c68746d6c206c616e673d22656e223e
  1024 bytes (cut off: a console line reads 1 KB at most) in 2831 ms
  sockets: ids 1 and 6, turn and turn about; 7 connections in this boot
    this request: 1
    owed closes: 0 waited for, 0 timed out, 0 written off; 0 discounted; 0 events for an id not in the rotation
```

**The last three lines are the socket rotation's** (task p9-02,
`components/core/net/socket_rotation.h`). Consecutive connections take two of the modem's socket
ids turn and turn about, because a close belonging to the connection before this one arrives after
this one has been opened; these lines say which id each connection of this request took, how many
connections the boot has made, and what the close accounting did. `lte` prints the same block for
the whole boot (`the last few: 1 6 1 6`), which is where a person looks after a request that
stalled. What each figure means:

| Figure | What it says |
|---|---|
| `ids 1 and 6` | the two the rotation holds. `id 1 alone (the library gave out no second one)` is the degraded case: the library would not reserve a second socket, so every connection is on one id and the owed-close rule carries it alone (`components/modem/walter_tls.h`) |
| `this request: 1 6` | the id of each connection this one line made, in order. A redirect makes two |
| `waited for` / `timed out` | connections that began on an id still owing a close: how many waited for it, and how many of those gave up after 250 ms |
| `written off` | debts that were still unpaid when the connection that waited had finished: the modem never reported that close. The id is then one *whose closes do not come* and costs nothing more until one arrives, which is the bound task p9-02 put on the review's S2 |
| `discounted` | late closes that paid a debt instead of ending the connection in progress. These are the 6 November fault being caught rather than happening |
| `events for an id not in the rotation` | a `+SQNSRING` or `+SQNSH` naming a socket this firmware is not using. Should be zero |

A line follows for an id that owes a close (*the next connection on it waits 250 ms for it*) or
that expects none (*one was owed and written off, and none has arrived since*).

A redirect is followed (at most three, only to `https://`) and the last host named: `http get: 200
ok after a redirect to release-assets.githubusercontent.com`. A refusal is one `error:` line: *only
https URLs are accepted*, *the uplink is down: 'lte up' first*, or - when the modem would not
connect - *no response: the modem refused the connection: the host was unreachable or the TLS
handshake failed (the library does not say which)*. That last wording is honest about the modem
library at 1.5.0, which answers one `ERROR` for a name that does not resolve and for a chain it
cannot verify; `docs/bench.md` §13 is where the two are told apart on a desk, by choosing hosts.

Two more refusals are worth telling apart, and since task p9-02 the console does: *the connection
closed before the body was complete* means a close really arrived before the response was whole -
the other end hung up - while *the modem stopped answering for this socket before the body was
complete* means it did not: the modem answered an error to a receive, or nothing at all before the
library's own 30 s command timeout (`WalterModem: Command time-out (TX)` on the line above it).
Both were the first sentence until v0.1.1, which is why the two stalls of 7 November read as a
server hanging up when they were one AT command going unanswered.

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

## The agent's uplink

From task p4-03 (`components/agent/modem_uplink.h`; `docs/ARCHITECTURE.md` §13). Two lines, no
slot; `agent probe` needs the uplink up (`lte up` first) and both `server.url` and `server.token`
set, and is refused in one line otherwise. Every call the agent makes to meshmaster.dk goes the
same road this command does: one request of the HTTPS client above, the chain verified, the token
read from `server.token` for the call and wiped after it, `X-Agent-Version` this firmware's own.

```
> agent url
agent url: https://meshmaster.dk
  X-Agent-Version: 0.4.0+1a2b3c4
  server.token set
```

`agent url` reads and does not call. The token is never printed: *set* or *unset*, as `get
server.token` says it. Without a URL it says `server.url unset (the agent makes no call)`, and
*not configured* until both are there.

```
> agent probe
agent probe: GET https://meshmaster.dk/agent/v1/config with the token (the chain is verified against this build's roots)
agent probe: /agent/v1/config -> http 200 ok, 186 bytes, 2831 ms, via lte
  X-RateLimit-Remaining: 119
  agent 3 "Grenaa unit", transport native, budget 2/min gap 20 s, poll 3600 s, serverTime 2026-10-22T14:00:01Z
```

What a status prints is what the agent's policy will do with it (`agent/ServerPolicy.h`): a `401`
is `-> http 401 not ok` and *the token was refused* (the loop halts on it); a `429` or a `503`
prints the `Retry-After` the server named; a `403` is *this unit is refused*. No byte of an error
body is ever printed - the line is the status and a sentence of this firmware's own. A host whose
chain this firmware does not carry, or a host that does not answer, is one `error:` line: `agent
probe: no reply: the TLS handshake failed: the chain, the name or the version` - a failure of the
handshake, not of the server, which `docs/bench.md` §17 is where to tell apart on a desk.

## The agent's identity

From task p4-04 (`components/agent/identity.h`, `walter_platform.h`, `observations.h`;
`docs/ARCHITECTURE.md` §13). Three lines, no slot, and **nothing in them is typed by a person**:
every field comes from the radio over the box link or from this firmware's own tables.

```
> agent id
agent: version 0.4.0+1a2b3c4, board walter, transport native
repeater: 7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c
  name "TwinOak Risskov", firmware 0.1.0, board rak4630-single, built on repeater-v1.17.1+d9296435, boot id 0x1a2b3c4d
radio: 869.618 MHz, bw 62.5 kHz, sf 8, cr 8
job types (13): status, telemetry, neighbours, config.read, config.apply, cli, trace, discover, advert.request, clock.sync, password.set, acl.set, reboot
local door: open: a job naming this radio is answered here; 2 job(s), 3 request(s), 1 answered with silence, 0 with no request to make
  from LOCAL_REQUEST: status, access list, neighbours, owner info (telemetry stays LPP_GET)
intent: next check-in 3600 s, at most 37 transaction(s) this wake (band normal)
clock: 1791936000, from the network
heap: 176880 free, 160224 the least ever
server.token set
the link said: 1 identity read(s), 1 radio read(s), 0 restart(s) of the radio
```

Where each of them comes from: the key and the advert name are §5.6's `IDENTITY`, the firmware,
the board and what it is built on are §5.1's `HELLO`, and the radio block is one `CLI` line -
`get radio`, which is the only line of the repeater's console that carries all four numbers. The
key is read once a boot and again whenever `boot_id` changes; the radio block again after that,
or after a `set` line has crossed the link for the unit's own repeater. **The `local door: open`
line is also what it costs** (task p9-05): a job naming this radio is answered here, so it needs no
password at any class — nothing is stored on meshmaster.dk for this unit's own repeater — and it
spends no airtime, so `agent jobs` shows it at `0` transactions. **The `from LOCAL_REQUEST` line
under it is what the door can answer** (task p9-06, link 1.2): with that capability in the session
a status, an access list, a neighbour list and the owner info of this unit's own repeater come
out of the radio's own request handler over the link (§5.9), and without it — a radio older than
link 1.2 — the line says so and those four are what they were before: three *failed* and one
silence, which is what kept the first unit's repeater *stale* on meshmaster.dk. **A radio with no identity
yet** - a RAK with no application, or one turned into a scanner - is not a failure: the unit still
checks in, and the line says that the heartbeat will carry no key, no radio block and no job types,
which is how a unit registered before its repeater says *radio not identified*.

**And a radio may refuse, which is the third thing and not the second** (task p10-03). From
repeater `v0.1.2` a node whose file system would not mount makes no key and says so: `IDENTITY`
answered `ERR_STATE`, `caps 0x21`, an attention of reason 6 detail 1. `agent id` then prints *the
radio refuses: its identity is lost*, the evidence it read it from, and what a person does -
`docs/flashing.md` §3a first, because the old key cannot be recovered, then `rak cli erase` and
`rak cli reboot`, which make the radio a **new node**. The last line counts the refusals beside
the answers with no key (*1 of them a refusal (ERR_STATE)*), the heartbeat carries
`radioIdentityLost: 1` among its `errors` - meshmaster.dk's *Counters* row shows it - and an
update applied to such a radio ends *node-fault* without spending a rollback (`docs/update.md`).
A radio awaiting its first install is unchanged: it needs its install, and this one needs a
person.

**Every wake reads this before it claims anything** (task p8-04). A timer wake's step 2 opens the
session; `agent wake` has no step 2 and opens one itself, so neither needs a `rak hello` typed
first, and one line of the log says which of the two happened:

```
agent: the radio is known (boot 0x1a2b3c4d): a job for this unit's own repeater goes through the local door, and every job type this unit serves is offered
agent: the radio did not answer a HELLO: only this unit's own job types are claimed (twinoak.fw.offer, twinoak.fw.apply, twinoak.fw.state, twinoak.power.cycle); a mesh job this end cannot tell is its own is not claimed
```

Until the key is there the job type list is **empty of mesh types**, and the server routes by that
list - so a radio that is silent, or one that has made no identity yet, leaves the unit claiming
only its own `twinoak.*` jobs rather than claiming a mesh job it would then have to refuse. `agent
id` asks nothing of a radio with no session, so it is still `rak hello` or a wake that gives it
one. The key is never written to the log: the boot id is what identifies the session
(`spec/SYSTEM.md` S-12).

```
> agent heartbeat
agent heartbeat: POST /agent/v1/heartbeat, 571 bytes, built and not sent
{"agentVersion":"0.4.0+1a2b3c4","transport":"native","at":"2026-10-14T00:00:00Z", ... }
```

The body is the library's own writer over the identity above, so it is the bytes a wake would
post - to be held against meshmaster.dk's `tests/fixtures/contracts/AgentHeartbeat.json` by eye on
the bench (`docs/bench.md` §17). It asks the radio nothing and sends nothing.

```
> agent obs pull
agent obs: the radio's ring drained: 2 record(s) kept
agent obs: 2 of 64 waiting, 2 heard, 0 dropped (the heartbeat's figure)
  the radio: 0 dropped since it booted, 0 still waiting there, 1 pull(s) made
  the last few, newest first (no names):
   1. 7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c 300 s ago, 2 hop(s), snr -2.50 dB, companion
   2. a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90 41 s ago, 0 hop(s), snr 7.25 dB, rssi -84 dBm, repeater, with a position
```

**No advert name is printed, ever.** §7 says it in so many words - "the name inside `app_data` is
something a stranger chose. Neither end logs it" - and every line of this console is kept in the
four kilobytes `dump` prints, so a name printed here would be a name written down. The key, the
age, the hops, the signal, the role and *whether* there was a position are what crosses into the
console; the name itself goes into the ring and out again in the batch the agent posts.

`agent obs` without `pull` reads the ring and asks the radio nothing. The two drop figures are
different things: the ring's is what **this** firmware had to overwrite and is the one the
heartbeat carries as `observationsDropped`, and the radio's is what the RAK's own ring lost since
it booted - the increase in it between two wakes is added to the first, once, so that a loss one
hop away is still a number on the unit's page.

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
