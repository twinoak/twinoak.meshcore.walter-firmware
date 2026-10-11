# How this firmware is put together

The design the task piles build, written before a line of this repository compiled. Numbers that
come from a datasheet or a specification are marked *(verify)*: the task that first uses one reads
the document, cites it in the code, and corrects this file if it was wrong.

## 1. Layout

```
CMakeLists.txt, sdkconfig.defaults, partitions.csv, idf_component.yml + lock
main/                   app_main: construct the real things, hand them to the core, run
components/
  core/                 everything that is logic. No ESP-IDF include, no FreeRTOS, no heap after start-up.
    board/              the pin map and RadioPins: the seven pins and their rules (§2) - task p0-03
    health/             why the last boot happened: the reset reason, a boot counter, the last five - task p0-03
    link/               the box link: codec, client (the UART, retries, events, the session,
                        telling a repeater from a scanner), the protocol's numbers in words
    swd/                the debug port: wire protocol and DP (task p1-03), MEM-AP (task p1-04)
    nrf52/              identification, access-port protection, the flash controller, core control
    flasher/            an image and a region in; erases, writes, verify out
    image/              manifest, signature, descriptor, the image store's records
    update/             the radio's update and its journal; the Walter's own update policy
    supervisor/         the radio's heartbeat and the ladder of remedies
    power/              what the battery allows; the check-in interval
    console/            the line parser and the commands' text (task p0-03: help, status, reboots,
                        pins, rak reset, rak kill, reboot; task p1-02: rak hello, ping, status, id,
                        time, power, env, cli, events, text; task p1-03: rak swd connect)
    cycle/              one wake, start to finish, against interfaces
  interfaces/           the interfaces core/ needs: pins, a clock, retained storage, text out, a restart
                        (task p0-03); a byte stream and the time of day (task p1-02); flash regions,
                        a key-value store, an uplink, a sleeper (later). ("hal/" when this file was
                        written; ESP-IDF owns a component of that name - task p0-01)
  platform/             ESP-IDF implementations of interfaces/ - thin: the GPIO driver, esp_timer,
                        RTC memory, the USB console, the reset reason (task p0-03), the radio's
                        UART (task p1-02)
  modem/                the Walter's LTE modem behind interfaces/' uplink
host_test/              a CMake project of its own: core/ + fakes + simulators + tests, built with the host compiler
  fakes/                fake pins that check the pin rules, a clock whose delays take no time, retained storage as a map
  sim/                  the scripted RAK on a fake UART (task p1-02); the simulated SWD target at
                        the wire level (task p1-03), the nRF52 behind it and the simulated flash
                        (later)
  fixtures/             the spec's fixtures turned into tables the tests walk (the descriptor's, task p0-02)
tools/                  verify.ps1, release/ (the release tooling), bench tools
keys/                   the public halves of the keys a release may be signed with - empty today (task p0-02)
spec/                   a copy of the system specification and its fixtures - never edited here
docs/                   this file, bench.md, release.md, console.md, field.md
tasks/                  the piles
```

The rule the layout enforces: **if it can go wrong in the field, it is in `core/` and has a host
test that makes it go wrong.** `platform/` is the part no session can test; it is kept so thin that
reading it is the test.

## 2. The board: seven pins and their rules

From the hardware repository's `INTERCONNECT.md` (frozen) and `BENCH-BREADBOARD.md`:

