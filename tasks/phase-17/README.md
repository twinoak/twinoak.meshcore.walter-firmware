# Phase 17 — room for the PSRAM

Written on 19 November.

**What happened.** Pile 16's task 05 put a session's room into the module's PSRAM, tested it,
booted it where there is no PSRAM, and merged its report alone. Switching the PSRAM on costs
5 904 bytes of static memory, and before `app_main` the second of three big blocks fits its
heap with 644 bytes to spare. With the PSRAM on it does not fit, the main task then has no
stack, and rail R22 refused the image. The report's number to beat is 5 256 bytes, and it found
no single object of ours that big.

**What was found on 19 November**, in the linker map of that night's build
(`out/images/twinoak-walter.map`): of 217 kB of `.bss`, 154 kB are our own objects, and the
biggest is the update verdict at 35 472 bytes. 20 480 of those are the manifest parser's token
table: 1 024 tokens, sized for a release of eight images of seven files each. The Walter's own
manifest is 64 tokens and the repeater's 167.

**Two tasks.** 01 sizes the table for the releases there can be, six images of five files,
and gives back at least 7 680 bytes. 02 brings task 05's implementation back from its two
commits on `main`, measures the start again, and writes the words that task left unwritten. If
the start still does not hold, 02 leaves the firmware as 01 left it and merges its report
alone.

```powershell
.\tasks\run-all.ps1 -Phase '17'
```

**Chosen here, and yours to change before the run:** six images and five files as the most a
release may hold, and 640 tokens. Today's biggest release is two images of four files.
A release beyond a bound is refused at check 2, as one beyond eight and seven is today.

**Not on the board.** No test here brings a PSRAM up. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-manifest-of-six-images](01-a-manifest-of-six-images.md) | — | a manifest may hold six images of five files, in 640 tokens; the start has at least 7 680 bytes more to spare in its tightest block |
| 02 | [the-psram-again](02-the-psram-again.md) | 01 | task 05 of pile 16 merged after all: a step's room is 64 kB of PSRAM where there is PSRAM and 8 192 bytes of the heap where there is not; or, if the start does not hold, a report alone |

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **The bench, before any release** (the ⚠ items task 02 writes into `docs/bench.md`; no test
  can bring a PSRAM up, so a board is the first proof). On a Walter alone: `heap room` says
  `psram of 2097152`, the boot's `heap:` line has lost no kilobyte against the release before,
  and a wake with no session is as long as it was. On a board with a radio:
  `rak session 60 scope 863 870 250 0 1000` gives sixty lines and a `room` line saying
  `psram`.
- **Then the release**, as pile 16's README has it. With it, a session started from
  meshmaster.dk has 64 kB a step, which is what the program its task 237 proposes was sized
  for. Until then each scope step fills 8 192 bytes after some fifteen frames and ends `full`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
