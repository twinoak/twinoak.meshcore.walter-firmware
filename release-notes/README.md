# The words a release goes out with

A release of this firmware is published by a person pressing *Run workflow*, and its body is
written before that, a line at a time, by the tasks that changed something
([`../docs/release.md`](../docs/release.md)). Nothing is composed at release time and nothing is
summarised afterwards.

| | |
|---|---|
| [`unreleased/`](unreleased/) | one file per task, `<task>.md`, holding **one line**: what a person with a unit will notice, and what has and has not been on a board. The release takes them all |
| [`standing.md`](standing.md) | the part that is true of every release and cannot be generated — which repeater firmware this goes with, how a unit takes an update. The task that changes it edits it |
| `vX.Y.Z.md` | what a release **was** published with, filed by the workflow after the release is out. Read, never edited |
| [`../VERSION`](../VERSION) | the version the next release gets. The workflow reads it from there and from nowhere else, and moves it to the next patch after a release |

`tools/release/release_notes.py` is all of it: it reads the lines in the order the tasks merged,
puts the standing part under them, and `tools/release/build_release.py` puts that in front of the
generated part of `NOTES.md`. **A release with no line waiting is refused**, before anything is
built, the way one with no release key is — a release nobody wrote a word about is not a release.

## What a line is

> A unit that rolls back now says why: the reset that ended the trial and the panic line from its
> core dump. Seen in QEMU and on the bench, not yet on a mast.

One or two sentences, plain words, at most 400 characters, no heading and no front matter, in
`unreleased/<task>.md` — named for the task, which is what gives the lines an order and keeps two
tasks from writing the same file. Two things belong in it:

1. **what a person with a unit will notice** — not what was done, and naming no task, decision or
   section of the specification;
2. **what has and has not been on a board.** This firmware is flashed onto a mast: a line that
   does not say where its change has been seen leaves the person who must decide whether to take
   the release with nothing to decide on.

**A change nobody can see writes no file.** CI, a refactoring, tests, a document, a rail: the task
leaves no line and the release is shorter for it. `tasks/RUNBOOK.md` says the same, where a task
reads it.

## What is already said for you

The four things a person asks of a release are answered without anybody writing them twice. What
can be generated is generated, from the build itself:

1. **what this release is**, and what has and has not been on a board — the lines;
2. **how to put it on a module** — the generated part: the link to
   [`../docs/flashing.md`](../docs/flashing.md) at the commit being released, the one `esptool`
   line, and what a factory flash keeps;
3. **which release of `twinoak.meshcore.repeater-firmware` it goes with** — `standing.md`, because
   nothing in a build knows it;
4. anything a person must **do by hand** — a line of the task that made it necessary.

What does not belong in a line: the version, the commit, the ESP-IDF and modem-library versions,
the `esptool` line, what a factory flash keeps, and how to check the signature. All of those are
written by `build_release.py` under the rule, from the build itself, and a hand-written copy of
them is one more thing that can be wrong.

A dry run (*Actions → release → Run workflow* with **dry run** ticked) and the local rehearsal in
`tools/verify.ps1` are versions of another shape (`0.0.0-dryrun+<sha7>`, `0.0.0-dev`), so they
need no line and are refused for nothing — but they do show the waiting lines in their `NOTES.md`,
which is how a line is read before it is published.

Every line is reviewed in the pull request that brings it, like any other file here.