| Net | D-sub | Walter GPIO | Direction | Rule |
|---|---:|---:|---|---|
| `LORA_nRESET` | 3 | 1 | open-drain | **output-low to assert, input to release. Never output-high.** The nRF52 has its own pull-up |
| `LORA_SWDCLK` | 4 | 2 | out | idle low; must not toggle while nRESET is held low (reserved for the dual adapter's selector) |
| `LORA_SWDIO` | 5 | 4 | bidirectional | input unless a transfer is driving it |
| `LORA_PWR_KILL` | 6 | 5 | out | **high = the LoRa 3.3 V rail is off.** Floating or low = on. Default at every boot and in sleep: not driving |
| `spare` | 7 | 6 | — | input, unused |
| `WALTER_TX` → radio RX | 8 | 7 | out | UART idle high while the radio is powered (pulled up, not driven, when the UART is not in use); input (not driving, not pulling) when it is not |
| `WALTER_RX` ← radio TX | 9 | 8 | in | an RTC-capable pin: a start bit can wake the Walter from deep sleep |

The D-sub column is the LTE box's connector (`INTERCONNECT.md`, "D-sub pinouts"); pins 1 and 2 are
ground and the raw battery. `core/board/pin_map.h` carries the same three columns and a host test
reads this table and fails if they disagree (task p0-03).

Each line has 470 Ω in series and about 2.2 nF to ground (two bulkhead filters): a time constant
near 1 µs.

**Rules, each enforced in one place (`core/board/radio_pins`, the only object that touches these
pins) and tested against fake pins that check them on every transition:**

1. **Safe state.** At boot, before anything else, and whenever this firmware has nothing to say to
   the radio: nRESET input, SWDCLK and SWDIO inputs, KILL not driving, TX idle high if the radio
   is believed powered — through the chip's weak pull-up, not driven, so that a radio that is
   unexpectedly dead sees microamps, not milliamps — and a plain input if not. TX is driven only
   between `uartEnable()` and `uartRelease()`.
2. **Reset is a pulse of output-low** (open-drain) of 100 ms, then input. **Two resets are at
   least one second apart**, measured from the end of one pulse to the start of the next — two
   inside half a second put the Adafruit bootloader into UF2 mode, where it stays. A reset that
   comes too soon *waits*; it is never refused and never shortened. The spacing is enforced across
   deep sleep too: the time of the last reset is kept in retained storage, and when the clock
   cannot be trusted against it (no record, a power cut, a clock that restarted) the worst is
   assumed — a reset ended just now — which also covers the one the power-up glitch (rule 5)
   gave the radio. There is no operation that drives nRESET high: the only handle on the pin can
   assert low and release.
3. **Before the kill line goes high, every other radio-facing pin is a plain input**, TX included
   and its pull-up off. The series resistors limit back-feed into a dead radio to a few milliamps;
   the firmware's job is to make it zero. Whoever held the UART or the SWD lines loses them (their
   operations return false from then on). After the rail is back, wait for it to settle before
   driving anything: the rail is *off* while the kill line is driven, *unknown* for the settle
   time after it is released — and for the settle time after boot, because a Walter that reset in
   the middle of a kill left it to float back on just before — and *on* after that.
4. **A kill is bounded**: at least long enough for the rail to collapse, at most a configured
   maximum (`kKillTiming`: 1 s, 30 s, 200 ms to settle — placeholders until the bench measures the
   rail, `docs/bench.md` §2); the kill line is driven high or not at all, never low; the firmware
   cannot leave the radio off by crashing — the line floats to *on* when the Walter resets, by
   hardware. Nothing acquires SWD while the rail is believed off, and the SWD clock cannot toggle
   while nRESET is asserted (a reset is refused while SWD is held; the holder releases first).
5. **Across deep sleep the pads are held** in the safe state, so that waking does not glitch the
   reset line: `safe()`, then the holds on; at wake the same state configured again, then the
   holds off. (A *power-up* of the ESP32-S3 does glitch GPIO1–14 low for about 60 µs, and that
   reaches nRESET: a Walter power cycle reboots the radio. That is expected, documented in the
   hardware repository, and the supervisor does not mistake it for a fault.)

The fake pins (`host_test/fakes/fake_pins`) know these rules by name — `reset-driven-high`,
`reset-spacing`, `reset-pulse-length`, `swdclk-while-reset`, `driven-while-killed`,
`kill-driven-low`, `kill-bounds` — and fail the running test at the transition that breaks one.
`tools/verify.sh` greps that no GPIO call exists outside `components/platform/` and that the
seven pins are named nowhere but `core/board/`.

## 3. The link, from this end

`spec/LINK-PROTOCOL.md`. Two layers: the codec, which is the protocol as bytes, and the client,
which is the protocol as calls.

