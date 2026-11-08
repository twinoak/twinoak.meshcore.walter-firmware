# Provisioning a Walter

What a person types to make a Walter theirs: the APN its SIM wants, the SIM's PIN if the card has
one, **the server it checks in with and the token it checks in by**, and a name so that the unit
says which unit it is. Over the module's USB-C and nothing else — not over the air, not over the box link,
not from a server ([`console.md`](console.md) is every command and every key).

**The session below is a host test.** `host_test/core/console/test_provisioning_doc.cpp` reads this
file, types every line after a `> ` prompt into the firmware's own console and compares what comes
back with the lines printed under it, so this document cannot drift from the firmware: a reply
that changes without this file changing fails the build. A line ending in `...` is a prefix, which
is how the lines carrying a time, a count or a version are written.

## Before the cable

- the unit built and flashed (`bench.md` §1), so that it boots and the console answers;
- the SIM in its holder, and the PIN from the operator if the card has one — **a card has three
  tries**, and this firmware never sends a PIN that has been refused once
  (`core/uplink/policy.h`, *The PIN*);
- the APN the operator wants, or nothing at all: unset is "let the network choose", which is what
  a Danish LTE-M SIM wants;
- **the server's URL and the unit's own token.** The token is made once, by a person, on
  meshmaster.dk: *Exit nodes -> Register an exit node* (`/exit-nodes/new`) gives this unit its
  identity on the server and a token nobody can read back afterwards — not from the page and not
  from this console. Copy it straight into `set server.token`. The same host is what a
  self-update's trial has to reach over TLS, so a unit with no server still updates itself on two
  conditions instead of three (`spec/SYSTEM.md` S-6, §8).

Nothing here needs a reboot: every `set` takes effect at once, and what it reaches is the right
column of [`console.md`](console.md)'s *What happens when one changes*.

## What the port says when the cable goes in

A unit out of the factory, before anything is set:

```
twinoak-walter 0.0.0-dev
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.2
settings: nvs namespace 'walter', 11 keys; 'get' prints them, 'set <key> <value>' changes one
settings: NOT encrypted - flash encryption is the owner's choice and is not assumed: a person with this board and a cable can read lte.pin and server.token out of it (docs/console.md)
uplink: the modem on UART2 behind interfaces::Uplink; not started yet ('lte', 'lte up')
link: UART1 115200 8N1 on the radio's two pins, open
boot 1: power-on reset
the boot record starts here (a power cut, or the first boot)
settings: 0 of 11 keys set; log.level info, swd 125 kHz ('get')
type 'help' for the commands
>
```

## The session

```console
> get
lte.apn unset
lte.pin unset
lte.psm unset (off)
server.url unset
server.token unset
fetch.url unset
wake.interval unset (auto)
wake.radio unset (on)
swd.khz unset (125)
log.level unset (info)
unit.name unset
> set unit.name Window at Grenaa
ok unit.name
at once: what `status` and the boot line call this unit
> set lte.apn lpwa.telia.iot
ok lte.apn
the APN is used the next time the uplink is brought up ('lte up')
> set lte.pin 1234
ok lte.pin
the next uplink sends it; a refused PIN is never sent a second time
> set server.url https://meshmaster.dk
ok server.url
a self-update's trial now has to reach meshmaster.dk:443, and the agent's calls go there
> set server.token mma_hRkQ2x7LpV4nT8yW1sB3dF5gH9jM0cZ
ok server.token
kept for the agent's calls ('agent probe', and every wake's heartbeat); nothing else reads it
> get lte.pin
lte.pin set
> get server.token
server.token set
> get
lte.apn lpwa.telia.iot
lte.pin set
lte.psm unset (off)
server.url https://meshmaster.dk
server.token set
fetch.url unset
wake.interval unset (auto)
wake.radio unset (on)
swd.khz unset (125)
log.level unset (info)
unit.name Window at Grenaa
> status
twinoak-walter...
unit: Window at Grenaa; role manager, board walter, band lte-m, link 1.2
booted: power-on reset; uptime...
radio: no session (nothing has been asked of it since this boot: try 'rak hello')
radio health:...
battery: never read (POWER_GET has not answered)
link:...
pins: power on;...
  uart released,...
images: golden=empty candidate=empty scanner=empty base=empty
uplink:...
backend: meshmaster.dk:443, the agent's server and a self-update's third condition
this wake: power-on;...
next wake: at the end of this wake; last band no-reading, wake.interval auto; line armed
> set server.url http://meshmaster.dk
error: server.url: the scheme must be https:// (S-12: TLS is verified)
> set wake.interval 3
error: wake.interval: auto, or 5..10080 minutes
> set lte.pin 12
error: lte.pin: 4..8 digits
> set swd.khz fast
error: swd.khz: not a whole number: fast
> get meshmaster.url
error: unknown key (try 'help'): meshmaster.url
> erase
error: erase forgets every setting; to do it, type: erase confirm
>
```

