# Provisioning a Walter

What a person types to make a Walter theirs: the APN its SIM wants, the SIM's PIN if the card has
one, the server it will talk to and the token it will use, and a name so that the unit says which
unit it is. Over the module's USB-C and nothing else — not over the air, not over the box link,
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
- the server's URL, and its token if it has one. There is no backend yet
  (`spec/SYSTEM.md` §8), so today the server is what a self-update's trial has to reach over TLS
  and nothing more.

Nothing here needs a reboot: every `set` takes effect at once, and what it reaches is the right
column of [`console.md`](console.md)'s *What happens when one changes*.

## What the port says when the cable goes in

A unit out of the factory, before anything is set:

```
twinoak-walter 0.0.0-dev
running from ota_0 at 0x010000 (3072 KB), ota state valid
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.0
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
a self-update's trial now has to reach meshmaster.dk:443; the backend call itself is task p3-05's
> set server.token mma_hRkQ2x7LpV4nT8yW1sB3dF5gH9jM0cZ
ok server.token
kept for the backend call (task p3-05); nothing else reads it
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
unit: Window at Grenaa; role manager, board walter, band lte-m, link 1.0
booted: power-on reset; uptime...
radio: no session (nothing has been asked of it since this boot: try 'rak hello')
radio health:...
battery: never read (POWER_GET has not answered)
link:...
pins: power on;...
  uart released,...
images: golden=empty candidate=empty scanner=empty
uplink:...
backend: meshmaster.dk:443, for a self-update's third condition (task p3-05 makes the call)
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
