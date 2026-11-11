# Phase 11 — room to start (v0.1.4)

Written on 9 November, after the first update of the Walter's own image over LTE.

**What happened.** meshmaster.dk served its first manifest (its PRs #190 and #192), and LTE Exit
Node 1 fetched 0.1.3 in 12 ranges and stored it. The apply switched to it, and the bootloader came
back to 0.1.2 by itself: *the image never reached its main loop*. The rollback did what phase 2 built
it to do, on a unit nobody was at.

**Why.** The core dump the trial left behind
([evidence/](evidence/README.md)) says
`assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)`. ESP-IDF could not create
the **main task**: its 16 KB stack did not fit in what the heap had left after the static
initialisers. This board has no PSRAM, and `main/main.cpp` takes the agent's runner (~66 KB) and the
kept-image scratch (20 KB) from the heap in the initialisers, on purpose (23 October). 0.1.3 added
static storage: the modem library's `_logEscapeBuffer` (3 080 bytes, brought in by
`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`, task p10-06), the redactor's 322 bytes and whatever agent-core
brought. Report p10-06 counted *37 037 bytes of DIRAM free*. That is the linker's figure, and it
does not see the 86 KB taken at start or the 16 KB the main task needs after them.

**It is not the update.** The heap at start is the same at every boot, so 0.1.3 by cable panics at
every boot too, and an image flashed by cable has no rollback. **0.1.3 must not go on a board.**
No session of phase 10 opened a port, so 0.1.3 had never started on one.

So this pile has four parts:

- put the room back (01);
- make the build refuse an image that would not start (02);
- let a rollback say what killed its trial, so the next one is read from meshmaster.dk and not with
  esptool at the bench (03);
- make a release start before it is published, in CI (04). v0.1.4 itself was published by hand on
  10 November, so Thomas could try it over LTE while away from the workstation.

**Before this pile (Thomas, now)** - *decided against on 9 November: only Thomas's unit runs this
firmware, so 0.1.3 stays as it is and v0.1.4 supersedes it.*

- meshmaster.dk: Firmware → 0.1.3 → **Withdraw**.
- GitHub: make **v0.1.2** *Latest* again. Put one line at the top of v0.1.3's notes: *Do not
  install. This build cannot start on a Walter: it runs out of memory before the firmware does. A
  unit offered it over LTE goes back to 0.1.2 by itself; a unit flashed with it by cable restarts
  in a loop until 0.1.2 is flashed again.*
- Keep `out/images/` (0.1.3's ELF and map, built at a990bf3) until 0.1.4 is out. 01 can use them,
  but it rebuilds both tags anyway.

**Not on the board.** No session opens a port.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [room-for-the-main-task](01-room-for-the-main-task.md) | — | 0.1.2 and 0.1.3 measured; a model of the heap at the main task's creation that explains both; the release build starts with a stated margin |
| 02 | [a-ceiling-on-the-heap](02-a-ceiling-on-the-heap.md) | 01 | a rail in `tools/verify.sh` that fails a build whose main task would not fit, with a margin; `heap` at boot and on the console, for the bench to check the model |
| 03 | [a-rollback-says-what-killed-it](03-a-rollback-says-what-killed-it.md) | — | a rolled-back trial carries the reset reason and the core dump's panic line to the journal, the apply's result and `twinoak.fw.state`; `coredump` on the console |
| 04 | [a-release-starts-first](04-a-release-starts-first.md) | 01–03 | a release boots in the pinned image's QEMU before it is signed, on a tag, a dry run and locally; the docs for 0.1.4. Publishes nothing: v0.1.4 went out by hand on 10 November |

```powershell
.\tasks\run-all.ps1 -Phase '11'
```

04 was stopped once for a bench record; that step is gone (see 04). Resume with
`.\tasks\run-all.ps1 -Phase '11' -From 04`.

## Models

- **Opus**: all four.

## Afterwards (Thomas)

- **Over LTE, from v0.1.2** (10 November): import v0.1.4 on meshmaster.dk, canary for LTE Exit Node
  1, *Offer a release*, *Apply the candidate*.
- **The bench, when there is time:** the dry-run factory image by cable, for the `heap:` line on
  silicon against `docs/bench.md` §1's figures.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
