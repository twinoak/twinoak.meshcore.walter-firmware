#!/usr/bin/env python3
"""Boot a release's factory image in the pinned image's QEMU, before anything is signed.

    python tools/release/boot_gate.py --factory out/release/twinoak-walter-v0.1.4-factory.bin \\
        --elf out/images/twinoak-walter.elf --version 0.1.4+1a2b3c4 [--work out/verify/boot-gate]

Exit 0 when all three of these hold, exit 1 with the reason when one does not:

  1. the image reached `app_main`;
  2. the `heap:` line was printed, with the main task's stack taken (task p11-02: the second line
     of every boot, and the one v0.1.3 never got to print). It is asked of every image that has
     that line's format string in its bytes, which is every build since 0.1.4 and none before;
  3. the banner names the version being released.

**Why this exists.** v0.1.3 was built, signed, verified against every check of
spec/FIRMWARE-MANIFEST.md §5 and published without ever having started anywhere - and it could
not start at all: ESP-IDF could not create its main task, every boot ended in
`assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)`, and a unit on a mast
found out over LTE (tasks/phase-11/README.md). Task p11-02's rail now refuses such a build from
its map, its DWARF and its sdkconfig, which is the cheapest place to catch it. This is the other
half: the file that would be published is actually started, on the chip model it is for, before
the manifest is signed. What QEMU cannot give - the modem, the radio, a trial's conditions - is
not asked for. The gate is *it starts*, and nothing more.

**How it reads the boot: gdb, not the console.** QEMU's esp32s3 machine has no USB Serial/JTAG
peripheral, and this firmware's console is that peripheral (sdkconfig.defaults, the module's USB-C
is the ESP32-S3's own USB). Two things follow, and both were measured in the pinned image
(tasks/phase-11/reports/04.md):

  * UART0 carries the ROM loader's lines (`ESP-ROM:esp32s3-20210327`, `entry 0x403c8948`) and
    nothing of ESP-IDF or of this firmware, because the console is not UART0;
  * every stdout byte is dropped inside ESP-IDF before it reaches a wire:
    `usb_serial_jtag_write()` of $IDF_PATH/components/esp_driver_usb_serial_jtag/src/
    usb_serial_jtag_vfs.c returns -1 at once when `usb_serial_jtag_is_connected()` is false, and
    in QEMU no USB host is ever connected.

So the gate breaks on that function and reads the buffer it was handed - the bytes the firmware
printed, before they are dropped - which is exactly what a cable would have shown. The same way
tasks p11-01 and p11-02 read the heap out of this firmware in this QEMU: a gdb script over the
ELF, with the breakpoints where the documents say the values are.

**Three hooks, and what each costs.** The ESP32-S3 has two instruction breakpoints (hardware),
and gdb cannot write a software breakpoint into flash. Each hook is set at the instruction *after*
the function's `entry` (3 bytes, the windowed ABI's prologue - Xtensa ISA Reference, `ENTRY`), so
that the arguments are in a2, a3, a4 as the ABI puts them. Every run disassembles the three
prologues into its `gdb.log`, which is the evidence for that +3; `--show-gdb` prints the script
and the QEMU command line as well.

  | hook | where | why there |
  |---|---|---|
  | `app_main` + 3 | flash, one hardware breakpoint, deleted as soon as it is hit | criterion 1 |
  | `usb_serial_jtag_write` + 3 | flash, the hardware breakpoint app_main gave back | a3 is the buffer and a4 its length: the console's own bytes |
  | `esp_system_abort` + 3 | IRAM, a software breakpoint, so it costs no hardware one and stays armed for the whole run | a2 is the panic's own sentence - this is how v0.1.3 is refused by name |

**The bound.** The window closes at whichever comes first: the write that begins `heap:` (the
boot's second line, so everything the gate asks for has been printed by then - the usual case, a
second or two), or `--seconds` of wall clock, 90 by default. A panic ends it at once. An image
with no `heap:` line in it - every release before 0.1.4 - runs to the bound, and so does one
whose stdout is written in pieces rather than a line at a time: the transcript is complete
either way, and only the waiting is longer. Nothing here waits for the firmware's main loop, the
modem or the radio.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# The three figures of app_main's second line (task p11-02, main/main.cpp). The gate asks for the
# third one to be nonzero: the main task's stack is taken by the time its own task prints this, so
# a zero there would mean the line is not the line this gate thinks it is.
HEAP_LINE = re.compile(r"^heap: (\d+) bytes free, largest free block (\d+) \(internal, 8-bit\), "
                       r"the main task's (\d+)-byte stack already taken")
# That line's format string, as it is in the image's own bytes. An image that does not carry it
# cannot print the line, and is not asked to: v0.1.2 and every release before 0.1.4 are such
# images, and the gate still judges them on app_main and the banner. Every build this repository
# makes now has it, and rail R22 holds main/main.cpp to printing it.
HEAP_FORMAT = b"heap: %lu bytes free"
# The banner: core/version/version.cpp is `"twinoak-walter " TWINOAK_VERSION`, printed by app_main
# as a line of its own (`std::printf("\n%s\n", core::banner())`).
PRODUCT = "twinoak-walter"
BANNER_LINE = re.compile(r"^" + re.escape(PRODUCT) + r" (\S+)$")
# A frame of gdb's own backtrace at the panic. The assert's text is redacted in QEMU - ESP-IDF's
# __assert_func ($IDF_PATH/components/newlib/src/assert.c) replaces every string that lives in
# DROM with "<cached disabled>", and the function it names with the caller's address, whenever
# spi_flash_cache_enabled() is false, which it is in this emulator - so the backtrace, read from
# the ELF rather than from the chip, is what says where the panic was.
FRAME = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+ in )?(\S+) \(.*?\)(?: at (\S+))?")
# The frames between the assert and the code that asserted: they are the same in every panic and
# say nothing about this one.
PANIC_PLUMBING = ("esp_system_abort", "panic_abort", "__assert_func", "abort")

# The chip, its RAM and the flash a Walter has (sdkconfig.defaults: ESP32-S3-WROOM-1-N16R2,
# 16 MB of quad SPI flash). A factory image is flashed at 0x0 and the rest of the flash is erased,
# which is 0xFF.
MACHINE = "esp32s3"
RAM = "32M"
FLASH_BYTES = 16 * 1024 * 1024
ERASED = 0xFF
# An ESP-IDF image begins with the second-stage bootloader's own 0xE9 magic byte
# (esp_image_format.h, ESP_IMAGE_HEADER_MAGIC). A file that does not is not a factory image, and
# would boot nothing at all.
IMAGE_MAGIC = 0xE9
# The eFuse block QEMU keeps in a file of its own. Zeros are a module out of the factory: no
# secure boot, no flash encryption, nothing burned. tasks/phase-11/reports/04.md: this firmware
# boots in this QEMU with 1 024 zero bytes there.
EFUSE_BYTES = 1024
# The Xtensa windowed ABI's prologue: `entry` is three bytes and has no narrow form, so the
# instruction after it is the first one at which a2..a7 hold the call's arguments (Xtensa ISA
# Reference Manual, ENTRY; and `--show-gdb` prints the three functions' first instruction).
ENTRY_BYTES = 3
# "heap" as the first four bytes of a write, little-endian, which is how the gdb script tells the
# boot's second line from every other write without reading strings on the target.
HEAP_WORD = 0x70616568

GDB_SCRIPT = """\
# The boot gate (tools/release/boot_gate.py). Addresses resolved from the ELF by that script, so
# that no name here can be ambiguous; +%(entry)d is the instruction after each function's `entry`.
set pagination off
set confirm off
set print address off
set height unlimited
# The prologue every hook below rests on: `entry` first, three bytes, then the instruction
# at which a2..a4 are the call's arguments. Disassembled out of the ELF in every run - before
# the target is attached, where the flash is not mapped yet - as the evidence for the +%(entry)d.
x/2i %(abort_at)#x
x/2i %(app_main_at)#x
x/2i %(write_at)#x