**The codec** (`core/link/codec/`, task p1-01) touches no UART and allocates nothing:
CRC-16/CCITT-FALSE and COBS, the packet, a `FrameReceiver` that turns bytes in any chunking into
packets and counts what it drops, a `FrameWriter` that puts §2's one `0x00` in front of a frame
after 100 ms of idle, a `Reader` and a `Writer` for the little-endian scalars, one struct per
payload with an encoder for every request this end sends and a decoder for every response and
event it receives, and one header (`codec/wire.h`) that is the specification's own tables of
numbers. A decoded message's variable-length fields are *views* into the receiver's buffer, valid
until the next frame is read, so keeping one means copying it. A decoder refuses a value that
selects the layout of what follows it (`path_mode`, a reply record's `state`) and passes on one
that is only data (a status, a role, a capability bit), because §5.1 says a minor version adds
those and that unknown ones are ignored. It is tested against `spec/fixtures/link/link-v1.json`,
which the radio firmware's own codec is tested against too; neither sees the other's source, and
if both pass every case they agree on the wire.

**The client** (`core/link/client/`, task p1-02) owns the UART (115200 8N1 on GPIO7/8) and gives
the rest of the firmware typed calls: *hello*, *ping*, *status*, *set time*, *run this console
line*, *read power*, and the mesh client's calls — one per request type of §4, each answering with
the decoded response or the reason there is none (link down, refused with which status, malformed,
a capability the radio does not have, the UART not ours). Underneath: one request at a time, a
`seq` different from the last, a response awaited for one second in slices of a tick, the same
frame sent again up to twice more, then "the link is down" — reported, not acted on; acting is the
supervisor's. A response of another type or `seq`, and one that arrives with nothing in flight, are
counted and ignored. A decoded answer's variable-length fields are views into the client's own
response buffer, valid until the next call.

**Its two pins are taken and given back as one thing with the UART.** `core/link/client/port.h` is
the only caller of `RadioPins::uartEnable()` and `uartRelease()` and the only caller of the byte
stream: the peripheral is routed to the pads after the board logic has enabled them and away before
it releases them, and every read and write asks the board whether the UART is still enabled — a
`rak kill` takes the lines without telling anyone (§2 rule 3), and the port then closes the stream
itself and counts the loss. So "TX is driven only between `uartEnable()` and `uartRelease()`" is a
property of the code's shape and not of everybody's care.

It keeps the radio's `boot_id` and treats a change as "everything I knew about that radio's state
is gone"; a changed `boot_id` or an `EV_BOOT` is raised to whoever subscribed. It sets the radio's
clock after every restart it sees, before anything else — from `interfaces::WallClock`, which
answers "I have no time to give" until phase 3's modem fills it. `EV_BOOT`, `EV_MESH_REPLY` and
`EV_ATTENTION` are decoded and queued as they arrive — a small bounded queue, its overflow counted,
their sequence numbers watched for gaps — whenever the client reads the port, which the main task
does every time round.

If `HELLO` gets no answer three times and lines beginning with `{` arrive — or one answers an
`info\n` probe — the radio is running NoiseScope: the client says *scanner (text)*, sends no more
frames, and offers the lines to whoever subscribed until the radio is reset. It never mixes the two.

## 4. SWD

Three layers, each tested against the layer below it in simulation:

**The wire** (task p1-03, `core/swd/wire`; Arm's ADIv5.0 to ADIv5.2, IHI 0031G, chapters B4
and B5, cited line by line). Bit-banged, clocked entirely by us, so only the minimum half-period
matters, not jitter. **125 kHz to start** — a half-period of 4 µs, the slowest clock the nRF52840
Product Specification allows (*Debug and trace > Electrical specification*, f_SWDCLK 0.125 to
8 MHz) and four time constants of the filtered lines, which are **predicted to carry about
300 kHz and no more**; the half-period is a parameter, bounded below at 1 µs. Line reset (at least
50 clocks with SWDIO high, then at least two idle cycles low, B4.3.3), the JTAG-to-SWD sequence
`0xE79E` LSB first between two of them (B5.2.2), then a read of `DPIDR`, the only transaction the
reset state accepts. A transfer: eight request bits (start, APnDP, RnW, two address bits, even
parity, stop, park), a turnaround, three ACK bits (OK `001`, WAIT `010`, FAULT `100`, LSB first),
then 32 data bits and a parity bit in the direction of the transfer, with turnarounds where the
bus changes hands; eight idle cycles after every transfer so that the clock may stop (B4.1.1).
The target samples and changes SWDIO on SWCLK's rising edge (B4.3.1); we change it right after the
falling edge and **read the target's bit at the end of the low half, just before the next rising
edge** — SWDIO's turnaround is the tight spot through the filters, and that point gives the
target's driver a whole period less the clock's own delay (about 7 µs at 125 kHz, 2.6 time
constants at the predicted ceiling) instead of half of it. An acknowledge of all ones is *no
target*; any other value that is none of the three is a protocol error, after which the host backs
off for the length of a data phase before anything else (B4.2.5).

**The debug port and the memory access port.** `DPIDR` (the nRF52840's reads `0x2BA01477`: Arm's
DPv1 SW-DP, part `0xBA`, revision 2 — the Product Specification only says "a standard Arm
CoreSight SW-DP", so the bench confirms the number), power-up through `CTRL/STAT` with a bounded
wait for both acknowledges and a `powerDown()` that withdraws the request (on the nRF52840 that is
what ends debug interface mode), `SELECT` rewritten only when the port or the bank changes, an AP
read's posted value collected through `RDBUFF` so a caller sees the register it asked for; WAIT
retried ten times with idle cycles between; a FAULT reported with `CTRL/STAT`'s sticky flags and
cleared through `ABORT`; a parity error a failed transfer, never a value (task p1-03,
`core/swd/dp`). Memory access
through AP 0: `CSW` for 32-bit accesses with auto-increment, `TAR`, `DRW`; reads are posted, so the
value comes one transfer later; the address auto-increment wraps inside a 1 KB block, so `TAR` is
rewritten at every boundary.

**The nRF52** *(verify against Nordic's product specification)*:

- identification from `FICR` (`0x10000000`): part, variant, flash size, page size, device id;
- **access-port protection** read from the control access port (AP 1, `APPROTECTSTATUS`). A
  protected chip answers on the debug port and refuses memory access. **The firmware detects it,
  says so, and stops.** Opening it needs `ERASEALL`, which destroys the bootloader, the SoftDevice,
  the node's identity and UICR; that is a bench procedure for a person with a debug probe (hardware
  repository, `BENCH-BREADBOARD.md` caveat 6), once per module. This firmware has no code for it;
- the core: halt, reset-and-halt, run, system reset — through `DHCSR`, `DEMCR`, `AIRCR`;
- the flash controller (`NVMC`, `0x4001E000`): `CONFIG` to enable erase or write, `ERASEPAGE`,
  `READY`; 4096-byte pages on the nRF52840. `ERASEALL` and `ERASEUICR` exist in the header as
  named constants that nothing calls.

**The simulator** (`host_test/sim/`) is the other half of this section: a wire-level SWD target
(`swd_target`, task p1-03) that stands between `RadioPins` and the fake pins as a `Pins` of its
own, sees every rising edge of SWCLK, samples and drives SWDIO on it as a target does, and decodes
our bit stream — so a wrong parity bit, a stop or park bit wrong, a line reset one clock short, a
missing turnaround, SWDIO driven while the target drives it, a request before any line reset or
one other than `DPIDR` in the reset state, or the clock stopped under eight idle cycles after a
transfer, fails the test with a sentence naming the section. Its debug port has `DPIDR`, `ABORT`,
`CTRL/STAT` with the power-up handshake and the sticky flags, `SELECT`, `RESEND`, `RDBUFF` and the
posted AP read; access ports plug in behind a register-level interface (one stub that stores what
is written, until task 04 brings the two real ones and behind them a memory map: flash that needs
an erase before a write and can only clear bits, an `NVMC` with its ready flag and enable states,
`FICR`, `UICR`, the debug registers). It can be told to answer WAIT N times, to carry a sticky
error, to return a bad parity once, to go silent, to not be there (no pull-up: the line keeps what
was last driven), to acknowledge a power-up late or never, to start in JTAG so that only the
switch sequence reaches it, and (later) to lose power at the Nth write and to be protected.
Everything in this section and the next is developed against it.

## 5. The flasher

Input: an image (a raw binary and its load address) and the region it may occupy. The region is
**this firmware's own table, per board** — for `rak4630-single`, from the end of the SoftDevice to
the start of the file system — and the manifest's `app_region` must agree with it, not replace it.

1. Identify the chip; refuse anything but the expected part. Refuse a protected chip.
2. Check the image lies inside the region. Refuse otherwise — before anything is erased.
3. Halt the core.
4. Erase the image's first page — the vector table — first, and write it last: the bootloader will
   not start an application whose first word is blank, so an interrupted flash leaves the radio in
   its bootloader, never in half an application.
5. For each other page the image covers: skip it if it already holds the image's bytes (a resumed
   or repeated flash converges quickly); otherwise erase it and write it. Read everything back and
   compare.
6. **The bootloader's settings.** The Adafruit bootloader records the size and CRC of the
   application it last installed and refuses to start one that does not match — but treats an
   erased settings page as "installed by a debugger" and checks the vector table instead. The
   flasher leaves that page in a state the bootloader accepts. Which state, and the page's address,
   are read from the bootloader's source at the version the modules ship with, by the task that
   writes this step.
7. Reset and release the pins to the safe state.

It **never** erases a page the image does not cover, never touches the file system (the node's
identity and settings), the SoftDevice, the bootloader's code or UICR. A power loss at any point
leaves a radio with a broken application and an intact bootloader and identity; running the same
flash again finishes it.

## 6. Images

**The store**: a data partition holding, per radio slot, a **golden** image (the last one that
passed its health check), a **candidate** (verified, not yet proven) and one **scanner** image.
Each is a header — what it is, its size and SHA-256, a copy of its descriptor, its state — and the
bytes. A record becomes valid by a final small write after everything else is in place, so a power
loss leaves the previous valid record or none, never half of one.

**What may enter the store**: only what passed all six checks of `spec/FIRMWARE-MANIFEST.md` §5 —
the signature by a key compiled into this build, the manifest, the right image for this radio, the
file's hash, the one descriptor inside it, and the region. The trusted keys are a small table
(room for three) generated from `keys/release-keys.txt`; a build with none accepts nothing, which
is the state today because the release key does not exist yet (`keys/README.md`).

**The descriptor** (§4) exists from task p0-02: `components/core/image/descriptor.h` builds,
parses and finds those 96 bytes, with no ESP-IDF include and no heap, tested against the spec's
fixtures; `core/image/this_image.h` is this firmware's own, built at compile time and referenced
by `main/` so the linker keeps it. The release tooling finds exactly one of it in each published
file and holds it against the manifest entry (`docs/release.md`). Phase 2 reads a radio's images
with the same parser and finder.

## 7. Updates

**The radio's** (`spec/SYSTEM.md` §6): preconditions (battery, link, clock, nothing else in
progress) → flash the candidate → reset → health check (`HELLO` within a window; version, role and
board as the descriptor said; the link steady for a while; the clock set; the radio initialised)
→ the candidate becomes golden — or the golden image is flashed back and the candidate's hash is
remembered as bad and never tried again. **The journal is written before each step**, so a Walter
that resets mid-update knows where it was: it resumes a flash, repeats a health check, or rolls
back. It never assumes.

