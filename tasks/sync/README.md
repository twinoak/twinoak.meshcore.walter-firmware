# agent-core sync tasks

Tier two of keeping up with the agent core. Tier one is the weekly check — a
workflow that notices a newer `vX.Y.Z` of
`twinoak/twinoak.meshmaster.exitnode-firmware` and, when it can read that
repository, opens the re-copy as a pull request — and this is what happens when
that bump is **red**: `tools/new-sync-task.ps1 -Tag vX.Y.Z` writes a task file
here from [`TEMPLATE.md`](TEMPLATE.md) with the tag, the pull request and the
failing job's log in it, and the driver runs it like any other task:

```powershell
.\tools\new-sync-task.ps1 -Tag v0.3.0 -Pr 12 -LogUrl <the failing job>
.\tasks\run-all.ps1 -Phase sync
```

[`docs/upstream.md`](../../docs/upstream.md) is the page that explains all three
tiers and holds the list of the places a bump re-reads; `TEMPLATE.md` is that
list as a session's checklist. Reports land in [`reports/NN.md`](reports/README.md)
as everywhere else.

A sync task of this repository branches from `main` like any other task **unless
the watch had a credential and pushed `upstream/<tag>` already**, in which case
it continues that branch (`tasks/RUNBOOK.md` section 1). The difference from the
repeater firmware's sync tasks, where the branch is always there, is the one
thing the agent core's privacy costs: see *What the watch cannot do without a
credential* in `docs/upstream.md`.

## Order and dependencies

Oldest first, as every other pile's table is. Each one is independent — a bump
is a bump — but they are numbered in the order they were written, and the driver
runs them from the first row's number to the last's.

<!-- new-sync-task.ps1 appends its row after the last one, in the driver's
     format: | NN | [name](NN-name.md) | tag | what happened | -->

| # | Task | Tag | Why it needed a session |
|---|---|---|---|
| 01 | [agent-core-v0-7-3](01-agent-core-v0-7-3.md) | v0.7.3 | CI red on pull request #87 |
| 02 | [agent-core-v0-7-7](02-agent-core-v0-7-7.md) | v0.7.7 | CI red on pull request #92 |
