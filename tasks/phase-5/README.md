# Phase 5 — the radio's base

Three tasks that give the Walter the one thing it was told it must never do by itself, and make
sure it still never does it by itself: give a factory radio its base. Every RAK4630 arrives with
RAK's RUI3, not the MBR, SoftDevice S140 6.1.1 and bootloader the repeater is linked against
(found on the first board, 20 October). Until now that was a debug probe on every unit. Since 20
October it is `spec/SYSTEM.md` **S-5a**: a person types `rak provision <device id> confirm`, and the
Walter erases the chip through its CTRL-AP, writes the base from its image store, sets the UICR
words, reads every word back and resets the radio once. Nothing else in this firmware can reach
that sequence — not the supervisor, not an update, not the agent — and the rails prove it, as they
proved until now that nothing could reach a chip erase at all.

**Prerequisites:** phase 3 merged; `spec/` copied from the hardware repository at a commit that has
S-5a and `FIRMWARE-MANIFEST.md` §4a (`python spec/tools/specsync.py verify spec` passes and
`spec/fixtures/manifest/base-rak4630.hex` exists). Phase 4 is not needed: nothing here talks to a
server. Run phase 5 after phase 4 has merged or before it starts — not while it runs, because the
two share `docs/` and `components/core/` and one driver works in one checkout at a time.

The source the sequence comes from, besides the documents each task cites: the hardware
repository's `software/firmware/walter-swd-bench/` — a bench tool that did this from a Walter on the
production chain before this firmware could (`src/nrf52.cpp` `provision()`, its variant table
`protectKind()`, and `host-test/sim_target.cpp`, a simulated nRF52840 with both kinds of
APPROTECT). Read it at a commit with `git -C ../twinoak.meshcore.standard-repeater-design-v2 show
<commit>:software/firmware/walter-swd-bench/<path>`; it is a second source, not code to copy — this
firmware's own SWD, nRF52 and store layers are the ones to build on, and Nordic's product
specification wins over both.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-base-in-the-store](01-the-base-in-the-store.md) | phase 3; the spec copy | `use: provision` read and checked (5p, §4a); the store keeps one base per slot; `walterctl push --as base` |
| 02 | [one-door](02-one-door.md) | 01 | the CTRL-AP's erase and soft reset and the UICR behind one door that only a typed grant opens; the simulated chip learns both kinds of APPROTECT; R3 rewritten so the door has exactly one caller |
| 03 | [rak-provision](03-rak-provision.md) | 02 | `rak provision check` and `rak provision <id>\|locked confirm`: S-5a's sequence, every refusal before the first write, every failure safe to run again; the docs and the bench step |

```powershell
.\tasks\run-all.ps1 -Phase 5
```

## Models

- **Fable** — 02, 03. The only code in this firmware that may erase a chip.
- **Opus** — 01.

## The exit criterion

In the sessions: against the simulated nRF52840, a factory radio of each kind — revision 2 open,
revision 2 locked, revision 3 open, revision 3 locked with a bootloader that keeps the port open and
one that does not — ends with the base in flash word for word, the UICR words the base names,
`UICR.APPROTECT` exactly as the variant table says, and one pin reset; an unknown variant on an open
radio is refused with nothing erased; a power cut at every step is followed by a run of the same
command that succeeds; and in every other test of the tree the simulated chip's tripwire is still
armed and never fires.

On the bench (phase 6, task 03): a factory RUI3 radio, through the D-subs, at 125 kHz.

## Not in this phase

`rak provision` as a MeshMaster job (S-5a, *later*). The first application after the base: that is
phase 2's `rak update --first-install`, unchanged. Anything on silicon (phase 6).

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
