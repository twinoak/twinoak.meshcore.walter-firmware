#!/usr/bin/env python3
"""tools/release/release_notes.py's tests: the version, the lines, the refusals and the move.

    python tools/release/test_release_notes.py        # or -v

Task p12-02. The button publishes now, so the two things a person used to do by hand at tagging
time - decide the version and write the body - are files in the repository, and these are the
tests of the code that reads them. What is held here is what a release must not do:

  * read the version from anywhere but VERSION, or accept one that is not X.Y.Z;
  * publish the lines in an order that is not the order the tasks merged;
  * publish a line that cannot be read: empty, a page long, a heading, two paragraphs;
  * release with nothing to say; release the version that was already released; release from
    anything but main - each refused before a byte is built, with its own sentence;
  * leave `unreleased/` behind after a release, or leave VERSION where it was - the two that
    would make the next release publish the same lines again under the same tag.

The merge order and the "already released" refusal are about git, so those tests build a scratch
repository with real commits and real tags in it. Everything else works on a directory.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import release_notes
from release_notes import Refused

A_LINE = "The console says why a trial failed. Seen in QEMU, not yet on a board."


class Scratch(unittest.TestCase):
    """A repository of this test's own: VERSION, release-notes/ and, where a test asks for it,
    git with commits in it. Nothing here reads or writes the real repository."""

    git = False

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="twinoak-notes-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.notes = os.path.join(self.dir, "release-notes")
        os.makedirs(os.path.join(self.notes, release_notes.UNRELEASED))
        self.version("0.1.5")
        if self.git:
            self.run_git("init", "-q", "-b", "main")
            self.run_git("config", "user.email", "test@example.invalid")
            self.run_git("config", "user.name", "a test")
            self.commit("the repository")

    # -- the scratch repository's own small language

    def version(self, text):
        with open(os.path.join(self.dir, "VERSION"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")

    def line(self, name, text=A_LINE):
        path = os.path.join(self.notes, release_notes.UNRELEASED, name)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")
        return path

    def standing(self, text):
        with open(os.path.join(self.notes, release_notes.STANDING), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write(text + "\n")

    def run_git(self, *args):
        done = subprocess.run(["git", "-C", self.dir] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, universal_newlines=True)
        self.assertEqual(done.returncode, 0, done.stdout)
        return done.stdout.strip()

    def commit(self, message, when=None):
        """One commit, at a time this test chooses: the committer date is what the merge order is
        read from, so a test that is about the order has to be able to say what it is."""
        env = {}
        if when:
            env = {"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when}
        self.run_git("add", "-A")
        old = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        try:
            self.run_git("commit", "-q", "--allow-empty", "-m", message)
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def lines(self):
        return release_notes.unreleased(self.notes, self.dir)

    def body(self):
        return release_notes.body(self.notes, self.dir)


# ------------------------------------------------------------------ the version, in one place


class TheVersion(Scratch):
    def test_it_is_read_from_the_file_and_nowhere_else(self):
        self.assertEqual(release_notes.read_version(self.dir), "0.1.5")
        self.assertEqual(release_notes.tag_of(release_notes.read_version(self.dir)), "v0.1.5")

    def test_a_version_that_is_not_x_y_z_is_refused(self):
        for bad in ("v0.1.5", "0.1", "0.1.5-rc1", "0.1.5+abc1234", "", "the next one"):
            self.version(bad)
            with self.assertRaises(Refused) as caught:
                release_notes.read_version(self.dir)
            self.assertIn("VERSION", str(caught.exception))

    def test_a_missing_file_says_what_it_is_for(self):
        os.remove(os.path.join(self.dir, "VERSION"))
        with self.assertRaises(Refused) as caught:
            release_notes.read_version(self.dir)
        self.assertIn("next version", str(caught.exception))

    def test_the_next_release_is_the_next_patch(self):
        self.assertEqual(release_notes.next_patch("0.1.5"), "0.1.6")
        self.assertEqual(release_notes.next_patch("0.1.9"), "0.1.10")
        self.assertEqual(release_notes.next_patch("1.0.0"), "1.0.1")

    def test_the_repositorys_own_version_is_a_version_and_is_ahead_of_every_tag_filed(self):
        """The real files. VERSION names the release that comes next, so a published note of that
        name would mean the version was not moved on - which is the refusal below, found here at
        every ci run instead of at the button."""
        version = release_notes.read_version()
        filed = os.path.join(release_notes.NOTES_DIR, "%s.md" % release_notes.tag_of(version))
        self.assertFalse(os.path.exists(filed),
                         "%s is already filed: VERSION must name the release that comes next"
                         % filed)


# ------------------------------------------------------------------ what a line is


class ALine(Scratch):
    def test_the_lines_make_the_top_of_the_body(self):
        self.line("p12-01-a-task.md", "The unit says why a trial failed. Not on a board yet.")
        self.standing("**Goes with** the repeater firmware v0.1.2.")
        self.assertEqual(self.body(),
                         "## What is new\n"
                         "\n"
                         "- The unit says why a trial failed. Not on a board yet.\n"
                         "\n"
                         "**Goes with** the repeater firmware v0.1.2.\n")

    def test_with_no_line_there_is_no_body_at_all(self):
        self.standing("**Goes with** the repeater firmware v0.1.2.")
        self.assertEqual(self.lines(), [])
        self.assertIsNone(self.body())

    def test_a_line_written_over_two_lines_is_published_as_one(self):
        self.line("p12-01-a-task.md", "A sentence that was wrapped\nin the file it was written in.")
        self.assertIn("- A sentence that was wrapped in the file it was written in.", self.body())

    def test_a_dotfile_is_not_a_line(self):
        """release-notes/unreleased/.gitkeep is what keeps the directory in git, and it is not a
        release note - nor is any other file a tool leaves behind."""
        with open(os.path.join(self.notes, release_notes.UNRELEASED, ".gitkeep"), "w") as fh:
            fh.write("")
        self.assertEqual(self.lines(), [])

    def test_an_empty_file_is_refused(self):
        self.line("p12-01-a-task.md", "   ")
        with self.assertRaises(Refused) as caught:
            self.lines()
        self.assertIn("is empty", str(caught.exception))

    def test_a_heading_is_refused(self):
        self.line("p12-01-a-task.md", "## What is new\n\nSomething.")
        with self.assertRaises(Refused) as caught:
            self.lines()
        self.assertIn("begins with a heading", str(caught.exception))

    def test_two_paragraphs_are_refused(self):
        self.line("p12-01-a-task.md", "The first thing.\n\nThe second thing.")
        with self.assertRaises(Refused) as caught:
            self.lines()
        self.assertIn("more than one paragraph", str(caught.exception))

    def test_a_page_is_refused_and_says_how_long_it_may_be(self):
        self.line("p12-01-a-task.md", "word " * 200)
        with self.assertRaises(Refused) as caught:
            self.lines()
        message = str(caught.exception)
        self.assertIn(str(release_notes.MAX_CHARS), message)
        self.assertIn("p12-01-a-task.md", message)

    def test_a_file_with_carriage_returns_is_refused(self):
        path = self.line("p12-01-a-task.md")
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("A line." + chr(13) + chr(10))
        with self.assertRaises(Refused) as caught:
            self.lines()
        self.assertIn("carriage returns", str(caught.exception))

    def test_the_repositorys_own_waiting_lines_can_be_published(self):
        """The real directory, read the way the release reads it: a line that could not be
        published is a red ci run and not a refused release."""
        for name, text in release_notes.unreleased():
            self.assertTrue(text, name)
            self.assertLessEqual(len(text), release_notes.MAX_CHARS, name)


# ------------------------------------------------------------------ the order they merged in


class TheOrder(Scratch):
    git = True

    def test_the_lines_come_out_in_the_order_the_tasks_merged(self):
        """Not alphabetical, and not what the file system answers: the commit that added each
        file is what says when its task arrived."""
        self.line("p12-09-the-last-task.md", "The last task's line.")
        self.commit("the last task", when="2026-11-10T09:00:00 +0000")
        self.line("p12-01-the-first-task.md", "The first task's line.")
        self.commit("the first task", when="2026-11-11T09:00:00 +0000")
        self.assertEqual([name for name, _ in self.lines()],
                         ["p12-09-the-last-task.md", "p12-01-the-first-task.md"])
        self.assertIn("- The last task's line.\n- The first task's line.", self.body())

    def test_a_line_not_committed_yet_is_the_newest(self):
        """The line a task is writing right now - what a dry run from a task's own branch shows -
        goes where it will be when it lands: last."""
        self.line("p12-01-the-first-task.md", "The first task's line.")
        self.commit("the first task", when="2026-11-11T09:00:00 +0000")
        self.line("p12-02-not-committed.md", "The line being written now.")
        self.assertEqual([name for name, _ in self.lines()],
                         ["p12-01-the-first-task.md", "p12-02-not-committed.md"])

    def test_two_lines_of_one_commit_are_ordered_by_name(self):
        self.line("p12-02-second.md", "The second.")
        self.line("p12-01-first.md", "The first.")
        self.commit("two tasks at once", when="2026-11-11T09:00:00 +0000")
        self.assertEqual([name for name, _ in self.lines()],
                         ["p12-01-first.md", "p12-02-second.md"])

    def test_outside_a_repository_the_names_decide(self):
        """A tarball, or a scratch directory: there is nothing to read an order from, so the
        files are taken in name order rather than in whatever order the file system gives."""
        loose = tempfile.mkdtemp(prefix="twinoak-notes-loose-")
        self.addCleanup(shutil.rmtree, loose, ignore_errors=True)
        os.makedirs(os.path.join(loose, "release-notes", release_notes.UNRELEASED))
        for name in ("b.md", "a.md"):
            with open(os.path.join(loose, "release-notes", release_notes.UNRELEASED, name),
                      "w", encoding="utf-8", newline="\n") as fh:
                fh.write("A line for %s.\n" % name)
        got = release_notes.unreleased(os.path.join(loose, "release-notes"), loose)
        self.assertEqual([name for name, _ in got], ["a.md", "b.md"])


# ------------------------------------------------------------- whose ci run speaks for a commit


class TheCiRun(Scratch):
    """Deliverable 7 of the task: the commit the release's own pull request makes has no ci run
    of its own - GitHub starts no workflow from an event raised with the job's token - so the
    release after it waits for the run of the commit below."""

    git = True

    def head(self):
        return self.run_git("rev-parse", "HEAD")

    def code(self, text):
        with open(os.path.join(self.dir, "main.c"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

    def test_a_commit_that_changed_code_speaks_for_itself(self):
        self.code("int main(void) { return 0; }\n")
        self.commit("a task")
        self.assertEqual(release_notes.ci_commit(self.dir, self.head()), (self.head(), []))

    def test_the_run_of_the_commit_below_a_release_notes_commit_is_taken(self):
        self.code("int main(void) { return 0; }\n")
        self.commit("a task")
        code = self.head()
        self.line("p12-01-a-task.md")
        release_notes.publish("0.1.5", self.notes, self.dir)
        self.commit("Release notes: v0.1.5")
        notes_commit = self.head()
        commit, walked = release_notes.ci_commit(self.dir, notes_commit)
        self.assertEqual(commit, code)
        self.assertEqual(walked, [notes_commit])

    def test_a_commit_that_touched_anything_else_as_well_stops_the_walk(self):
        """One byte of firmware in the same commit and it is a commit like any other: it is
        waited for, and a release of it is refused until its own ci run is green."""
        self.code("int main(void) { return 0; }\n")
        self.commit("a task")
        self.line("p12-01-a-task.md")
        self.code("int main(void) { return 1; }\n")
        self.commit("notes and a change")
        self.assertEqual(release_notes.ci_commit(self.dir, self.head()), (self.head(), []))

    def test_two_release_notes_commits_in_a_row_are_both_walked_past(self):
        self.code("int main(void) { return 0; }\n")
        self.commit("a task")
        code = self.head()
        self.line("p12-01-a-task.md")
        release_notes.publish("0.1.5", self.notes, self.dir)
        self.commit("Release notes: v0.1.5")
        self.line("p12-02-another-task.md")
        release_notes.publish("0.1.6", self.notes, self.dir)
        self.commit("Release notes: v0.1.6")
        commit, walked = release_notes.ci_commit(self.dir, self.head())
        self.assertEqual(commit, code)
        self.assertEqual(len(walked), 2)


# ------------------------------------------------------------------ the three refusals


class TheRefusals(Scratch):
    git = True

    def test_a_release_with_nothing_to_say_is_refused(self):
        with self.assertRaises(Refused) as caught:
            release_notes.preflight("0.1.5", "refs/heads/main", set(), self.lines())
        message = str(caught.exception)
        self.assertIn("there is nothing to release", message)
        self.assertIn("release-notes/unreleased/", message)

    def test_the_version_that_was_already_released_is_refused_and_says_how_to_finish(self):
        self.line("p12-01-a-task.md")
        self.commit("a task")
        self.run_git("tag", "v0.1.5")
        with self.assertRaises(Refused) as caught:
            release_notes.preflight("0.1.5", "refs/heads/main",
                                    release_notes.existing_tags(self.dir), self.lines())
        message = str(caught.exception)
        self.assertIn("v0.1.5 already exists", message)
        self.assertIn("VERSION", message)
        self.assertIn("merge it", message)

    def test_a_release_from_anything_but_main_is_refused(self):
        self.line("p12-01-a-task.md")
        for ref in ("refs/heads/task/p12-02-a-release-from-the-button", "refs/tags/v0.1.5", ""):
            with self.assertRaises(Refused) as caught:
                release_notes.preflight("0.1.5", ref, set(), self.lines())
            self.assertIn("main's head", str(caught.exception))

    def test_main_with_a_line_waiting_and_no_such_tag_passes(self):
        self.line("p12-01-a-task.md")
        self.commit("a task")
        release_notes.preflight("0.1.5", "refs/heads/main",
                                release_notes.existing_tags(self.dir), self.lines())

    def test_the_bare_branch_name_is_main_too(self):
        """GITHUB_REF is refs/heads/main and a person typing the command says main; both are the
        same answer, and neither is a branch whose name merely ends in main."""
        self.line("p12-01-a-task.md")
        release_notes.preflight("0.1.5", "main", set(), self.lines())
        with self.assertRaises(Refused):
            release_notes.preflight("0.1.5", "refs/heads/not-main", set(), self.lines())


# ------------------------------------------------------------------ the move, after a release


class TheMove(Scratch):
    git = True

    def test_the_lines_are_filed_the_version_moves_on_and_unreleased_is_empty(self):
        self.line("p12-01-the-first-task.md", "The first task's line.")
        self.commit("the first task", when="2026-11-10T09:00:00 +0000")
        self.line("p12-02-the-second-task.md", "The second task's line.")
        self.commit("the second task", when="2026-11-11T09:00:00 +0000")
        self.standing("**Goes with** the repeater firmware v0.1.2.")
        published = self.body()

        done = release_notes.publish("0.1.5", self.notes, self.dir)

        with open(os.path.join(self.notes, "v0.1.5.md"), encoding="utf-8") as fh:
            filed = fh.read()
        self.assertEqual(filed, published)
        self.assertIn("- The first task's line.\n- The second task's line.", filed)
        self.assertEqual(os.listdir(os.path.join(self.notes, release_notes.UNRELEASED)), [])
        self.assertEqual(release_notes.read_version(self.dir), "0.1.6")
        self.assertEqual(done["version"], "0.1.6")
        self.assertEqual(done["took"], ["p12-01-the-first-task.md", "p12-02-the-second-task.md"])

    def test_it_touches_the_releases_own_files_and_nothing_else(self):
        """The commit this makes is read by the release after it, which takes the ci run of the
        commit below it: that is only safe because nothing here can change the firmware."""
        with open(os.path.join(self.dir, "main.c"), "w", encoding="utf-8") as fh:
            fh.write("int main(void) { return 0; }\n")
        self.line("p12-01-a-task.md")
        self.commit("a task and some code")
        release_notes.publish("0.1.5", self.notes, self.dir)
        changed = sorted(line.split()[-1] for line in
                         self.run_git("status", "--porcelain").splitlines())
        self.assertEqual(changed, ["VERSION", "release-notes/unreleased/p12-01-a-task.md",
                                   "release-notes/v0.1.5.md"])

    def test_the_standing_part_stays_where_it_is(self):
        self.line("p12-01-a-task.md")
        self.standing("**Goes with** the repeater firmware v0.1.2.")
        release_notes.publish("0.1.5", self.notes, self.dir)
        self.assertTrue(os.path.isfile(os.path.join(self.notes, release_notes.STANDING)))
        self.assertIn("v0.1.2", release_notes.standing(self.notes))

    def test_a_release_already_filed_is_not_filed_twice(self):
        self.line("p12-01-a-task.md")
        release_notes.publish("0.1.5", self.notes, self.dir)
        self.line("p12-02-another-task.md")
        with self.assertRaises(Refused) as caught:
            release_notes.publish("0.1.5", self.notes, self.dir)
        self.assertIn("v0.1.5", str(caught.exception))

    def test_with_nothing_waiting_there_is_nothing_to_file(self):
        with self.assertRaises(Refused) as caught:
            release_notes.publish("0.1.5", self.notes, self.dir)
        self.assertIn("there is nothing to release", str(caught.exception))

    def test_the_next_release_of_the_same_repository_refuses_until_the_version_moved(self):
        """The whole round: release 0.1.5, do not move the version (the pull request was left
        open), and the next press of the button is refused rather than publishing v0.1.5 twice."""
        self.line("p12-01-a-task.md")
        self.commit("a task")
        text = self.body()
        with open(os.path.join(self.notes, "v0.1.5.md"), "w", encoding="utf-8") as fh:
            fh.write(text)
        self.run_git("tag", "v0.1.5")
        self.line("p12-02-another-task.md", "Another thing a person will notice. Not on a board.")
        with self.assertRaises(Refused) as caught:
            release_notes.preflight("0.1.5", "refs/heads/main",
                                    release_notes.existing_tags(self.dir), self.lines())
        self.assertIn("the version was not moved on", str(caught.exception))


# ------------------------------------------------------------------ the command line


class TheCommandLine(Scratch):
    git = True

    def main(self, *args):
        return release_notes.main(list(args) + ["--repo", self.dir])

    def test_the_commands_answer(self):
        self.line("p12-01-a-task.md")
        self.assertEqual(self.main("version"), 0)
        self.assertEqual(self.main("tag"), 0)
        self.assertEqual(self.main("next"), 0)
        self.assertEqual(self.main("lines"), 0)
        self.assertEqual(self.main("body"), 0)
        self.assertEqual(self.main("preflight", "--ref", "refs/heads/main"), 0)

    def test_a_refusal_is_exit_1(self):
        self.assertEqual(self.main("preflight", "--ref", "refs/heads/main"), 1)
        self.line("p12-01-a-task.md")
        self.assertEqual(self.main("preflight", "--ref", "refs/heads/a-branch"), 1)

    def test_body_writes_the_file_the_workflow_reads(self):
        self.line("p12-01-a-task.md")
        out = os.path.join(self.dir, "top.md")
        self.assertEqual(self.main("body", "--out", out), 0)
        with open(out, encoding="utf-8") as fh:
            self.assertIn("## What is new", fh.read())

    def test_publish_moves_everything_the_pull_request_carries(self):
        self.line("p12-01-a-task.md")
        self.assertEqual(self.main("publish"), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.notes, "v0.1.5.md")))
        self.assertEqual(release_notes.read_version(self.dir), "0.1.6")


if __name__ == "__main__":
    unittest.main(verbosity=2)
