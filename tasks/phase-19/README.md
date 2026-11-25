# Phase 19 — the first try of a flash

Written on 25 November.

**What happened.** Since Walter 0.1.11 the scanner flash of every NoiseScope session fails
at its first try in the scan, *swd wait-timeout, nothing written*: eight sessions of eight
on the unit at Bellevue. Phase 18's 05 tries once more and that has carried six of seven.
The place and the moment are not fixed: `0x036000` after 32 to 33 s four times, `0x034000`
after 30 s, `0x029000` after 9 s, `0x035000` after 31 s. The flash back over a running
scanner has never failed, and neither did the radio's update of 23 November, which began
minutes into its wake.

**What is known of the cause.** The debug port gives up on a WAIT after ten tries back to
back, a few milliseconds. The radio writes its own flash now and then (its clock's file at
the first clock set after a boot, and every session ends in a boot), and a chip that is
erasing a page does not answer for tens of milliseconds. That the scan meets such an erase
is my reading; nothing has shown it.

**What this pile builds.** 01: a WAIT is waited out for as long as the chip's own
specification says a flash operation may take, and the unit says how long it waited.

**Chosen by me, his to change before the run:**

- **Waited out, not avoided.** The scan stays where it is, before the halt, so that a flash
  that would change nothing stops nothing.
- **The bound is the Product Specification's**, quoted, with a margin; not my figure.
- **The second try stays** as the net for what this does not explain.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-wait-is-waited-out](01-a-wait-is-waited-out.md) | — | a WAIT retried for a time from the chip's specification, with the line idle between; the longest WAIT in the counters, the journal and the report; the second try as it was |

```powershell
.\tasks\run-all.ps1 -Phase '19'
```

## Models

- **Opus**: 01.

## Afterwards (Thomas)

- **Merge the notes pull request of v0.1.12, release** (it carries 18/06 too), import,
  *Offer and apply* on the unit.
- **The next session on the unit is the proof:** no failed first try, and a line that says
  how long the port waited. I read it.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
