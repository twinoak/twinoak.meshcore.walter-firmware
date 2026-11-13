# Phase 11 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–04 goes below them when it
is written.

- [x] **[01] Phase 10's *The library's escape buffer is 3 080 bytes of `.bss` in every build now*
  (report 06) is this pile's cause.** 01 settles it; tick it in `tasks/phase-10/FOLLOWUPS.md` too.
  Settled in [reports/01.md](reports/01.md), and **report 06 had the cause slightly wrong**: the
  3 080-byte `_logEscapeBuffer` is in v0.1.2's `.bss` as well (`0xc08` in both maps) - it is not
  guarded by `CONFIG_LOG_MAXIMUM_LEVEL` - so taking the option out gave back **680 bytes** of
  static DRAM and not 3 080. It was enough: 0.1.3 was 412 bytes short of starting and 0.1.4 has
  356 to spare. The cause of the pile is right; the number was the whole-image debug level's
  other costs, not that buffer.

## The review of 04 (12 November)

04 merged as #57 with ci green on `main` (#156), and the release workflow's dry run with the boot
gate in it passed on the task's branch (release #7). Nothing in the diff needs changing: the gate
is a step of the publish job between `build_release.py` and the signing, the secret is not in its
environment, and `tools/verify.sh` runs the same script in its release step.

- [ ] **[next task that touches the words] `v0.1.4` is on a unit.** meshmaster.dk's page of LTE
  Exit Node 1 (exit node 5) says *slot 0: committed (0.1.2 → 0.1.4)* for the update Thomas asked
  for on 10 November, 14 range requests for the download, and the heartbeat of 12 November still
  reports `0.1.4+62538fa`. README (~L58) and `docs/flashing.md` still say
  whether a unit has taken it is *not known*; `docs/bench.md` §18's *The Walter's own release over
  LTE, 0.1.2 to 0.1.3* is the item report 04 left for the reviewer and can now be rewritten as
  walked, with 9 November's rollback as the half that proved the rollback.
  **Done in p12-01** ([`tasks/phase-12/reports/01.md`](../phase-12/reports/01.md)). README's *not
  known* is gone and says what the server saw — the four offers, *stored* 14:52 UTC and *committed*
  15:20, the heartbeat since — and says in the same breath that no console was watched;
  `docs/flashing.md` has *What it did on 10 November* with the times, the three expired offers and
  what an expired offer costs; `docs/bench.md` §18's item is for **0.1.2 to 0.1.4** and splits what
  the field gave from what only a cable gives, with the download's seconds still open (the field
  bounds it at *at most 10 min 15 s*). *Still unverified on a board* in README no longer says a
  release of its own has never been kept; it says it has never been **timed**. The 14 requests and
  the one-second journal are answered in the report and in the two documents; one of them left a
  line in `tasks/phase-12/FOLLOWUPS.md`.
- [ ] **[later] The gate stops at the `heap:` line** (report 04). A longer window reads the
  descriptor, the partitions and the prompt for nothing; worth it when a second criterion is
  wanted.
- [ ] **[later] `printf` before a USB host is attached goes nowhere** (report 04): a person who
  plugs a cable in after a boot sees neither the banner nor the `heap:` line. One sentence in
  `docs/console.md`.
  **The sentence is written (p12-01)**, in *What the port prints besides replies*: the boot's first
  lines are `printf` through the VFS, they are dropped while no host is connected, and they are not
  in `dump` either (they predate the console's ring) — `status` prints the banner again and
  `reboot` prints the whole boot. The behaviour itself is untouched and this item stays open for
  whoever decides whether it should change.
- [ ] **[by hand] The driver could not delete `origin/task/p11-03-a-rollback-says-what-killed-it`
  or `origin/task/p11-04-a-release-starts-first`.**
