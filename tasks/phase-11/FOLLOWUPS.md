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