target remote :%(port)d

# A panic, wherever it happens: esp_system_abort(const char *details) is in IRAM, so a software
# breakpoint reaches it and it costs neither of the chip's two instruction breakpoints. This is
# the hook that names what killed a boot - `assert failed: esp_startup_start_app
# app_startup.c:86 (res == pdTRUE)` for v0.1.3.
break *%(abort)#x
commands
silent
printf "GATE-PANIC %%s\\n", $a2
echo GATE-WHERE\\n
bt 6
detach
quit
end

# Criterion 1: the image reached app_main. One hardware breakpoint, given back at once.
hbreak *%(app_main)#x
continue
printf "GATE-APPMAIN\\n"
delete 2

# The console: usb_serial_jtag_write(int fd, const void *data, size_t size), one call per
# printf, before ESP-IDF drops the bytes for want of a USB host. a3 is the buffer, a4 the length.
hbreak *%(write)#x
commands 3
silent
eval "append binary memory %(console)s %%u %%u", $a3, $a3 + $a4
if *(unsigned int *)$a3 == %(heap_word)#x
  printf "GATE-HEAP\\n"
  detach
  quit
end
cont
end
continue
printf "GATE-STOPPED-EARLY\\n"
detach
quit
"""


class Refused(Exception):
    """Why this image may not be signed. The message is for a person reading a workflow log."""


def tool(name):
    """A tool of the pinned ESP-IDF image, or a refusal that says where it lives."""
    found = shutil.which(name)
    if not found:
        raise Refused("%s is not on the path. The boot gate runs inside the image "
                      "tools/idf-image.txt pins (QEMU is /opt/esp/tools/qemu-xtensa/...), the "
                      "way tools/verify.sh runs everything else." % name)
    return found


def symbols(elf, wanted):
    """The address of each name in the ELF, through the image's own nm. A name that is missing, or
    that two symbols answer to, is a refusal: the gate would otherwise hook the wrong code."""
    out = subprocess.run([tool("xtensa-esp32s3-elf-nm"), elf], check=True, text=True,
                         capture_output=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in ("T", "t"):
            continue
        address, name = parts[0], parts[2]
        if name in wanted:
            found.setdefault(name, []).append(int(address, 16))
    for name in wanted:
        hits = found.get(name, [])
        if len(hits) != 1:
            raise Refused("%s has %d symbol(s) named %s; the boot gate needs exactly one, "
                          "because it breaks on its address" % (elf, len(hits), name))
    return {name: hits[0] for name, hits in found.items()}


def flash_image(factory_path, path, flash_bytes=FLASH_BYTES):
    """The factory image as a whole flash: it is flashed at 0x0 and the rest of the chip is
    erased, so everything above it is 0xFF - which is what the partition table's own gap is
    (partitions.csv) and what a module out of the box has."""
    with open(factory_path, "rb") as fh:
        data = fh.read()
    if not data:
        raise Refused("%s is empty" % factory_path)
    if data[0] != IMAGE_MAGIC:
        raise Refused("%s does not begin with 0x%02X, so it is not an ESP-IDF image flashed at "
                      "0x0 (the release's -factory.bin is)" % (factory_path, IMAGE_MAGIC))
    if len(data) > flash_bytes:
        raise Refused("%s is %d bytes, more than the %d of this board's flash"
                      % (factory_path, len(data), flash_bytes))
    with open(path, "wb") as fh:
        fh.write(data)
        fh.write(bytes([ERASED]) * (flash_bytes - len(data)))
    return len(data)


def prints_heap_line(path):
    """Whether this image carries app_main's `heap:` line at all, read out of its own bytes
    (HEAP_FORMAT). Every build since 0.1.4 does; v0.1.2 and v0.1.3 do not, and the gate does not
    ask an image for a line it cannot print."""
    with open(path, "rb") as fh:
        return HEAP_FORMAT in fh.read()


def free_port():
    """A port for QEMU's gdb server. Asked of the kernel rather than fixed, so that two gates (a
    verify run and a release run on the same machine) cannot take each other's."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def panic_reason(gdb_log):
    """The panic's own sentence and, after it, the frame that asserted - taken from gdb's
    backtrace, because the sentence itself is redacted in this emulator (FRAME above). "" when
    the boot did not panic."""
    panic = ""
    for line in gdb_log.splitlines():
        if line.startswith("GATE-PANIC "):
            panic = line[len("GATE-PANIC "):].strip()
    if not panic:
        return ""
    for line in gdb_log.splitlines():
        match = FRAME.match(line.strip())
        if match and match.group(1) not in PANIC_PLUMBING:
            where = match.group(1)
            if match.group(2):
                where += " at " + os.path.basename(match.group(2))
            return "%s - in %s" % (panic, where)
    return panic