The value of `lte.pin` and of `server.token` is typed once and **echoes as asterisks** — the
transcript above shows the line as the test types it, not as the port echoes it — and is printed
by nothing afterwards: `get` says `set`, and no log line, journal entry, `status` or `dump` has
it. The four `error:` lines are what a wrong value gets, and nothing is stored by one.

## After it

1. `lte up` — **minutes**: an attach from cold is slow. It says how far it got, and `lte` after it
   says what the last attempt did (`console.md`, *The uplink*). A card whose PIN was wrong is
   `sim-locked`, and the PIN is not sent again until a different one is set.
2. `lte time` — the network's time, which is what sets the radio's clock in a wake.
3. `trust tls` — the roots this build trusts, and whether the modem's certificate slots hold them.
4. Pull the cable, put it back in, and `get`: the same answers. The settings are in the `nvs`
   partition, which is above the application slots, so a reflash of the application keeps them
   (`ARCHITECTURE.md` §11). A **merged** flash of the whole table does not.
5. `erase confirm` is how a unit is handed on: every setting forgotten, and only the settings —
   the images, the journal, the bad list and the supervisor's counts stay.

## The agent's first call

Once the uplink is up (`lte up`, step 1 above), two lines say whether the token is right — and
they are the last thing typed at the cable before the unit is left alone.

**This block is a host test too**, replayed by `host_test/agent/test_uplink_console.cpp` against
the firmware's own console over a scripted meshmaster.dk; it is marked `console-agent` because it
needs the uplink and a server under the console, and the `...` rule is the same. The agent's
number and label are the server's, so they are whatever the register says; the milliseconds and
the bytes are the run's.

```console-agent
> agent url
agent url: https://meshmaster.dk
  X-Agent-Version: ...
  server.token set
> agent probe
agent probe: GET https://meshmaster.dk/agent/v1/config with the token (the chain is verified against this build's roots)
agent probe: /agent/v1/config -> http 200 ok,...
  agent 3 "TwinOak Risskov companion", transport native, budget 2/min gap 20 s, poll 3600 s, serverTime 2026-09-23T14:00:01Z
```

- **`http 200 ok` and a config line** is the token accepted and this unit recognised: the agent's
  id and label are what `/exit-nodes` shows beside it, `transport native` is meshmaster.dk's word
  for an exit node that *is* its repeater, and the server's time should be within a few seconds of
  a watch.
- **`http 401 not ok`, *the token was refused*** is a token that was mistyped or that belongs to
  another unit. Nothing of the server's body is printed and the token itself is printed by nothing
  at all — `dump` afterwards holds no byte of it. Type `set server.token` again.
- **`error: agent probe: no reply: …` naming the handshake** is TLS: the chain the host offers
  does not end at a root this build carries (`trust tls`), and the token never went on the wire.
- Then **look at the page**: `/exit-nodes` shows this unit *last seen* just now, and after the
  first `agent wake` it shows it asleep with its repeater's key beside it. If the page still says
  the unit has never been seen, the 200 above was a different unit's token.

What happens next needs no cable: the unit wakes on its own interval, heartbeats, claims the jobs
the server has for it and sleeps again ([`field.md`](field.md)). `docs/bench.md` §§17–18 is the
walk that watches it the first time.

## A factory radio

Every RAK4630 arrives with RAK's own firmware (RUI3), not the MBR, SoftDevice S140 6.1.1 and
bootloader a TwinOak application is linked against. Giving it that **base** is the one thing this
firmware does to a radio that nothing automatic may ever do (`spec/SYSTEM.md` S-5a): it erases
the chip whole — RUI3, any identity it had, UICR — so a person types it, naming the radio, and
types the word. Once, per radio, at the bench, with the radio on the cable (`bench.md` §4a).

**The session below is a host test too**, replayed by
`host_test/core/provision/test_provision_console.cpp` against the firmware's console over a
simulated nRF52840 — a revision 3 module (`AAF0`) as RAK ships it, its access port open — with the
base and a candidate already pushed over the cable. The blocks are marked `console-provision`
because this session needs a simulated chip under the console; the `...` rule is the same as
above. The device id is the simulated chip's; a real one prints its own, and that is what gets
typed.

Before it: the base in the store (`python tools/walterctl.py push --port COM7 --as base
<release-dir>` of a repeater release that carries a `provision` file — `console.md`, *The board's
base*), and the radio's first application in the candidate slot (`push --as candidate`). Then
`rak swd id` to read the radio, `rak provision check` to be told exactly what to type, the typed
line, and `rak update --first-install`:

