# 02 — One door

**Depends on:** 01.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5 and **S-5a** (read both in full: S-5a is the one exception and its wording is the contract), §5 (*provisioning*), §10.2; Nordic's nRF52840 Product Specification v1.7 (or the newest), *CTRL-AP — Control access port* (RESET, ERASEALL, ERASEALLSTATUS, APPROTECTSTATUS, IDR), *APPROTECT*, *UICR* (NRFFW, APPROTECT, the registers' reset values), *NVMC* (CONFIG, READY, writing UICR "in the same way as flash"), *FICR — INFO.VARIANT*; Nordic's IN-141 (*nRF52840 revision 3: improved APPROTECT*; build code F, `HwDisabled` = `0x5A`, "unlocked until the next pin, power or brownout reset"); the hardware repository's `hardware/pcb/BENCH-BREADBOARD.md` §6 caveat 6 and `software/firmware/walter-swd-bench/` at its newest commit (`src/nrf52.cpp` `eraseAll()`, `protectKind()`; `host-test/sim_target.cpp` `eraseAll()`, `softReset()`, `pinReset()` — a second source); this repository's `components/core/include/core/nrf52/{nrf52,regs}.h`, `components/core/nrf52/nrf52.cpp` (`open`, `gate`, `readProtection`, `setConfig`), `host_test/sim/{nrf52_chip,swd_target}.{h,cpp}`, `host_test/core/nrf52/test_nrf52_forbidden.cpp`, `tools/check_rails.py` R3, `docs/ARCHITECTURE.md` §4–§5.
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

Until today this firmware could not name a register that erases a chip, and four separate checks
held that. S-5a makes one exception: a person's typed command may erase a factory radio and give it
its base. This task builds the **door** that exception goes through, and keeps the rest of the
house exactly as sealed as it was. The door can erase through the CTRL-AP, soft-reset through it,
read and write UICR words, and nothing else; it can only be opened with a grant that only the
console command of task 03 can make; and every test in the tree that is not about the door still
runs against a simulated chip whose tripwire fires the moment any of it is touched.

## Deliverables

