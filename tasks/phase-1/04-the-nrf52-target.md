# 04 — The nRF52 target

**Depends on:** 03.
**Spec:** [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §4 ("The debug port and the memory access port", "The nRF52", "The simulator"); Arm IHI 0031 (the MEM-AP chapter); Arm, *ARMv7-M Architecture Reference Manual* (the debug registers `DHCSR`, `DEMCR`, `AIRCR`); Nordic, *nRF52840 Product Specification* (`FICR`, `UICR`, `NVMC`, the debug and trace chapter with the control access port) and Nordic's note *Working with the nRF52 Series' improved APPROTECT*; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/BENCH-BREADBOARD.md` §6 caveats 5 and 6, `PROJECT-KNOWLEDGE.md` §6.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

Through the debug port of task 03, the Walter can read and write the nRF52840's
memory, say exactly which chip it is talking to, tell whether the chip will let
it in, stop and restart the core, and erase and write flash pages — and it is
**unable to erase the whole chip or its UICR by any path a program can take on
its own**. The simulated target grows into a simulated nRF52840 to match.

## Starting facts — verify each against the documents and cite it

**Memory access port** (AP 0, an AHB-AP): `CSW` at `0x00`, `TAR` at `0x04`,
`DRW` at `0x0C`, `IDR` at `0xFC`. `CSW` selects the access size and address
auto-increment; with auto-increment on, `TAR` advances after each `DRW` access
but **only within a 1 KB block** — at each boundary `TAR` must be written
again.

**The nRF52840**
- `FICR` at `0x10000000`: `CODEPAGESIZE` `+0x010`, `CODESIZE` `+0x014`,
  `DEVICEID` `+0x060`, `INFO.PART` `+0x100` (`0x52840`), `INFO.VARIANT`
  `+0x104` (four ASCII characters; the build code), `INFO.FLASH` `+0x110`.
- `UICR` at `0x10001000`: the bootloader's address at `+0x014`, `APPROTECT`
  `+0x208`, `REGOUT0` `+0x304`, the reset pin selection `+0x200` / `+0x204`.
  **Read-only to this firmware.**
- `NVMC` at `0x4001E000`: `READY` `+0x400`, `CONFIG` `+0x504` (0 read, 1 write
  enabled, 2 erase enabled), `ERASEPAGE` `+0x508`, `ERASEALL` `+0x50C`,
  `ERASEUICR` `+0x514`. Flash pages are 4096 bytes; a write can only clear
  bits; a page erase takes tens of milliseconds and a word write tens of
  microseconds — take the limits from the electrical specification.
- **The control access port** (AP 1): `RESET` `+0x000`, `ERASEALL` `+0x004`,
  `ERASEALLSTATUS` `+0x008`, `APPROTECTSTATUS` `+0x00C` (bit 0: 0 = access-port
  protection is **on**), `IDR` `+0x0FC`. With protection on, the debug port
  answers and AP 0 does not grant memory access. Newer revisions of the chip
  ship with protection on unless `UICR.APPROTECT` holds a magic value, and
  firmware must also open it at run time — read Nordic's note; the module
  batch decides which we get.
- **The core**: `DHCSR` `0xE000EDF0` (the debug key in the top half on every
  write; `C_DEBUGEN`, `C_HALT`; `S_HALT` to read), `DEMCR` `0xE000EDFC`
  (`VC_CORERESET` to halt at reset), `AIRCR` `0xE000ED0C` (`SYSRESETREQ` with
  its key).

## Deliverables

- **`components/core/swd/MemAp`**: `read32`, `write32`, and block read and
  write that use auto-increment and rewrite `TAR` at every 1 KB boundary;
  the access port's `IDR` checked once. Errors surface as task 03's results,
  with the address that failed.
- **`components/core/nrf52/`**:
  - **`identify()`** — part, variant (as text), flash size and page size from
    `FICR`, the device id. Anything but an nRF52840 with 4096-byte pages is
    reported and refused by everything below.
  - **`protection()`** — from the control access port: *open* or *protected*.
    On a protected chip, memory access is not attempted and every operation
    below returns "protected" — a distinct, reportable result.
  - **core control** — `halt()`, `resetAndHalt()`, `run()`, `systemReset()`.
  - **flash** — `erasePage(addr)` (page-aligned; waits for `READY` within a
    bound taken from the datasheet's maximum, with margin), `writeWords(addr,
    data)` (word-aligned; enables write, writes, waits, disables), `read()`.
    `CONFIG` is returned to read-only after every operation and on every error
    path.
  - **An address guard at the bottom**: `erasePage` and `writeWords` take a
    region object, not a bare address, and refuse anything outside it. There is
    no overload without one. (The flasher of task 05 owns *which* region; this
    is the backstop that makes "the wrong page" a type error or a refusal.)
  - **What does not exist**: no function that writes `NVMC.ERASEALL`,
    `NVMC.ERASEUICR`, the control port's `ERASEALL`, or any address in `UICR`.
    The register offsets are named constants in the header, for the simulator
    and for a person reading; a host test greps the component and fails if any
    non-test code references them for writing.
- **`host_test/sim/Nrf52Chip`** — behind task 03's simulated debug port: an
  AHB-AP with `CSW`/`TAR`/`DRW` semantics (auto-increment, the 1 KB wrap —
  the simulator wraps exactly as the hardware does, so forgetting to rewrite
  `TAR` writes to the wrong place and a test catches it); a control access
  port; and a memory map of `FICR`, `UICR`, 1 MB of flash that **only clears
  bits on write and needs `CONFIG` right**, `NVMC` with a `READY` that takes
  simulated time, and the three debug registers with a core that is running or
  halted. It records every flash erase and write. It can be told to: be
  protected; be a different part; lose power at the Nth flash operation
  (leaving a page half-written or half-erased, as flash does); report `READY`
  late or never; have `ERASEALL` written to it — **and it fails the test
  immediately if that ever happens**.
- **The console**: `rak id` (connect, identify, protection state, the core's
  state, release) and `rak mem <addr> [words]` (read-only).

## Tests (host)

- Identify the simulated chip; a different part refused; a protected chip
  reported and nothing below attempted.
- Block read and write across a 1 KB boundary and across several.
- Page erase and word write: the simulator's log shows `CONFIG` set and
  restored, `READY` awaited; a write to a page that was not erased leaves the
  AND of old and new and the verify catches it; `READY` late; `READY` never
  (bounded, an error, `CONFIG` restored).
- The guard: erase and write inside the region succeed; one page below, one
  above, straddling the end — refused before any `NVMC` access (the log is
  empty).
- Halt, reset-and-halt, run, system reset: the simulated core's state follows.
- The grep test for the forbidden registers, and the simulator's tripwire —
  each shown failing once on purpose.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] Every starting fact is cited in the code where it is used, or corrected in the report with the citation.
- [ ] The simulated chip's log proves `NVMC.CONFIG` is read-only after every operation, including each failure case.
- [ ] No code path, in any test, touches a flash page outside the region it was given.
- [ ] The report says what the datasheet gives for erase and write times and what bounds were chosen.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "4. SWD":

- `rak id`: part `52840`, the variant string — **write it down**; it decides
  whether production modules arrive protected — the flash size, and
  *open* or *protected*.
- If *protected*: stop. The procedure for opening a module is a person's, once
  per module, with a full reflash of bootloader and SoftDevice afterwards
  (hardware repository, `BENCH-BREADBOARD.md` caveat 6); it is not this
  firmware's to automate.
- `rak mem 0x10000100 2` prints the part and variant; `rak mem 0x00026000 4`
  prints the start of the application's vector table.

## Out of scope

Deciding what to flash and where (05). Opening a protected chip.
