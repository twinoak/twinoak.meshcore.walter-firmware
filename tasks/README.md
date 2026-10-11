# Tasks — how the firmware gets built

## Where things are decided

| | Where | In this repository |
|---|---|---|
| what a unit is, the decisions (`S-1`…) | `twinoak.meshcore.standard-repeater-design-v2/software/spec/SYSTEM.md` | a copy, `spec/SYSTEM.md` |
| the link to the repeater | …`/LINK-PROTOCOL.md` + fixtures | a copy, `spec/`; spoken by the link client |
| the release manifest, its signature, the image descriptor | …`/FIRMWARE-MANIFEST.md` + fixtures | a copy, `spec/`; written by the release tooling, **checked by the firmware** |
| the pins and their rules | …`/hardware/pcb/INTERCONNECT.md` (**frozen**), `BENCH-BREADBOARD.md` | read from there, by link, at a commit; `docs/ARCHITECTURE.md` §2 |
| the modem | `QuickSpot/walter-esp-idf` (`dptechnics/walter-modem`) | a managed component, pinned |
| the nRF52's debug port and flash controller | Arm's ADIv5 and Nordic's product specification | cited where used |
| how this firmware is put together | — | `docs/ARCHITECTURE.md` |
| the MeshMaster agent | not decided (`spec/proposals/`) | a seam, nothing more |

`spec/` is copied, never edited: `python spec/tools/specsync.py verify spec`
holds every file against `spec/MANIFEST.txt`, in `tools/verify.ps1` and in CI.
When a task finds the specification wrong or silent, it says so in its report
and takes the smallest decision that fits; the reviewer changes the
specification where it lives and copies it again.

## Conventions

- **One task, one branch, one PR.** Branch `task/p1-03-the-swd-wire` for
  `tasks/phase-1/03-the-swd-wire.md`. The PR description is the task's
  acceptance checklist, ticked.
- **Read before writing.** Every task lists what it implements. Read those
  sections in full before touching code.
- **Cite what you rely on.** A register address, a bit, a timing, a sequence:
  the comment beside it names the document and section it came from. A
  simulator is only as true as its sources.
- **Logic on the host, pins behind an interface.** `components/core/` has no
  ESP-IDF include. What it needs of the world it takes as an interface from
  `components/hal/`; `host_test/` gives it fakes and simulators, the firmware
  gives it the real thing. A task that adds logic adds host tests, with the
  failure cases first: this firmware's whole purpose is what happens when
  something goes wrong.
- **Verification is part of the task, and it is local.** A task is done when
  `tools/verify.ps1` and the task's own commands pass on this machine, and
  then in CI. Warnings are errors in our own components; the host tests run
  under the address and undefined-behaviour sanitizers.
- **No scope creep.** If a task needs something a later task owns, stub it
  behind a seam and say so in the PR.
- **Small, deliberate dependencies**, each pinned exactly (a component's
  version in `idf_component.yml` and the lock file; a vendored source file
  with its origin, version and hash in a header). Adding one needs a one-line
  justification in the PR.
- **Secrets never in the repo**: no token, no SIM PIN, no password, no private
  key — not in a default, a fixture, a log line or a report.
- **Docs move with code.** A task that changes what the console says, how the
  board is flashed or what an update does updates `docs/` in the same PR, and
  adds its bench lines to `docs/bench.md`.
- **Leave the evidence.** Build logs, images and test output stay on disk in
  `out/` after the run (git-ignored).

## Unattended runs

`tasks/run-all.ps1 -Phase N` runs the tasks of a phase in order, each in a
fresh headless session (`claude -p --dangerously-skip-permissions`) that
follows [`RUNBOOK.md`](RUNBOOK.md): branch, implement, verify locally, write
`phase-N/reports/NN.md`, open a PR, wait for CI, merge. A task counts as done
when its report exists on `origin/main`; the first task that does not land
stops the run (`phase-N/logs/STOPPED.md`), and `-From NN` resumes.
`tasks/run-pile.ps1` runs phase after phase and stops where a phase stops.
Review findings go into `phase-N/FOLLOWUPS.md`, tagged with the task that must
pick them up. Merging never releases; a release is a person pushing a tag.

## The piles

| Phase | What | Tasks |
|---|---|---|
| [0](phase-0/README.md) | the repository: toolchain, CI, release, the board's pins | 3 |
| [1](phase-1/README.md) | hands on the radio: the link, the debug port, the nRF52, the flasher | 6 |
| [2](phase-2/README.md) | images and updates: signatures, the store, the radio's update, its own, the supervisor | 5 |
| [3](phase-3/README.md) | the uplink and staying alive: the modem, HTTPS, settings, the duty cycle, the wake cycle | 6 |

Not in these piles, on purpose: anything that needs the rig (a bench pile
follows the first boards); the MeshMaster agent and the TwinOak pack's jobs
(`spec/proposals/` — nothing here is written against a guess about the
server); a NoiseScope session; the dual adapter's second radio (the seam is
there from phase 1, task 06).
