# Phase 12 — the update that held

Written on 12 November, after reading meshmaster.dk's pages for LTE Exit Node 1.

**What happened.** On 10 November the unit took `v0.1.4` over LTE from `v0.1.2`: one offer stored
in 14 range requests, one apply, *committed*, and the heartbeat has reported `0.1.4+62538fa` since.
Three offers before it expired on the server because the unit did not check in within 30 minutes
of any of them. [evidence/](evidence/README.md) has the pages as read.

**What is left.** Words. README and `docs/flashing.md` say the result is *not known*, and
`docs/bench.md` §18's item is unticked and written for `v0.1.3`. Two things on the pages need an
answer from the code: two ranges fetched twice with nothing saying so, and three journal lines
stamped with the same second.

**Not on the board.** No session opens a port. No release.

**13 November: 02 added.** Thomas wants the firmware repositories to release the way
meshmaster.dk does: *Run workflow* publishes, the notes are the tasks' own lines, and the version
is in the repository. 02 builds that here. It publishes nothing itself.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-update-that-held](01-the-update-that-held.md) | — | README, the flashing guide and the bench's §18 say what the unit did on 10 November and what was not watched; the two questions answered from the code |
| 02 | [a-release-from-the-button](02-a-release-from-the-button.md) | 01 | *Run workflow* publishes the version the repository names, booted and signed, with notes compiled from the tasks' own lines, and then moves the version on; a dry run by a tick; the tag trigger gone |

```powershell
.\tasks\run-all.ps1 -Phase '12'
```

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **The bench, when there is time:** unchanged from phase 11.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
