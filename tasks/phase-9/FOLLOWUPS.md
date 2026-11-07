# Phase 9 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–07 goes below them when it
is written.

## Found while doing 01 (7 November)

- [ ] **[05] The access list is asked for twice on a local job, and the local door is documented
  never to answer it.** `LinkMesh::localRequest` answers `kReqTypeGetAccessList` by *sending*
  nothing and letting the silence stand — on purpose (its comment: the library's own rule is that
  the ACL's silence is *no ACL in this snapshot, no evidence either way*). But agent-core's
  `readAccessList` asks with `retry`, so a local `config.read` spends **two** transactions on a
  request this firmware knows cannot be answered: 35 for a read whose shape is 34 lines, measured
  in `host_test/agent-backend/test_agent_watchdog.cpp`. At the unit's budget that second try is a
  20 to 40 second wait for nothing. Task 05's exit-node task 40 — a local job neither waits for nor
  spends the budget — makes the *cost* nil, so this may need no code at all; what it needs is a
  look, because the retry is still a request the door is known to swallow, and the figure 35 in
  that test is what will change if 40 alters the pacing.

- [ ] **[later] `wake` says how long each wake was, not what it did.** 7 November, the 5-minute wake
  test: the list showed three wakes of about 40 s and four of 6–7 s. They were read as wakes that
  skipped the modem, then as a failed connection. Both were wrong: `lte` said seven attempts and no
  failures. The journal keeps only wakes with news (`WakeCycle::journalWake`), so a quiet wake leaves
  nothing but its length. One letter per wake would have said it: modem up, backend reached, jobs
  run.