- **The door** (`components/core/nrf52/provision_door.{h,cpp}`, no ESP-IDF include):
  - `ProvisionGrant` — a value with no public constructor, made by exactly one function,
    `grantFromTypedCommand(...)`, declared in a header that only `components/core/provision/` (task
    03) and `components/core/console/console.cpp` — the file where a parsed verb runs; the parser
    (`parser.cpp`) only recognises the words — may include. It carries what the person typed, in
    S-5a's grammar: the radio's `DEVICEID` (16 hex digits, case-insensitive, held as `%08X%08X` of
    `DEVICEID[1]` then `[0]`), `locked`, or `locked:<key16>`. It is consumed by the door when it
    opens: one grant, one opening.
  - `ProvisionDoor(Nrf52&, swd::Dp&, Clock&, ProvisionGrant&&)`. **The door checks the grant
    itself**, so that a later caller (S-5a's MeshMaster job) cannot skip what the console checks:
    opening reads APPROTECTSTATUS; an open port needs a `DEVICEID` grant equal to FICR's; a closed
    port needs a `locked` or `locked:<key16>` grant (which of the two is the sequence's to decide,
    from the link — task 03); any other pairing refuses before a register is written. And on it only:
    `approtectStatus()`; `eraseAll(timeout)` — write CTRL-AP `ERASEALL = 1`, poll `ERASEALLSTATUS`
    until 0 or the timeout (cite the product specification's erase time; the bench tool used 5 s);
    `softReset()` — CTRL-AP `RESET` 1, then 0, then the DP reconnected (line reset, `DPIDR`, power-up:
    say in a comment why a reconnect, and test both a chip that needs it and one that does not);
    `writeUicr(addr, const uint32_t* words, n)` — inside `0x10001000`–`0x10001307` only, never
    `UICR.APPROTECT` (that is `writeApprotect`'s); NVMC `CONFIG = Wen`, the words, `READY` after each,
    `CONFIG = Ren` whatever happened (the earlier failure wins, as `restoreConfig` does);
    `writeApprotect(kind)` — `0x0000005A` for the improved kind, nothing for the legacy kind (the
    variant table of task 03 passes the kind; the door never takes a value);
    `writeFlash(addr, words, n)` — the base's flash runs, **refusing any address inside the board's
    application region or its file system** (the board table's numbers, not a caller's);
    `blankCheck()` — the first flash word, `NRFFW[0]` and `UICR.APPROTECT` read back, for task 03's
    step 4. **The three writes refuse unless `eraseAll` succeeded in this same opening.**
    With `writeApprotect` and `blankCheck` on the door, `core/provision/` never names a UICR
    register.
    Nothing else: no NVMC `ERASEALL`, no `ERASEUICR`, no partial erase — the CTRL-AP's erase is the
    only erase S-5a needs, and it is the one that works on a locked part.
  - Every register access cites the document and section beside it.
- **The session, for a locked chip** (`nrf52`): `open()` today refuses before any memory access when
  APPROTECTSTATUS reads closed. Keep that for every caller; give the door a way to *talk to the
  CTRL-AP* of a closed chip (it is AP 1; nothing of AP 0 is touched) and, after its erase, to have
  `open()` and `identify()` run again. Say in ARCHITECTURE §4 exactly which calls a closed chip
  allows and through whom.
- **The simulated chip** (`host_test/sim/nrf52_chip`) learns what the bench tool's simulator knows,
  from the product specification first:
  - `INFO.VARIANT` as a parameter; **legacy** (build codes A–D) and **improved** (F) APPROTECT;
  - factory states: open with RUI3-like contents, locked by firmware (legacy: `UICR.APPROTECT` =
    `0xFFFFFF00`), locked by default (improved: `UICR.APPROTECT` erased);
  - CTRL-AP `ERASEALL` (flash, UICR and RAM to `0xFF`; `ERASEALLSTATUS` busy for a set number of
    reads); after it, improved: the port open until a pin/power/brownout reset; legacy: the erased
    UICR takes effect at the next reset (the conservative model — the bench tool's — so that the
    soft-reset path is exercised);
  - CTRL-AP `RESET` (a soft reset: legacy re-reads UICR; improved keeps the port as it is);
  - a pin reset and a power-on: legacy reads UICR; improved opens the port only if `UICR.APPROTECT`
    is `0x5A` **and** the flash holds firmware that writes `APPROTECT.DISABLE` (a parameter: *the
    bootloader opens the port* yes/no — the RAK's bootloader may or may not have been built with MDK
    8.40.2 or later);
  - UICR writable only through NVMC with `Wen`, each word at most twice between erases (as flash);
  - **resets counted by kind** — CTRL-AP soft reset, pin reset, power-on — so that task 03's test
    can hold S-5a's rule (no pin, power-on or brownout reset between the erase and the last UICR
    write; the one soft reset is allowed).
  - **The tripwire stays armed by default.** A test that is about the door arms the chip with
    `allowProvisioning()`; every other test, as today, fails the moment `ERASEALL`, `ERASEUICR`, a
    CTRL-AP erase or a UICR write is seen. The tripwire's four messages stay word for word (R3
    greps for them).
- **The rails** (`tools/check_rails.py` R3, and `test_nrf52_forbidden.cpp`, which reads the tree):
  - the forbidden register names may now appear in `regs.h` **and in `provision_door.cpp`**, and
    nowhere else under `components/` and `main/` — an exact list of two files in the rail, not a
    pattern;
  - **new, R3b — the door has one caller:** `provision_door.h` is included by `core/provision/`
    (task 03) and by nothing else, and the grant's header by `core/provision/` and
    `core/console/console.cpp` and nothing else; `supervisor/`, `update/`, `flasher/`, `cycle/`
    and any future `agent/` include neither (list them by name and fail if one does — a directory
    that does not exist yet is a pass, said in the output);
  - the grep must still match something (as today: a rail that has stopped matching cannot pass).
  - `docs/ARCHITECTURE.md`'s rail table and `README.md`'s rails paragraph say what changed and why,
    in a sentence each, citing S-5a.
- **Tests (host)**, failure cases first: `eraseAll` on each factory state, with the status read back;
  an erase that never finishes (the timeout); `softReset` on a chip that needs a reconnect and one
  that does not; each write without a successful `eraseAll` in the same opening (refused before any
  access); `writeUicr` at `UICR.APPROTECT` or outside UICR's registers (refused before any access);
  each grant against each port state (a `DEVICEID` grant on a closed port, `locked` on an open one,
  a `DEVICEID` that is not this chip's — all refused at opening); `writeFlash` into the application region and into the file
  system (refused before any access, nothing written: the chip's write count says so); a grant used
  twice (refused); the tripwire firing in a test that did not arm it (a test that expects the failure).

## Acceptance criteria

- [ ] The door's five operations behave on both kinds of chip as the product specification says, with every register cited (tests).
- [ ] R3 and R3b pass on the tree and fail on a planted include of the door in `supervisor/` and a planted `kEraseAll` in `update/` (show both in the report: plant, run, revert).
- [ ] Every existing test passes with the tripwire armed; only the door's tests arm it.
- [ ] ARCHITECTURE §4–§5, the rail table and README say what the door is and who may open it.
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas, with the rig)

Nothing on silicon in this task: the door is walked by task 03's command, on the bench in phase 6.

## Out of scope

The sequence and the command (03). Writing the base's flash *runs* in order and verifying them is
03's; this task only provides the guarded write.