**Its own**: ESP-IDF's two application slots with rollback. A new image is marked valid only after
it has booted, exchanged `HELLO` with the radio, and reached the server within a window; otherwise
the bootloader returns to the previous one at the next reset — which this firmware asks for.

**Never both in one wake** (S-7). When the link's major version changes, the Walter goes first and
speaks both majors until the radio has followed; the order is enforced here, not left to whoever
starts the update.

## 8. The supervisor

The radio is healthy when it answers `PING`. When it does not, a ladder, each rung tried only
after the one before failed, each with a wait and a cap:

1. ask again (the link's own retries, then again after a pause);
2. a reset pulse;
3. a power cycle through the kill line;
4. flash the golden image, then 2;
5. stop, record, report at the next check-in. A radio that does not answer after the golden image
   is back is a hardware fault and is left alone.

Caps are per day and survive a restart of the Walter. A radio that restarts by itself over and
over (many `EV_BOOT`s) is a boot loop and takes the same ladder from rung 3. A radio whose restart
followed the Walter's own power-up is not a fault. Before any rung that cuts power or flashes: the
battery must allow it.

## 9. The uplink

The modem (`walter-modem`) behind an interface: power up, check the SIM, attach on LTE-M, read the
network's time, signal quality; power-saving modes; a taxonomy of failures (no SIM, not
registered, no data, no route) with a backoff for each. Every modem call has a timeout.

