#!/usr/bin/env python3
"""The version this repository releases next, and the notes that release is published with.

Task p12-02. Before it, a release was a pushed tag and its body was a file somebody wrote at
tagging time (`release-notes/vX.Y.Z.md`). Now the button publishes, and the two things a tag used
to carry by hand are in the repository instead:

    VERSION                                 the version the NEXT release gets: X.Y.Z, one line.
                                            The workflow reads it from here and from nowhere
                                            else, and moves it on to the next patch after a
                                            release (`publish` below).
    release-notes/unreleased/<task>.md      one line per task that changed something a person
                                            with a unit will notice: what they will notice, and
                                            what has and has not been on a board. A change
                                            nobody can see leaves no file.
    release-notes/standing.md               the part of a release's body that is true of every
                                            release and cannot be generated - which repeater
                                            firmware it goes with, how a unit takes it. The task
                                            that changes it edits it.
    release-notes/vX.Y.Z.md                 what was published, written here by `publish` after
                                            the release is out. Never edited afterwards.

The body a release is published with is this file's `body()`: the waiting lines, in the order the
tasks merged, then the standing part. `tools/release/build_release.py` puts it in front of the
generated NOTES.md exactly where the hand-written file used to go, and refuses a release that has
none - a release nobody wrote a word about is still not a release.

    python tools/release/release_notes.py version        # 0.1.5
    python tools/release/release_notes.py tag            # v0.1.5
    python tools/release/release_notes.py next           # 0.1.6
    python tools/release/release_notes.py lines          # the waiting files, in merge order
    python tools/release/release_notes.py body           # the top of the next release's body
    python tools/release/release_notes.py preflight --ref refs/heads/main
    python tools/release/release_notes.py publish        # the move, after a release is out

`preflight` is what the workflow runs before anything is built, and each of its three refusals is
one sentence (deliverable 5 of the task):

  * no line is waiting: there is nothing to release;
  * the tag of VERSION already exists: the version was not moved on after the last release;
  * a release asked for from anything but `main`.

Nothing here signs, publishes, or talks to GitHub. The merge order comes from `git log`; outside a
git repository (a test's scratch directory, a tarball) the files are taken in name order.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Where the three live, relative to the repository root. `build_release.py` and the workflow use
# these names; a test passes its own directory instead.
VERSION_FILE = "VERSION"
NOTES_DIR = os.path.join(REPO, "release-notes")
UNRELEASED = "unreleased"
STANDING = "standing.md"

# X.Y.Z and nothing else: the "+<sha7>" is added by the workflow from the commit, and a version
# with a pre-release part in it would not be the shape sign_manifest.py and the descriptor expect
# (spec/FIRMWARE-MANIFEST.md §2).
VERSION_SHAPE = re.compile(r"^\d+\.\d+\.\d+$")

# A line is a sentence or two, not a page: what a person with a unit will notice, and what has and
# has not been on a board. 400 characters is about three lines of this file - long enough for both
# halves and short enough that a release's top stays a screen. The bound is here, where the
# release reads it, and tools/release/test_release_notes.py holds every file in the repository to
# it, so a line that cannot be published is found by ci and not by the release.
MAX_CHARS = 400


class Refused(Exception):
    """Something a release may not do. The message is one sentence, for a person reading a
    workflow log, and is the whole of what the run's summary says went wrong."""


# ------------------------------------------------------------------ the version


def read_version(repo=None):
    """The version the next release gets, from the one file that says it."""
    path = os.path.join(repo or REPO, VERSION_FILE)
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        raise Refused("%s cannot be read (%s): it is the one place this repository's next "
                      "version is written." % (VERSION_FILE, exc))
    version = text.strip()
    if not VERSION_SHAPE.fullmatch(version):
        raise Refused("%s says %r, and a version is X.Y.Z and nothing else (the commit is added "
                      "by the release as +<sha7>)." % (VERSION_FILE, version))
    return version