def judge(console, gdb_log, version, expect_heap=True):
    """What the boot showed, against what a release has to show. `console` is what the firmware
    handed its console, decoded; `gdb_log` is what the gdb script printed; `expect_heap` is
    whether this image carries the `heap:` line at all (HEAP_FORMAT). Returns (ok, lines) - the
    lines are the gate's own account, for the log and the workflow summary.

    This is the whole of the gate's reading of a boot, and it is a pure function of two strings so
    that tools/release/test_release.py can hold it to a passing and a failing transcript without
    QEMU."""
    lines = []
    ok = True

    def says(fine, text):
        nonlocal ok
        lines.append(("ok    " if fine else "NO    ") + text)
        if not fine:
            ok = False

    panic = panic_reason(gdb_log)

    # 1. app_main. The one thing v0.1.3 could not do, and the reason this gate exists.
    reached = "GATE-APPMAIN" in gdb_log
    if reached:
        says(True, "the image reached app_main")
    elif panic:
        says(False, "the image never reached app_main: it panicked first - %s" % panic)
    else:
        says(False, "the image never reached app_main, and did not panic either: it is stuck "
                    "before the firmware's own code (the bootloader, or the start-up)")

    # 2. the banner, with the version being released in it.
    banners = [m.group(1) for m in (BANNER_LINE.match(ln) for ln in console.splitlines()) if m]
    if not banners:
        says(False, "the banner '%s <version>' was never printed" % PRODUCT)
    elif version in banners:
        says(True, "the banner names the version being released: %s %s" % (PRODUCT, version))
    else:
        says(False, "the banner says '%s %s' and the version being released is %s"
                    % (PRODUCT, banners[0], version))

    # 3. the heap line, and the main task's stack in it.
    heap = None
    for line in console.splitlines():
        match = HEAP_LINE.match(line)
        if match:
            heap = tuple(int(g) for g in match.groups())
    if not expect_heap:
        says(heap is None or heap[2] > 0,
             "this image has no 'heap:' line in it at all - it was built before 0.1.4, where "
             "task p11-02 added one - so the gate asks it for app_main and the banner only")
    elif heap is None:
        says(False, "the 'heap:' line was not printed (main/main.cpp prints it as the second "
                    "line of every boot; task p11-02)")
    elif heap[2] == 0:
        says(False, "the 'heap:' line says the main task's stack is 0 bytes")
    else:
        says(True, "the heap: line was printed, with the main task's %d-byte stack taken: "
                   "%d bytes free, largest free block %d" % (heap[2], heap[0], heap[1]))

    if panic and reached:
        says(False, "the image panicked after app_main: %s" % panic)
    return ok, lines


