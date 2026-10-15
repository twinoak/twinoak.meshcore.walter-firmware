# Runbook — one task, unattended

You are Claude Code running headless (`claude -p`): nobody is watching, nobody
answers questions. `tasks/run-all.ps1` started you with "Run task NN of phase
P". Everything below uses `tasks/phase-P/` for that phase. Do exactly this,
in order. Everything you decide goes into files in the repo, never into the
console.

This is twinoak/meshmaster-agent-firmware's runbook, for this repository.
Three things differ from it and are worth knowing before the rest: the trunk
here is **`main`**; **verification is local first** (§2) — CI confirms what
the machine you are on has already shown; and the toolchain is **ESP-IDF in a
container**, not PlatformIO.

## 0. Orient

- You are on `main`, clean and up to date (the driver checked). If not:
  `git checkout main && git pull --ff-only`.
- Read `tasks/README.md`, `tasks/phase-P/README.md`, then
  `tasks/phase-P/NN-<name>.md`, then every section of `spec/` and `docs/` that
  task references, in full. **`spec/` wins over the task file**; note any
  disagreement in the report. `spec/` is a copy and is never edited here
  (`spec/README.md`).
- Read `tasks/phase-P/FOLLOWUPS.md`. Every open item tagged `[NN]` for your
  task is part of your task. Items tagged for other tasks are not yours; do
  not tick anything — the reviewer does.
- Read `tasks/phase-*/reports/*.md` for the tasks yours depends on: their
  "deviations" are what you inherit. Phase 0's task 01 report is the one to
  read before touching the build, `sdkconfig.defaults`, the partition table,
  CI or the release.
- **Read what you build against** rather than trusting a transcription of it,
  this file's and the task's included: ESP-IDF is in the build image
  (`$IDF_PATH`), the modem library is under `managed_components/` after a
  build, and the documents a task cites for the nRF52 and the debug port are
  Nordic's and Arm's own. Where code depends on a register, a bit or an
  address, a comment cites the document and section it came from.

**Other repositories.** A task may tell you to read a file of a sibling
repository at a commit. Read it without touching that repository's working
tree — other sessions are working in some of them:

```bash
gh api "repos/twinoak/<repo>/contents/<path>?ref=<commit>" -H "Accept: application/vnd.github.raw"
# or, from the sibling clone, a read that changes nothing:
git -C ../<repo-folder> show <commit>:<path>
```

Never check out, pull, commit or write in a sibling repository. The two
MeshMaster repositories (`meshmaster.dk`, `meshmaster-agent-firmware`) and the
hardware repository are read-only to every task here.

## 1. Branch

`git checkout -b task/pP-NN-<name>` from `main`. One task, one branch.

**If that branch already exists** (locally, or on `origin` — check both), a
previous session of this task was cut off: a usage limit, a crash, a closed
lid. Do not delete or reset it and do not start over. `git checkout` it,
`git pull` if it has a remote, read `git log main..HEAD` and any draft of
`reports/NN.md` — the reviewer may have moved one to
`tasks/phase-P/logs/NN-report-draft.md`, which is git-ignored so that an
unfinished report cannot reach `main` and make the driver think the task
landed — run the verification to see where it stands, and continue from
there. The commits already made are the task's; yours are added after them.
Say in the report that the task was done in more than one session.

## 2. Implement and verify — locally, first and for real

- The conventions in `tasks/README.md` apply: spec wins, no scope creep,
  libraries pinned to an exact version with a one-line justification, secrets
  never in the repo, docs move with code, leave the evidence.
- **One command verifies the repository: `tools/verify.ps1`** (phase 0's task
  01 writes it; until it exists, the task's own commands are the
  verification). It runs what CI runs — the check of `spec/` against its
  manifest, the host tests, the firmware build — inside the pinned ESP-IDF
  image, and CI runs *it*. Run the task's own verification commands as well.
  Fix until green. Warnings are errors in our own components.
- **Green locally is the bar for pushing.** Do not push to see what CI says.
  GitHub Actions minutes are not where this project debugs.
- **Everything runs in the container.** Nothing of ESP-IDF is installed on
  this machine and nothing should be. `tools/verify.ps1` is the way in; by
  hand, the same image and the same mounts it uses. In Git Bash, prefix a
  `docker run` with `MSYS_NO_PATHCONV=1` or its paths are mangled into drive
  letters.
- **Most of this firmware is tested on the host.** The logic lives in
  components with no ESP-IDF include and is tested, with fakes and simulators,
  by `host_test/` — that is where a task spends its time. The firmware build
  proves the same code compiles and links for the board. A task that adds
  logic adds host tests; a task whose only proof is "it builds" has probably
  put logic in the wrong place.
- **Run the long commands in the foreground and let them finish inside the
  same tool call.** A clean firmware build takes minutes and the first run
  pulls a large image; raise that call's timeout to suit. Never start one in the
  background and then end your turn to wait for a completion notification: a
  headless session with nothing left to do *ends*, and the task dies there —
  the run unfinished, the report unwritten, the branch unpushed. Block on it
  inside the turn, or poll it inside the turn.