HTTPS through the modem's own HTTP and TLS, with the server's chain **verified** against a root
loaded into the modem — never "validate nothing". Requests are small; a download is in ranges,
resumable, hashed as it arrives, written straight to the image store.

## 10. One wake

The Walter sleeps deeply and wakes for one of three reasons: its timer, a start bit from the radio
(attention), or USB being plugged in (a person; stay awake). One wake is one pass of a fixed
sequence, each step bounded in time and energy:

```
why did I wake → is the radio alive (supervisor) → set its clock → read power
     → may I use the modem (battery) → attach → talk to the backend → act on what it said
     → at most one update step → tell the radio I am going → sleep until the next time
```

How long until the next time comes from the battery: a table from state of charge and its trend to
an interval, with hysteresis. Readings taken while there was no uplink are kept in a bounded ring
and sent later.

**The backend is an interface.** In these piles its implementation does nothing. The MeshMaster
agent goes behind it when the server side is decided (`spec/proposals/01` and `03`).

## 11. Flash layout

16 MB. The constraints the table was written to, and (task p0-01) the table itself, held to them by
`tools/check_partitions.py` on every build:

- two application slots with room to grow (3 MB each);
- **settings (NVS) and the image store above the application slots**, so that a factory image,
  which ends where the application ends, cannot reach them — the agent firmware learned this the
  expensive way;
