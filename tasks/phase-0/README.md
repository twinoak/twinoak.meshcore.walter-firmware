# Phase 0 — the repository

Three tasks that turn a repository holding documents into one that builds, tests
and releases — and that knows, before any other code exists, how to treat the
seven pins it shares with the radio.

Nothing here needs a Walter on a desk. At the end of this phase there is a
firmware image that boots to a console and does nothing dangerous, a host test
suite that runs in seconds, and a release whose manifest the specification's
own tool accepts.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-toolchain](01-the-toolchain.md) | nothing | an ESP-IDF project that builds in the pinned image, with the modem library resolved; host tests; the partition table; `tools/verify.ps1`; CI |
| 02 | [the-release](02-the-release.md) | 01 | a tag makes a GitHub Release with the OTA and factory images, a manifest, its signature and `SHA256SUMS`; the descriptor is in the image from the start |
| 03 | [the-board](03-the-board.md) | 01 | the pins and their rules, tested against fake pins; a console with `status`, `rak reset`, `rak kill`; why the last boot happened |

01 → 02 → 03.

```powershell
.\tasks\run-all.ps1 -Phase 0
```

## Models

- **Fable** — 01, 03. The toolchain is inherited by everything; the pin rules
  are what stands between a bug and a radio that is held in reset on a mast.
- **Opus** — 02.

## The exit criterion

`tools/verify.ps1` is green on this machine and in CI; a dry run of the release
produced a manifest and two images that `spec/tools/manifestref.py` validates,
each image with one descriptor; and no sequence of console commands can make
the fake pins break a rule of `docs/ARCHITECTURE.md` §2.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