def next_patch(version):
    """The version after this one: the patch moved on, which is what a release leaves behind. A
    minor or a major release is a person editing VERSION before pressing the button."""
    major, minor, patch = (int(part) for part in version.split("."))
    return "%d.%d.%d" % (major, minor, patch + 1)


def write_version(version, repo=None):
    path = os.path.join(repo or REPO, VERSION_FILE)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(version + "\n")


def tag_of(version):
    return "v" + version


# ------------------------------------------------------------------ the waiting lines


def _git(repo, *args):
    """git, with its answer as text; None when there is no git, no repository or no answer."""
    try:
        done = subprocess.run(["git", "-C", repo] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, universal_newlines=True)
    except OSError:
        return None
    if done.returncode != 0:
        return None
    return done.stdout.strip()


def _merge_order(repo, directory, names):
    """The files, in the order the tasks that wrote them arrived.

    The commit that *added* a file is the task's own (`--diff-filter=A`), and its committer date
    is when that task's branch was written; a file not committed yet - the line a task is writing
    right now, and every file in a scratch directory with no git in it - is newest, which is where
    it will be when it lands. Ties are broken by name so that two files of one commit have an
    order at all."""
    ranked = []
    for name in names:
        when = None
        rel = os.path.relpath(os.path.join(directory, name), repo) if repo else None
        if rel and not rel.startswith(".."):
            answer = _git(repo, "log", "--diff-filter=A", "--format=%ct", "-1", "--",
                          rel.replace(os.sep, "/"))
            if answer:
                when = int(answer.splitlines()[0])
        ranked.append((when if when is not None else float("inf"), name))
    ranked.sort(key=lambda pair: (pair[0], pair[1]))
    return [name for _, name in ranked]


RELEASE_OWN_FILES = ("VERSION", "release-notes/")


def _is_release_own(names):
    return bool(names) and all(name == RELEASE_OWN_FILES[0]
                               or name.startswith(RELEASE_OWN_FILES[1]) for name in names)


def ci_commit(repo, sha, limit=10):
    """The commit whose ci run speaks for `sha`, and the commits walked past to find it.

    A release leaves a commit behind it: the pull request that files the lines and moves VERSION
    on, opened and merged by the workflow's own token - and **GitHub starts no workflow from an
    event raised with that token**, so that commit has no ci run of its own and never will. The
    next release would then wait 80 minutes for a run that cannot come.

    So the gate walks back along the first parents while a commit changes nothing but VERSION and
    release-notes/, and waits for the ci run of the first commit that changed anything else. That
    is sound because those two files are not compiled into anything: the firmware's bytes come
    from components/ and main/, the version is passed in by the workflow as PROJECT_VER, and the
    lines are read by the release itself, which checks each of them in `preflight` before it
    builds. A commit that touches one byte of anything else stops the walk."""
    probe = sha
    walked = []
    for _ in range(limit):
        parent = _git(repo, "rev-parse", "-q", "--verify", "%s^" % probe)
        if not parent:
            break
        changed = _git(repo, "diff", "--name-only", parent, probe)
        names = [n for n in (changed or "").splitlines() if n.strip()]
        if not _is_release_own(names):
            break
        walked.append(probe)
        probe = parent
    return probe, walked


def _check_line(name, text):
    """What a line must be, so that a release's top is readable without anybody editing it."""
    where = "release-notes/%s/%s" % (UNRELEASED, name)
    if not name.endswith(".md"):
        raise Refused("%s is not a .md file: a release note is one line of markdown named for "
                      "the task that wrote it." % where)
    if "\r" in text:
        raise Refused("%s has carriage returns in it: write it with Unix line endings." % where)
    body = text.strip()
    if not body:
        raise Refused("%s is empty: a release note says what a person with a unit will notice, or "
                      "the file is not written at all." % where)
    if body.lstrip().startswith("#"):
        raise Refused("%s begins with a heading: a release note is a sentence or two of plain "
                      "words, and the release's own headings are around it." % where)
    if "\n\n" in body:
        raise Refused("%s is more than one paragraph: a release note is a sentence or two - what "
                      "a person will notice, and what has and has not been on a board." % where)
    if len(body) > MAX_CHARS:
        raise Refused("%s is %d characters and a release note may be %d: say what a person will "
                      "notice and what has been on a board, and leave the rest to the task's "
                      "report." % (where, len(body), MAX_CHARS))
    return " ".join(body.split())


