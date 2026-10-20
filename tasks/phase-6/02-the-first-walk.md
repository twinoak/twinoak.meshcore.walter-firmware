# 02 — The first walk

**Depends on:** 01.
**Spec:** `docs/bench.md` §1 *The Walter alone on USB*, §2 *The pins and the reset line* (its console lines), §4 *SWD*, *What to bring back*; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) §5 (*SWD*: 125 kHz to start, through 470 Ω and 2.2 nF), §10.2; the hardware repository's `hardware/pcb/BENCH-BREADBOARD.md` §4 (the filtered chain: τ ≈ 1 µs, "≤ ~300 kHz, SWDIO turnaround is the tight spot") and §6 caveat 6 (APPROTECT), `hardware/pcb/INTERCONNECT.md` (the D-sub pins); this repository's `components/core/swd/` (`wire`, `dp`, `mem_ap`), `components/core/nrf52/` (`identify`), `components/core/settings/keys.cpp` (`swd.khz`), `docs/console.md` (`rak swd …`, `pins`, `rak reset`), tasks/phase-1 reports 03–04 (*Not verified here*).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The first time this firmware runs on a Walter in the real unit, the first time it pulls a reset
line, the first time it speaks SWD through two filtered D-subs and a ribbon to a radio. Nothing in
this task writes to the radio: it finds out whether the chain works, how fast it works, and what the
radio says it is — the three facts every later step leans on — and writes them down.

## Deliverables

- **`bench walk 1`** — §1 by the tool: the banner after a reset of the Walter, `status`, `help`
  (every command `docs/console.md` lists is in it, and nothing it does not), `pins`, `reboots`,
  `get` (secrets masked), a flash of this commit's build through `bench flash` and the banner again
  with the new version (the settings unchanged: 01's check). Measure the time from the Walter's
  reset to its prompt, and from that reset to the radio answering again after the GPIO glitch
  (caveat 3) — the constant 01 left *to be measured*.
- **`bench walk 2`** — §2's console lines only: `pins` says the reset line is released and the kill
  line floating; `rak reset` once, then a second `rak reset` inside a second refused by the console
  (not by the tool), then one after a second accepted. Everything with a meter or a scope in §2 is
  printed as *needs a person*.
- **`bench walk 4`** — §4 on silicon, **read-only**: `rak swd connect` (DPIDR `0x2BA01477`), `rak
  swd id` (FICR `INFO.PART`, `INFO.VARIANT`, `DEVICEID`, APPROTECTSTATUS), `rak swd mem` of FICR and
  of the first words of flash (on a factory radio: RUI3's), each three times. A closed access port is
  a result, not a failure: the variant, *locked*, and the line `rak provision check` will say in task
  03.
- **The clock's ceiling through the chain** — `swd.khz` stepped 125, 167, 250, 500: the clocks the
  bit-bang can really make (its half-period is a whole number of microseconds, at least one —
  `core/swd/wire.h`; record the half-period in force at each step, as `rak swd connect` reports it,
  not the number typed); at each, 50 rounds of connect + `id` + a 256-word FICR read, counting every failure by kind (no ACK,
  WAIT exhausted, FAULT, parity, a value that differs from the 125 kHz reading). The ceiling is the
  highest clock with zero failures in three consecutive runs of 50, and the margin is the next step
  down; write both into *What to bring back*, with the table. Restore `swd.khz` to 125 at the end
  (`bench restore`) whatever happened — the default does not change in this task; whether it should is
  a follow-up for Thomas with the numbers.
- **What to bring back:** DPIDR; `INFO.VARIANT` (as four characters and what the table of phase 5
  says it is); APPROTECTSTATUS on arrival; the ceiling and the margin; the reset-to-prompt and
  glitch-to-radio times. And a `[Thomas]` line in `tasks/phase-6/FOLLOWUPS.md`: *write
  `INFO.VARIANT` and the SWD ceiling into the hardware repository's `PROJECT-KNOWLEDGE.md`* (a session
  never writes a sibling repository).
- **What the walk finds broken** is fixed in this task when it is in the code the walk exercised and
  the fix is small: reproduce it in a host test first, then fix, then walk again. Anything larger is a
  follow-up tagged for the task that owns the code, with the walk's result file named.
- `bench walk 1|2|4 --tick` run at the end; the result files under `out/bench/` named in the report.

## Acceptance criteria

- [ ] §1, §2 (console lines) and §4 walked by the tool on the rig, results under `out/bench/`, ticked or followed up.
- [ ] The SWD ceiling table is in *What to bring back* and the report, and `swd.khz` reads 125 afterwards.
- [ ] `INFO.VARIANT`, `DEVICEID` and APPROTECTSTATUS on arrival recorded in the report.
- [ ] Nothing was written to the radio (the tool's log of the session shows no ⚠ line).
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas)

§2's meter and scope lines; the scope on SWDIO at the ceiling (the turnaround edge), which says why
the ceiling is where it is.

## Out of scope

Anything that writes to the radio (03). The link (§3) — it needs a repeater image, which 03 puts there.
