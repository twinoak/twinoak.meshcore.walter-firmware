# Phase 9 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–07 goes below them when it
is written.

- [ ] **[later] `wake` says how long each wake was, not what it did.** 7 November, the 5-minute wake
  test: the list showed three wakes of about 40 s and four of 6–7 s. They were read as wakes that
  skipped the modem, then as a failed connection. Both were wrong: `lte` said seven attempts and no
  failures. The journal keeps only wakes with news (`WakeCycle::journalWake`), so a quiet wake leaves
  nothing but its length. One letter per wake would have said it: modem up, backend reached, jobs
  run.
