# The hand-written top of a release

One file per tag, `vX.Y.Z.md`, and it is what a person reads first when they open the release:
`tools/release/build_release.py` puts it in front of the generated `NOTES.md`, with a rule between
the two. **A tag whose file is missing is refused**, before anything is signed or published, the
way a tag with no release key is ([`../docs/release.md`](../docs/release.md)) — a release nobody
wrote a word about is not a release.

A dry run (*Actions → release → Run workflow*) and the local rehearsal in `tools/verify.ps1` are
versions of another shape (`0.0.0-dryrun+<sha7>`, `0.0.0-dev`), so they need no such file and get
the generated part alone, which says in its own first paragraph that nothing is published.

## What belongs in one

Short — a screen, not a page. Markdown, no front matter, no title line of its own (the release's
title is `TwinOak Walter firmware vX.Y.Z`). Four things, because they are the four a person asks:

1. **what this release is**, in a sentence or two, and **what has and has not been on a board**;
2. **how to put it on a module**: a link to [`../docs/flashing.md`](../docs/flashing.md), which is
   the guide, rather than the commands again;
3. **which release of `twinoak.meshcore.repeater-firmware` it goes with** — a unit is two boxes and
   one key signs both;
4. anything a person must **do by hand** when they take this release after the one before it.

What does not belong: the version, the commit, the ESP-IDF and modem-library versions, the
`esptool` line, what a factory flash keeps, and how to check the signature. All of those are
written by `build_release.py` under the rule, from the build itself, and a hand-written copy of
them is one more thing that can be wrong.

The file is reviewed in the pull request that is tagged, like any other file here.
