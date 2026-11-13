# Phase 12 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01 goes below them when it
is written.

- [ ] **[later] agent-core is behind.** The exit-node firmware's pile 1.10 (tasks 45–50) changes the
  library: owner info and a guest answer a job can use, a late reply that is no longer read as a
  refused login, `setperm` for the unit's own key checked by a login, `acl: 0`. This repository
  takes agent-core at a release tag (`tasks/sync/`), so it arrives with the exit-node firmware's
  next release. Until then meshmaster.dk's pile 4.32 is what protects this unit from the first of
  them.
