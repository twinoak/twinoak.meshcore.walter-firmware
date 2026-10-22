# 03 — `rak provision`

**Depends on:** 02.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) **S-5a** (the sequence, step for step, and what must never lead there), S-5, S-7, §10.2; [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §4a; Nordic IN-141 and the nRF52840 Product Specification (*FICR INFO.VARIANT* and its enumerated values; *CTRL-AP*; *UICR APPROTECT*); the hardware repository's `software/firmware/walter-swd-bench/src/nrf52.cpp` `provision()` and `host-test/test_main.cpp` (the scenarios a second implementation already passes) and `README.md` (*What `provision yes` does*); `hardware/pcb/BENCH-BREADBOARD.md` §6 caveats 3–6; this repository's tasks 01–02 of this phase, `components/core/board/radio_pins` (reset open-drain, resets at least a second apart), `board/slot` (`SlotLock`), `supervisor/` (how it holds the radio), `update/radio_update` (`--first-install`), `console/commands.cpp` (the `confirm` convention of `rak flash-test confirm` and `erase confirm`), `docs/console.md`, `docs/provisioning.md`, `docs/field.md` (*What cannot be recovered remotely*), `docs/bench.md`, `README.md`, `CLAUDE.md`.
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

The command S-5a describes, on this firmware's layers: a person reads a factory radio's
`DEVICEID`, types it back, and the Walter gives that radio its base and leaves it waiting for its
first application. Every refusal comes before the first write; every failure after it leaves a
radio that the same command, typed again, finishes. Written so that a later MeshMaster job can call
the same sequence with a grant of its own — and so that today nothing but the console can.

## Deliverables

- **The sequence** (`components/core/provision/`, no ESP-IDF include, host-tested), S-5a step for
  step; each step a named state in the result so the console and the report can say where it ended:
  1. **Refuse before anything:** no base in the store for this slot (01), or the stored base fails
     5p again (re-check, the store is flash); the slot lock not free (the supervisor or an update
     holds the radio — take the slot lock for the whole command, and say in ARCHITECTURE how the
     supervisor learns that the radio is being provisioned and must not climb its ladder meanwhile;
     a supervisor that reads a provisioning radio as *silent* would power-cycle it in the middle);
     the battery **low or falling** by the radio update's rule (`update/battery`) — an *unknown*
     battery is accepted exactly as `--first-install` accepts it (`radio_update.cpp`, ~L462–478),
     because a factory RUI3 radio has no link and so no reading; the kill line asserted.
  2. **Attach:** `DPIDR`, CTRL-AP `IDR`, APPROTECTSTATUS. **Open:** identify (FICR `INFO.PART`,
     `INFO.VARIANT`, `DEVICEID`); refuse — nothing erased — if the part is not an nRF52840, if the
     variant is not in the table, or if the typed ID is not this radio's `DEVICEID` (compared without
     regard to case; or the person typed a `locked` form). **Closed:** ask the link for `HELLO`
     (S-5a's grammar): no answer → only `locked` is accepted; an answer (an application with an
     identity is running) → only `locked:<key16>` is accepted, `<key16>` equal to the first 16 hex
     digits of the public key `IDENTITY` returns — a radio with an identity is named before it loses
     it. Anything else is refused. The door (02) checks the port half of this again when it opens.
  3. **The variant table** (`provision/variant`): build code A–D → legacy, `UICR.APPROTECT` stays
     erased; F → improved, `UICR.APPROTECT = 0x0000005A`; anything else → refused. A lookup with
     its source cited (IN-141, the product specification's enumeration), never a guess from
     behaviour. On a locked radio it is applied after the erase, and a variant outside it then stops
     the command with the radio erased — say so in the console line and the docs (the radio has lost
     nothing it had not lost already; extend the table, run it again).
  4. **Erase:** the door's `eraseAll`; APPROTECTSTATUS read back; closed → one `softReset`, attach
     again; still closed → stop. Identify again (now readable); blank check (the first flash word,
     `NRFFW[0]`, `UICR.APPROTECT` read `0xFFFFFFFF`); halt the core.
  5. **Write:** the base's runs (01's iterator) through the door, flash first then UICR, then
     `writeApprotect` with the table's kind; **no pin, power-on or brownout reset between the erase
     and this last write** — step 4's one CTRL-AP soft reset is the only reset allowed there (a test
     holds it with the simulated chip's resets counted by kind).
  6. **Verify:** every word of the base read back and compared; `UICR.APPROTECT` compared with the
     table's value. A difference stops the command and names the first addresses (the radio is
     then safe to provision again).
  7. **Reset and look:** release the halt, one pin reset through `RadioPins` (its own rules: open
     drain, at least a second since the last), wait for the bootloader, attach again, read
     APPROTECTSTATUS and `NRFFW[0]`. Report the port **open** or **closed** after the reset (closed on
     a revision 3 part means the RAK's bootloader does not copy `UICR.APPROTECT` into
     `APPROTECT.DISABLE`: the radio works, the flasher cannot reach it until that is solved —
     `spec/SYSTEM.md` §10.2; the report says it, it is not a failure of the command).
  The result: the end state, the radio's `DEVICEID` and variant, the base's hash, the port after the
  reset, and how long each step took. It goes to the console and to the log ring (no secret is
  involved, and nothing the mesh said).