def unreleased(notes_dir=None, repo=None):
    """The waiting lines, in merge order: [(file name, the line)]. Each is checked as it is read,
    so a file that could not be published stops the release before anything is built."""
    notes_dir = notes_dir or NOTES_DIR
    directory = os.path.join(notes_dir, UNRELEASED)
    if not os.path.isdir(directory):
        return []
    names = [n for n in os.listdir(directory) if not n.startswith(".")]
    lines = []
    for name in _merge_order(repo or REPO, directory, sorted(names)):
        # newline="": a line written on Windows must be refused for its carriage returns, and
        # python's text mode would translate them away before anything could see them.
        with open(os.path.join(directory, name), encoding="utf-8", newline="") as fh:
            lines.append((name, _check_line(name, fh.read())))
    return lines


def standing(notes_dir=None):
    """The part of a release's body that is true of every release and cannot be generated. Absent
    or empty is allowed: it is prose, not a gate."""
    path = os.path.join(notes_dir or NOTES_DIR, STANDING)
    if not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8") as fh:
        return fh.read().strip("\n")


def body(notes_dir=None, repo=None, lines=None):
    """The top of the next release's body, or None when no line is waiting.

    The four things release-notes/README.md asks a release to say: what this release is and what
    has and has not been on a board (the lines), how to put it on a module (the generated part
    below the rule, which links the flashing guide), which repeater firmware it goes with (the
    standing part), and anything to do by hand (a line says so)."""
    lines = unreleased(notes_dir, repo) if lines is None else lines
    if not lines:
        return None
    out = ["## What is new", ""]
    out += ["- %s" % text for _, text in lines]
    rest = standing(notes_dir)
    if rest:
        out += ["", rest]
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ the three refusals


NOTHING_TO_RELEASE = """there is nothing to release: no task has left a line in \
release-notes/unreleased/.

A release is the lines the tasks wrote. A change somebody with a unit will notice has a line - one
or two sentences, what they will notice and what has and has not been on a board - written by the
task that made the change, in its own pull request (release-notes/README.md, tasks/RUNBOOK.md). A
change nobody can see has none, and a run of releases with nothing to say is a release that should
not happen."""

ALREADY_RELEASED = """the tag %s already exists: %s was released and the version was not moved on.

A release moves VERSION to the next patch in a pull request of its own, opened and merged by the
release workflow. That pull request was not merged - main's rules may not let the workflow merge
it - so it is still open: merge it, or edit VERSION to the version this release should carry, and
press the button again."""

NOT_MAIN = """a release is made from main's head and from no other commit, and this run's ref is \
%s.

Press *Run workflow* with the branch left at `main`, and leave the `ref` box empty: only a dry run
may rehearse another commit."""


def existing_tags(repo=None):
    """Every tag this clone knows. The workflow fetches them before it asks (a shallow checkout
    has none, which would answer "no release yet" to every question)."""
    answer = _git(repo or REPO, "tag", "--list")
    return set(answer.splitlines()) if answer else set()


def preflight(version, ref, tags, lines):
    """The three things that stop a release before a byte is built. Each raises one sentence."""
    branch = ref.split("refs/heads/")[-1] if ref else ""
    if branch != "main":
        raise Refused(NOT_MAIN % (ref or "(none)"))
    tag = tag_of(version)
    if tag in tags:
        raise Refused(ALREADY_RELEASED % (tag, version))
    if not lines:
        raise Refused(NOTHING_TO_RELEASE)