- the image store sized for two radios (golden + candidate + scanner each, a radio image being at
  most the size of its application region — bounded here by the nRF52840's whole flash, 1 MB);
- a journal that is not the settings store (its own wear, its own failure modes);
- a core-dump partition;
- ~~whatever the modem library needs for a modem-firmware update~~ — read in task p0-01: the
  library's own update path (MOTA, driven by BlueCherry) wants a FAT partition named `ffat` large
  enough for the modem image, 9 MB for `LR8.2.1.0`, which does not fit beside the rest in 16 MB.
  MOTA is off and nothing is allocated for it; a modem firmware update is a bench procedure
  (QuickSpot's `modem_firmware_flash` example flashed for the occasion, or the module's `UART1`
  per the datasheet). The 3.75 MB above `rakfw` is left unallocated.

`partitions.csv`:

| Name | Type | Offset | Size | |
|---|---|---:|---:|---|
| (bootloader) | | `0x000000` | | |
| (table) | | `0x008000` | 4 KB | `0x9000`–`0xDFFF` empty on purpose: a factory image fills it with `0xFF` |
| `otadata` | data/ota | `0x00E000` | 8 KB | |
| `ota_0` | app/ota_0 | `0x010000` | 3 MB | |
| `ota_1` | app/ota_1 | `0x310000` | 3 MB | |
| `nvs` | data/nvs | `0x610000` | 64 KB | above the slots |
| `journal` | data/`0x41` | `0x620000` | 64 KB | ours |
| `rakfw` | data/`0x40` | `0x630000` | 6 MB | ours: 2 radios × 3 images × 1 MB |
| — | | `0xC30000` | 3.75 MB | unallocated |
| `coredump` | data/coredump | `0xFF0000` | 64 KB | |

## 12. What is tested where

| | Host tests | Build only | Bench |
|---|---|---|---|
| pin rules | ✔ fake pins | GPIO calls, sleep holds | a scope on nRESET and KILL |
| link | ✔ fixtures, scripted radio | UART driver | through the bulkheads |
| SWD wire, DP, AP | ✔ wire-level simulator | bit-bang timing | IDCODE, the speed ceiling |
| nRF52, flasher | ✔ simulated chip, power loss | — | a real flash, the bootloader starts it |
| manifest, signature, descriptor | ✔ spec fixtures, RFC vectors | — | — |
| image store, journal | ✔ simulated flash, power loss | partition access | — |
| updates, supervisor | ✔ scripted radio and flash | — | a deliberate bad image rolls back |
| modem, HTTPS | policy against a fake | every modem call | a SIM, a tower, a server |
| duty cycle | ✔ the policy | sleep entry and wake | current asleep; wake by UART |
