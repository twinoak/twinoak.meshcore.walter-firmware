# 03 — The notes say what is true

**Depends on:** —
**Spec:** `.github/workflows/release.yml` (the publish job's last step: the comment at ~L393, the pull request's body at ~L433), `docs/release.md` (*After a release: the notes go back to main*), `tasks/phase-13/FOLLOWUPS.md` (the item tagged *next pile*)
**Size:** one session, a short one.
**Model:** claude-opus-5

## Goal

After a release the publish job offers one commit to `main` as a pull request: the released
lines moved out of `release-notes/unreleased/`, and `VERSION` moved on. Its body, the comment
above it and `docs/release.md` all say that no check runs on that pull request, because GitHub
starts no workflow from an event raised with the job's own token.

One does. For v0.1.5's pull request (#60) GitHub started a `ci` run and **held it for a
maintainer's approval** (*action required*). Unapproved, such a run expires after about an hour
and is listed as failed, which is what the repeater's did on 13 November. Thomas found a red run
an hour after a release that had gone well, and the page he was sent to said the run could not
exist.

What the job does is right and stays: the run ends green, the summary says the notes are still
waiting for `main`, and the pull request is merged by a person. Only the sentences are wrong.

## Deliverables

1. **The pull request's body** says what happens: GitHub holds this pull request's `ci` run until
   a maintainer approves it; approve it and merge when `verify` is green, or merge without
   waiting; a run nobody approved is listed as failed after about an hour, and that failure is
   about nothing.
2. **`docs/release.md`**, the same, in its section on the notes going back to `main`, and the
   sentence further down that says the commit *has no CI run of its own and never will*, if what
   it rests on is the same belief. Read what the release gate does with a held or an expired run
   on that commit (`tools/release/release_notes.py`) before changing a word about it, and say in
   the report what it does.
3. **The workflow's own comments** that say the same thing (~L143, ~L393): true, or gone.
4. **Nothing the job does changes.** No step is added, removed or reordered, and the YAML that is
   not a comment or the body's text is byte for byte what it was. Show it in the report with a
   diff.
5. **A line in `release-notes/unreleased/`** for this task.

## Acceptance criteria

- [ ] The body the job writes names the held run and both ways out, and no longer says that no check runs (read it out of the built file in a test, if the body is testable where it stands; if not, say so and quote it in the report).
- [ ] `docs/release.md` and the workflow's comments say nothing that #60's history contradicts.
- [ ] The diff of `release.yml` touches comments and the body's text only.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Making the pull request merge by itself. Changing which checks `main` requires: a repository's
settings are Thomas's. A release: Thomas presses the button.
