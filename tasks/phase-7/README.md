# Phase 7 — the first release

Written on 5 November. Thomas powered up the first unit: its repeater came up
on the mesh, and its Walter did not. There is nothing released to flash, no
guide, and no release notes. One task.

**Prerequisite:** the release key, which Thomas made on 20 October: key id
`73efdd383a957596`, its public half in `keys/release-keys.txt`, the private half
in `RELEASE_SIGNING_KEY` here and in the repeater firmware. Nothing to do.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-first-release](01-the-first-release.md) | — | `docs/flashing.md`, a hand-written top to the release notes, and `v0.1.0` published as *Latest*, verified against the trust table |

```powershell
.\tasks\run-all.ps1 -Phase '7'
```

Run the repeater firmware's pile 3.7 too: it is the same key, and the guide here
points at its release.

## Models

- **Opus** — 01.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