- **Awaiting its first install.** After a successful provision the radio has a bootloader and no
  application, and is silent on the link — exactly what the supervisor's ladder would answer with a
  reset, a power-cycle or a golden image. So the slot is marked *awaiting first install*, kept across
  a Walter restart where the supervisor's own state is kept; while it is set the supervisor leaves the
  radio alone and says why (`rak watch`, `status`), and the wake cycle does not count the radio as
  silent. A committed `rak update --first-install` clears it; nothing else does, except a new
  `rak provision`. Tests: a provisioned radio left alone across a simulated week of wakes; the first
  install clearing the mark; a Walter restart keeping it.
- **The console** (`console/commands.cpp`, thin), under the `@slot` prefix like every `rak`
  command:
  - `rak provision check` — no write, ever: attach, identify (or *locked*, and whether an
    application answers on the link), the variant and what the table would do, the base in the store
    (hash, bootloader address, SoftDevice), and **the exact line to type next**, with the `DEVICEID`
    (as `rak swd id` prints it) or the `locked` form filled in.
  - `rak provision <DEVICEID>|locked|locked:<key16> confirm` — the grant from what was typed, then the
    sequence. Without `confirm`, or with a malformed ID, it prints what it would do and the line to
    type, as `rak flash-test` does.
  - After a success it prints the next two lines: `walterctl.py push --as candidate <release-dir>`
    and `rak update --first-install` — **a first install flashes the candidate** (`radio_update.cpp`
    `sourceOf`), and the image that passes its health check becomes golden; check against the
    simulator that it accepts a radio whose bootloader settings page is erased — it is, after an
    `ERASEALL` — and if it does not, that is this task's to fix inside the flasher's own rules, said
    in the report.
- **Nothing else reaches it.** No setting enables it, no wake runs it, no job type names it; the
  supervisor's ladder still ends at *stop* for a radio it cannot bring back. A test drives the
  supervisor, a radio update and a whole simulated wake cycle against a factory RUI3 radio and shows
  the door never opened (the armed tripwire of 02 is the proof).
- **Docs:** `docs/provisioning.md` gains *A factory radio* (the session: `rak swd id`, `rak provision
  check`, the typed line, the output, `walterctl.py push --as candidate`, `rak update
  --first-install`); `docs/field.md` *The radio's first golden image* is corrected — it says
  `push --as golden` before `--first-install`, and a first install takes the candidate;
  `docs/field.md` *What cannot be recovered remotely* (a radio whose bootloader is gone is now
  recoverable with a cable and this command, still not from a distance — that is the MeshMaster job
  of S-5a's *later*); `docs/console.md`; `docs/ARCHITECTURE.md` (the sequence, the door, who holds the
  slot); `README.md` and `CLAUDE.md`, whose *never* lines now read: no **automatic** path to a chip
  erase or a UICR write — S-5a's typed command is the one path, and R3/R3b hold it.
- **Tests (host)**, failure cases first, against 02's simulated chip:
  - each factory state — legacy open, legacy locked, improved open, improved locked with a
    bootloader that opens the port and one that does not — ends provisioned, with the base word for
    word, `UICR.APPROTECT` as the table says, one pin reset, and the reported port state;
  - an unknown variant: open → refused with zero writes; locked → erased, then refused, then the
    table extended in the test and the same command succeeds;
  - the wrong ID typed (and the right one in lower case, accepted); `locked` typed for an open radio;
    an ID typed for a locked one; `locked` for a locked radio whose application answers `HELLO`, and
    `locked:<key16>` with the wrong key; no base;
    a base that no longer passes 5p (a flipped byte in the simulated store); the slot lock held;
    the battery low: each refused before the first write (the chip's write and erase counts are 0);
    an unknown battery (a factory radio, no link): accepted;
  - **a power cut at every step** (the simulated chip and the Walter both restart): then the same
    command, typed again, succeeds — and `rak provision check` in between says exactly what it sees;
  - a verify difference (the simulator corrupts one word as it is written): stopped, addresses named;
  - the reset spacing: a `rak reset` 300 ms before the command's own reset delays it, never skips it;
  - the timing at 125 kHz, from the simulator's clock count: state the expected minutes for a real
    ~180 KB base in the report.

## Acceptance criteria

- [ ] Every scenario above passes; the chip's counters show zero writes for every refusal (tests).
- [ ] The supervisor, an update and a simulated wake cycle against a factory radio never open the door (test, tripwire armed).
- [ ] `rak provision check` and `rak provision … confirm` behave as documented in `docs/console.md`, replayed by the console session test the way `docs/provisioning.md`'s session already is.
- [ ] The docs and the two *never* lines say what S-5a says, no more and no less.
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md` a new step between §4 *SWD* and §5 *Flashing* — **§4a The radio's base ⚠** —
in the bench's four-part form: a factory RAK4630 with RUI3 on the production chain, `rak provision
check` (DPIDR, variant, *locked* or the ID), the typed command, the six steps with their times, the
port after the reset, then the RAK's USB-C on a laptop: the UF2 drive appears. Write the measured
times and the variant into *What to bring back*. Phase 6, task 03 walks it.

## Out of scope

The base's arrival in a release (repeater firmware pile 3.4) and over LTE (a `twinoak.fw.offer` for a
base is not proposed: a base is put in the store over the cable, at the bench, with the radio). The
command as a MeshMaster job (S-5a, *later*).
