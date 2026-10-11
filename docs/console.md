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
running from ota_0 at 0x010000 (3072 KB), ota state undefined (flashed over USB, not by an update; rollback does not apply)
image descriptor at 0x3c03ac70: role manager, board walter, band lte-m, version 0.0.0-dev, link 1.0
link: UART1 115200 8N1 on the radio's two pins, open
boot 1: power-on reset
the boot record starts here (a power cut, or the first boot)
type 'help' for the commands
>
```

Before the first line, the seven radio-facing pins have already been put in the safe state:
constructing `RadioPins` is the first thing `app_main` does.

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

A refusal is one line starting with `error:` and names what was wrong: an unknown command or
subcommand, an argument where none is taken, a missing or non-numeric duration, a line over 200
characters, a control character.

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

## The radio's debug port, over SWD

From task p1-03. The two SWD lines (GPIO2 SWDCLK, GPIO4 SWDIO) are acquired for the length of
the command and released on every exit path; the clock is 125 kHz (a half-period of 4 µs, the
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
