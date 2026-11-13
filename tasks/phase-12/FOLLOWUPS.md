# Phase 12 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01 goes below them when it
is written.

- [ ] **[later] A download's retries are counted and then dropped, so only the plan's bill says a
  range was fetched twice.** Found by 01, from 10 November's offer 24932: the server billed 877 kB
  for a 780 528-byte file in **14 requests** where twelve 64 kB ranges cover it, and neither the
  result (`bytes: 780528`, `resumed: false`) nor the agent's four log lines mentions the two extra.
  The repeat itself is right — `core/net/download.cpp` advances `offset` only by what the sink
  actually took, so a range whose bytes never reached flash is asked for again, and a server that
  already wrote the body counts what it wrote. The gap is upward: `net::DownloadResult` carries
  `ranges`, `retries` and `restarts`, `core/update/fetch.cpp` prints all three in its `fetch:`
  summary line and then **keeps none of them** — `upd::FetchResult` has no field for them — so
  `FirmwareJobs::runOffer` could not put them in the offer's result or its log if it wanted to.
  `resumed` is a different question (whether an *earlier wake* left bytes in flash; for the
  Walter's own slot it is false by construction, `SelfUpdate::beginWrite`). Three fields on
  `FetchResult` and one sentence in the offer's log line would make a unit's own account agree with
  the bill, which matters because the bill is a data plan. Not fixed in 01: no code changes there.
- [ ] **[later] agent-core is behind.** The exit-node firmware's pile 1.10 (tasks 45–50) changes the
  library: owner info and a guest answer a job can use, a late reply that is no longer read as a
  refused login, `setperm` for the unit's own key checked by a login, `acl: 0`. This repository
  takes agent-core at a release tag (`tasks/sync/`), so it arrives with the exit-node firmware's
  next release. Until then meshmaster.dk's pile 4.32 is what protects this unit from the first of
  them.
