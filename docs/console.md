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
| `status` | the version; the uptime and the boot count; why this boot happened; what is believed about the radio's power; whether the UART, SWD, nRESET and the kill line are in use; how long until the next reset is allowed; every pin's mode and level on one line |
| `reboots` | why this board last restarted and the four before that, newest first, each with how long the run before it lasted. The record is in RTC memory: it survives a `reboot`, a panic, a watchdog and a deep sleep, and a power cut clears it (and the first line says so) |
| `pins` | the seven pins, one line each: net, D-sub pin, GPIO, mode as this firmware configured it, the level read from the pad, and what it is in use for (`released`, `uart`, `swd`, `reset`, `kill`); then the radio's power |
| `rak reset` | one reset pulse: nRESET open-drain low for 100 ms, then released. If the last reset ended less than a second ago the command says how long it waits and waits; it never pulses early (two resets inside half a second put the radio into its UF2 bootloader). The first reset after a boot waits a full second, because the Walter's own power-up may have reset the radio just before |
| `rak kill <seconds>` | the LoRa 3.3 V rail off for that long: every other radio-facing pin released first (the UART and SWD taken back from whoever held them), the kill line driven high, held, released (never driven low), then the settle time, then the safe state. Whole seconds, 1 to 30 — the bounds are placeholders until the bench measures the rail (`bench.md` §2). The console is busy for the duration |
| `reboot` | restart the Walter; the pins go to the safe state first and `reboots` after it says *asked for on the console* |

A refusal is one line starting with `error:` and names what was wrong: an unknown command or
subcommand, an argument where none is taken, a missing or non-numeric duration, a line over 200
characters, a control character.

## The radio's power, as `status` says it

| | Means |
|---|---|
| `on` | the kill line is not driven and the settle time has passed since it was last released |
| `off (the kill line is driven)` | inside a `rak kill` |
| `unknown (the rail is settling)` | within the settle time after a kill was released or after boot (a Walter that reset in the middle of a kill left the line to float back on just before). The UART and SWD wait it out |

## What the port prints besides replies

Nothing else yet: there is no log line and no event in this task. ESP-IDF's own boot messages come
before the banner at the bootloader's log level.