# ------------------------------------------------------------------ the move, after a release


def publish(version, notes_dir=None, repo=None, text=None):
    """What the release's own pull request carries: the lines as published, and the next version.

    `release-notes/vX.Y.Z.md` is written with the very body that was published, the files it came
    from are deleted, and VERSION moves to the next patch. Nothing else is touched - the commit
    this makes is release-notes/ and VERSION, which is what lets the release after it read the ci
    run of the commit below it (.github/workflows/release.yml, docs/release.md)."""
    notes_dir = notes_dir or NOTES_DIR
    repo = repo or REPO
    lines = unreleased(notes_dir, repo)
    text = body(notes_dir, repo, lines) if text is None else text
    if not text:
        raise Refused(NOTHING_TO_RELEASE)
    path = os.path.join(notes_dir, "%s.md" % tag_of(version))
    if os.path.exists(path):
        raise Refused("%s is already there: %s's notes were filed once and are not written twice."
                      % (os.path.relpath(path, repo), tag_of(version)))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    for name, _ in lines:
        os.remove(os.path.join(notes_dir, UNRELEASED, name))
    moved_to = next_patch(version)
    write_version(moved_to, repo)
    return {"notes": path, "took": [name for name, _ in lines], "version": moved_to}


# ------------------------------------------------------------------ the command line


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["version", "tag", "next", "lines", "body",
                                            "preflight", "publish", "ci-commit"])
    parser.add_argument("--repo", default=REPO, help="the repository root (default: this one)")
    parser.add_argument("--ref", default="", help="preflight: the ref the run was started on")
    parser.add_argument("--sha", default="HEAD", help="ci-commit: the commit being released")
    parser.add_argument("--out", help="body: write it here instead of to stdout")
    args = parser.parse_args(argv)
    notes_dir = os.path.join(args.repo, "release-notes")
    try:
        version = read_version(args.repo)
        if args.command == "version":
            print(version)
        elif args.command == "tag":
            print(tag_of(version))
        elif args.command == "next":
            print(next_patch(version))
        elif args.command == "lines":
            for name, text in unreleased(notes_dir, args.repo):
                print("%s: %s" % (name, text))
        elif args.command == "body":
            text = body(notes_dir, args.repo)
            if text is None:
                raise Refused(NOTHING_TO_RELEASE)
            if args.out:
                with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
                print("%s: the top of %s's body, %d line(s) from the tasks"
                      % (args.out, tag_of(version), len(unreleased(notes_dir, args.repo))))
            else:
                sys.stdout.write(text)
        elif args.command == "preflight":
            lines = unreleased(notes_dir, args.repo)
            preflight(version, args.ref, existing_tags(args.repo), lines)
            print("ok    %s may be released from %s: %d line(s) waiting"
                  % (tag_of(version), args.ref, len(lines)))
            for name, text in lines:
                print("      %s: %s" % (name, text))
        elif args.command == "ci-commit":
            sha = _git(args.repo, "rev-parse", args.sha) or args.sha
            commit, walked = ci_commit(args.repo, sha)
            print(commit)
            for passed in walked:
                sys.stderr.write("      %s changes only %s and %s: it has no ci run of its own\n"
                                 % (passed[:7], RELEASE_OWN_FILES[0], RELEASE_OWN_FILES[1]))
        elif args.command == "publish":
            done = publish(version, notes_dir, args.repo)
            print("ok    %s holds what was published (%d line(s): %s); %s is now %s"
                  % (os.path.relpath(done["notes"], args.repo), len(done["took"]),
                     ", ".join(done["took"]), VERSION_FILE, done["version"]))
    except Refused as refused:
        sys.stderr.write("REFUSED: %s\n" % refused)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