```console-provision
> rak swd id
swd: 125 kHz: connect, power-up, control port, memory port, FICR
  DPIDR 0x2ba01477 (the nRF52840's)
  control access port: IDR 0x02880000 (Nordic); protection: open
  memory access port: IDR 0x24770011 (Arm, class MEM-AP; the Cortex-M4's AHB-AP)
  chip: nRF52840, variant AAF0, flash 1024 KB = 256 pages of 4096 bytes (INFO.FLASH 1024 KB)
  device id 0badf00ddeadbeef
  core: running
  power-up request withdrawn; pins released;...
> rak provision check
rak provision check: slot 0, rak4630-single; nothing is written
  base: sha256 ...
  battery: unknown (no reading over the link); accepted, as a first install accepts it
  swd: DPIDR 0x2ba01477; control access port IDR 0x02880000; protection: open
  chip: nRF52840, variant AAF0: improved APPROTECT (build code F, revision 3): UICR.APPROTECT = 0x5A
  device id 0badf00ddeadbeef
  next: rak provision 0badf00ddeadbeef confirm
> rak provision 0badf00ddeadbeef confirm
rak provision: slot 0, 0badf00ddeadbeef: S-5a's seven steps; nothing is written before step 4, each printed as it ends
  [1/7] checked: the base sha256 ...
        battery unknown (no reading over the link: accepted); the slot is free (...
  [2/7] attached: DPIDR 0x2ba01477, protection open; nRF52840 AAF0, device id 0badf00ddeadbeef, as typed (...
  [3/7] variant AAF0: improved APPROTECT (build code F, revision 3): UICR.APPROTECT = 0x5A (...
  [4/7] erased: CTRL-AP ERASEALL; the port open after it (no soft reset needed); flash, NRFFW[0] and APPROTECT blank; the core halted (...
  [5/7] written: 54 flash words in 4 runs, 2 UICR words, UICR.APPROTECT = 0x0000005a (...
  [6/7] verified: 56 words read back and equal; UICR.APPROTECT reads 0x0000005a (...
  [7/7] reset: one pin reset (waited 0 ms for the spacing), the bootloader given 1500 ms; the port after it open, NRFFW[0] 0x000f4000 (...
rak provision: PROVISIONED slot 0: device id 0badf00ddeadbeef, variant AAF0, base sha256 ...
  the slot awaits its first install: the supervisor leaves the radio alone until 'rak update --first-install' commits
  next: python tools/walterctl.py push --port <port> --as candidate <release-dir>
        rak update --first-install
> rak update --first-install
rak update: slot 0; every transition is printed as the journal takes it (this takes minutes:
  the flash, then a 180 s proving period)
update: check -> flashing-candidate: the preconditions hold
  reading the pages the image covers: page 1/1 (0x00026000)
  writing and verifying the first page: page 1/1 (0x00026000)
  reading the whole image back: page 1/1 (0x00026000)
update: flashing-candidate -> booting: flashed; the radio was reset
update: booting -> proving: the radio answered
update: proving -> committing: the health check passed
update: committing -> done: the candidate is the golden image
rak update: done: ...
  (the clock check was skipped: this Walter had no time of its own in this boot, so
   there was none to give the radio. 'lte up' before 'rak update' lets it run)
>
```

What the seven steps are, and what each refusal means, is `console.md`, *The radio's base*. The
things worth knowing at the bench:

- **Nothing is written before step 4.** Every refusal of steps 1 to 3 — no base in the store, a
  base that no longer passes check 5p, the battery low, the radio held by an update or the
  ladder, a part that is not an nRF52840, the wrong device id, a variant the table does not know —
  leaves the radio exactly as it was. `rak provision check` makes every one of them before
  anything is typed.
- **A locked radio is named `locked`**, and a locked radio that still runs an application is
  named `locked:<key16>` — the first 16 hex digits of the public key `rak id` prints — so that a
  radio with an identity is named before it loses it. `rak provision check` prints the right form.
- **A failure after step 4 leaves the radio erased, and the same command finishes it.** There is
  no journal: the command starts from erased, so it is simply run again. After a power cut
  `rak provision check` says what it sees — a revision 3 part whose flash has something in it and
  whose `UICR.APPROTECT` is not yet `0x5A` reads *closed* again, so the line to type becomes
  `locked` — and that line is the one to type.
- **Until `rak update --first-install` commits, the slot awaits its first install**: the radio has
  a bootloader and no application and answers nothing on the link, and the supervisor leaves it
  alone instead of climbing its ladder (`rak watch` and `status` say so). A first install takes
  the **candidate**, and the image that passes its health check becomes golden.
- **The port after the reset** is reported *open* or *CLOSED*. Closed on a revision 3 part means
  the RAK's bootloader does not copy `UICR.APPROTECT` into `APPROTECT.DISABLE` at start-up
  (`spec/SYSTEM.md` §10.2): the radio works — a UF2 drive on its USB — and the flasher cannot
  reach it until that is solved. It is said, not failed.

## What is not provisioned here

- **the radio's own settings.** A repeater's frequency, bandwidth and admin password are the
  repeater firmware's, over the box link or its own console; `rak cli <line>` is the way in from
  here.
- **the release key.** What this firmware accepts an update signed with is a build constant,
  generated into the image from `keys/release-keys.txt` (`trust` prints it). A console cannot
  change what a unit trusts.
- **anything a server sends.** Settings over the uplink are out of scope for phase 3
  (`tasks/phase-3/03-settings-and-console.md`); a unit is provisioned by a person with a cable.
- **flash encryption.** The owner's choice, and not assumed: `console.md`, *Secrets*.