def boot(factory, elf, version, work, seconds, show_gdb=False, out=sys.stdout):
    """Boot `factory` in QEMU with `elf`'s symbols and judge what it printed. Returns
    (ok, lines)."""
    qemu = tool("qemu-system-xtensa")
    gdb = tool("xtensa-esp32s3-elf-gdb")
    os.makedirs(work, exist_ok=True)
    flash = os.path.join(work, "flash.bin")
    efuse = os.path.join(work, "efuse.bin")
    console_path = os.path.join(work, "console.bin")
    uart0 = os.path.join(work, "uart0.log")
    script = os.path.join(work, "boot_gate.gdb")
    qemu_log = os.path.join(work, "qemu.log")
    gdb_log_path = os.path.join(work, "gdb.log")
    for stale in (flash, efuse, console_path, uart0, qemu_log, gdb_log_path):
        if os.path.exists(stale):
            os.remove(stale)

    size = flash_image(factory, flash)
    expect_heap = prints_heap_line(factory)
    with open(efuse, "wb") as fh:
        fh.write(b"\0" * EFUSE_BYTES)
    where = symbols(elf, ("app_main", "usb_serial_jtag_write", "esp_system_abort"))
    port = free_port()
    with open(script, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(GDB_SCRIPT % {
            "entry": ENTRY_BYTES, "port": port, "console": console_path, "heap_word": HEAP_WORD,
            "abort_at": where["esp_system_abort"], "abort": where["esp_system_abort"] + ENTRY_BYTES,
            "app_main_at": where["app_main"], "app_main": where["app_main"] + ENTRY_BYTES,
            "write_at": where["usb_serial_jtag_write"],
            "write": where["usb_serial_jtag_write"] + ENTRY_BYTES,
        })

    print("boot gate: %s, %d bytes into a %d MB flash, version %s"
          % (os.path.basename(factory), size, FLASH_BYTES // (1024 * 1024), version), file=out)
    print("           %s on %s, gdb on port %d, at most %d s"
          % (os.path.basename(qemu), MACHINE, port, seconds), file=out)
    print("           app_main 0x%08x, usb_serial_jtag_write 0x%08x, esp_system_abort 0x%08x "
          "(+%d each: after `entry`)" % (where["app_main"], where["usb_serial_jtag_write"],
                                         where["esp_system_abort"], ENTRY_BYTES), file=out)

    # The same invocation tasks p11-01 and p11-02 read the heap with (tools/heap_programme.gdb),
    # with the flash made above instead of a build directory's: no network, no graphics, the timer
    # group's watchdog off, UART0 to a file and UART1 (the modem's and the radio's, as far as this
    # firmware is concerned) to nothing at all.
    command = [
        qemu, "-M", MACHINE, "-m", RAM,
        "-drive", "file=%s,if=mtd,format=raw" % flash,
        "-drive", "file=%s,if=none,format=raw,id=efuse" % efuse,
        "-global", "driver=nvram.%s.efuse,property=drive,value=efuse" % MACHINE,
        "-global", "driver=timer.%s.timg,property=wdt_disable,value=true" % MACHINE,
        "-nic", "none", "-nographic", "-monitor", "none",
        "-serial", "file:%s" % uart0, "-serial", "null",
        "-gdb", "tcp::%d" % port, "-S",
    ]
    if show_gdb:
        with open(script, encoding="utf-8") as fh:
            print(fh.read(), file=out)
        print(" ".join(command), file=out)

    started = time.time()
    gdb_log = ""
    with open(qemu_log, "wb") as log:
        machine = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        try:
            # QEMU is stopped at reset (-S) and gdb is what starts it; a moment for the gdb
            # server's port to be listening.
            deadline = started + 10
            while time.time() < deadline:
                with socket.socket() as probe:
                    if probe.connect_ex(("127.0.0.1", port)) == 0:
                        break
                time.sleep(0.2)
            try:
                run = subprocess.run([gdb, "-batch", "-x", script, elf], timeout=seconds,
                                     text=True, capture_output=True, stdin=subprocess.DEVNULL)
                gdb_log = run.stdout + run.stderr
            except subprocess.TimeoutExpired as expired:
                gdb_log = (expired.stdout or b"").decode("utf-8", "replace") \
                          + (expired.stderr or b"").decode("utf-8", "replace") \
                          + "\nGATE-TIMEOUT after %d s\n" % seconds
        finally:
            machine.terminate()
            try:
                machine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                machine.kill()
    elapsed = time.time() - started
    with open(gdb_log_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(gdb_log)

    console = b""
    if os.path.exists(console_path):
        with open(console_path, "rb") as fh:
            console = fh.read()
    text = console.decode("utf-8", "replace")

    if "GATE-PANIC" in gdb_log:
        ended = "a panic ended it"
    elif "GATE-HEAP" in gdb_log:
        ended = "the heap: line ended it"
    else:
        ended = "the %d s bound ended it" % seconds
    print("           %d byte(s) of console, %d s, %s; this image %s `heap:` line in it"
          % (len(console), round(elapsed), ended,
             "carries a" if expect_heap else "carries no"), file=out)
    print("--- what the image printed (%s)" % console_path, file=out)
    for line in text.splitlines():
        print("    | %s" % line, file=out)
    if not text:
        print("    | (nothing; UART0's own lines are in %s)" % uart0, file=out)
    # The gate's markers, and - when there was a panic - gdb's backtrace under it, which is where
    # the panic's own redacted sentence stops being enough (FRAME above).
    frames = False
    for line in gdb_log.splitlines():
        if line.startswith("GATE-"):
            print("    gdb: %s" % line, file=out)
            frames = line.startswith("GATE-WHERE")
        elif frames and line.strip():
            print("    gdb: %s" % line.strip(), file=out)

    ok, lines = judge(text, gdb_log, version, expect_heap=expect_heap)
    print("--- the gate", file=out)
    for line in lines:
        print("    %s" % line, file=out)
    print("boot gate: %s" % ("PASSED: this image starts" if ok else
                             "REFUSED: this image is not fit to be released"), file=out)
    return ok, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description="Boot a release's factory image in QEMU before it "
                                             "is signed (task p11-04).")
    ap.add_argument("--factory", required=True, help="the release's -factory.bin")
    ap.add_argument("--elf", required=True,
                    help="the build's twinoak-walter.elf, for the symbols the gate breaks on")
    ap.add_argument("--version", required=True,
                    help="the version being released; the banner must name it")
    ap.add_argument("--work", default=None,
                    help="where the flash image, the transcript and the logs are left "
                         "(default: a temporary directory, kept)")
    ap.add_argument("--seconds", type=int, default=90,
                    help="the bound on the boot, in seconds (default 90)")
    ap.add_argument("--show-gdb", action="store_true", help="print the gdb script and the QEMU "
                                                            "command line as well")
    args = ap.parse_args(argv)
    work = args.work or tempfile.mkdtemp(prefix="twinoak-boot-gate-")
    try:
        ok, _lines = boot(args.factory, args.elf, args.version, work, args.seconds,
                          show_gdb=args.show_gdb)
    except Refused as refused:
        print("boot gate: REFUSED: %s" % refused, file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