- **A session cannot flash a board, attach to a network or wiggle a pin** —
  not in these piles. It can build and run the host tests. Everything else is
  Thomas's to walk with the rig on the bench, so every task ends with a *Not
  verified here* list precise enough to be walked, and the same lines are
  added to `docs/bench.md` in the order the rig is brought up
  (`spec/SYSTEM.md` §7).
- Nobody will answer a question. Where the spec and the task are silent, take
  the smallest decision consistent with the spec and record it in the report
  under "Decisions taken without asking".
- Never: `git push --force`, delete a branch other than your own, write a
  secret anywhere (including the report and the logs), edit a file under
  `spec/` or `managed_components/`, tell a TLS client to skip verification,
  or add a code path that erases the nRF52's whole flash or its UICR without
  a person's explicit command — the unit exists because radios were bricked,
  and a test holds each of these.

## 3. Report

Write `tasks/phase-P/reports/NN.md` and commit it on the branch. It is the
PR description and what the reviewer reads:

```
# pP-NN — <name>

## Acceptance checklist
- [x] <criterion> — <one line: how it was verified>
...
## Deviations from the spec or the task
## Decisions taken without asking
## New libraries (with the one-line justification)
## Not verified here (and why)      <- the bench list: what Thomas must do with the rig
```

## 4. PR, CI, merge

**With `gh` (installed and logged in):**

1. `git push -u origin task/pP-NN-<name>`
2. `gh pr create --title "pP-NN: <name>" --body-file tasks/phase-P/reports/NN.md`
3. `gh pr checks --watch --fail-fast`, **in the foreground, in calls of at most
   nine minutes**: a CI run takes ten to fifteen minutes here, and one tool call
   may not take more than ten, so `timeout 540 gh pr checks --watch --fail-fast`
   and, when it times out, the same again - until it answers. Never in the
   background, never with a monitor, never by ending the turn to be told: a
   headless session is woken by nothing, and p3-06's first session ended that
   way with its pull request open and unmerged (the driver now continues such a
   session once or twice, but a nudge is a second session's cost).
4. Green: `gh pr merge --merge --delete-branch`, then
   `git checkout main && git pull --ff-only`.

**CI is red after a green local run.** That is a finding about
`tools/verify.ps1`, not a place to iterate. `gh run view --log-failed` on the
failing run; find what CI ran that the local script did not, or ran
differently; **fix the script so that it would have caught it**, reproduce the
failure locally, fix it locally, and push once. The report says what the gap
was, under "Deviations". **One round.** Red a second time is a stop condition
(§5). The only exception is phase 0's task 01, which writes CI itself and has
three rounds for the workflow file alone.

**Without `gh`:** after the local verification is green,
`git checkout main && git pull --ff-only && git merge --no-ff
task/pP-NN-<name> -m "Merge task/pP-NN-<name>" && git push && git branch -d
task/pP-NN-<name>`. CI runs on `main`; the reviewer watches it.

The driver considers the task done when `tasks/phase-P/reports/NN.md` exists
on `origin/main`. Nothing else counts. Merging never releases: a release is a
person pushing a tag `vX.Y.Z` (or pressing *Run workflow* for a dry run).

## 5. Stop conditions

Stop — do not merge, do not continue — when: CI is red a second time (§4); an
acceptance criterion cannot be met; a step needs a secret or a human (a board
on a bench, a SIM, a GitHub secret, the release key); Docker is not running;
**the design the task rests on turns out not to hold** — say what does not
hold and what would. Then: leave the branch pushed (PR open if `gh` exists),
write `tasks/phase-P/logs/STOPPED-NN.md` (git-ignored) saying exactly what is
missing and what was completed, and end. The driver stops the run and the
human reads that file.

## Phase-specific

**Phase 0.** Task 01 is the toolchain and everything after it inherits its
decisions: the ESP-IDF version and image, how the modem library is consumed,
the partition table, what `tools/verify.ps1` runs. Task 03 fixes how this
firmware treats the pins it shares with the radio; every later task that
touches a pin goes through it.

**Phase 1.** The debug port, the nRF52 and the flasher (03–05) are developed
entirely against a simulator the tasks build. The simulator is only as good as
the documents it was written from: cite them, and when a document and a habit
disagree, the document wins. **Nothing in this phase may contain a path that
erases the whole chip or UICR automatically** — the flasher detects a
protected chip and refuses; opening one is a bench procedure for a person.

**Phase 2.** An update is journalled before each step and tested against a
flash that loses power at every step. "It worked when nothing went wrong" is
not a passing test here.

**Phase 3.** Nothing talks to MeshMaster yet (`spec/SYSTEM.md` §8). The wake
cycle ends at a backend interface with a null implementation; do not invent a
server protocol to fill it.
