# Phase 9 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–07 goes below them when it
is written.

## Found while doing 01 (7 November)

- [x] **[done by p9-05] The access list is asked for twice on a local job, and the local door is documented
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
  - **Looked at, and it needs no code.** `readAccessList` does ask with `retry`
    (`agent/JobRunner.cpp`), and `LinkMesh::localRequest` does answer `kReqTypeGetAccessList` by
    sending nothing, so a local `config.read` still spends **two** sends on a request this
    firmware knows cannot be answered. What changed is the price: with the exit-node firmware's
    task 40 in the copy, `JobRunner::spend` returns before the airtime gate for a job the door
    answers, so neither try waits for a slot, spends one or counts a transaction. The 20-to-40
    second wait the second try used to cost is gone, and what is left is two UART round trips of
    a few milliseconds each.
  - **The figure moved as predicted**: 35 transactions became **0**.
    `host_test/agent-backend/test_agent_watchdog.cpp` holds the new figure, and
    `test_agent_local_door.cpp` holds `local_silent` at exactly **2** - so a door that learns to
    answer the access list (link 1.2, task 06) is a visible change rather than a quiet one.
  - **Not changed, deliberately.** Suppressing the second try would mean the Walter deciding
    something about the library's retry policy from inside an `IMesh`, and the library's reading
    of the silence - *no ACL in this snapshot, no evidence either way* - is the reading that keeps
    a local `config.read` from reporting an empty access list as a fact. Task 06 is where the
    door gets a real answer for it (`LOCAL_REQUEST`).

- [x] **[done by p10-02] `wake` says how long each wake was, not what it did.** 7 November, the 5-minute wake
  test: the list showed three wakes of about 40 s and four of 6–7 s. They were read as wakes that
  skipped the modem, then as a failed connection. Both were wrong: `lte` said seven attempts and no
  failures. The journal keeps only wakes with news (`WakeCycle::journalWake`), so a quiet wake leaves
  nothing but its length. One letter per wake would have said it: modem up, backend reached, jobs
  run.
  - **Three letters per wake, and the legend above the list**
    (`tasks/phase-10/reports/02.md`): `M` the uplink came up, `S` the server answered, `J` a job
    ran, `-` for one that did not and `?` for a wake that wrote nothing down. They are three bits
    of each entry of the duty cycle's retained record (`core/cycle/wake_reason.h`, `kDidModem`,
    `kDidAnswered`, `kDidJobs`, `kDidSaid`), written the moment each becomes true — at step 6 and
    at step 8 — so a wake that is reset or crashes still leaves what it had managed. **The record's
    size did not change for them**: the two bytes came from the top half of a `lasted_s` no wake
    ever filled. `host_test/core/cycle/test_what_a_wake_did.cpp` holds each of the four readings,
    and the list still fits a 100-column terminal.
  - And the half of the item it did not name: *the backend reached* was the wrong word for it.
    The journal said *the backend was reached* for every exchange that did not fail, so the wakes
    of 8 November that sent nothing claimed it. The same task made that flag an answered heartbeat
    and nothing else (`interfaces::Reached`).

## Found while doing 02 (7 November)

- [ ] **[Thomas, on the bench] The 30-second stall is still one unanswered AT command.** Task 02
  fixed the two ways a socket id can go wrong (the review's S2 and S3), printed the rotation's
  counters so that the next stall says which id it was on, and told *the connection closed before
  the body was complete* apart from *the modem stopped answering for this socket*. It did **not**
  settle why `AT+SQNSRECV` went unanswered for the library's whole 30 s command timeout, twice on
  7 November. That needs the library's TX/RX trace: `docs/bench.md` §13 has the build, the four
  things the trace must show and the lines that get them, and report 02 has the three mechanisms
  the code allows. **A fix without that trace would be a guess**, which is why task 02 made none.

- [ ] **[later] The library drops socket events, and nothing of ours would know.** The event queue
  is `WALTER_MODEM_EVENT_QUEUE_MAX_ITEMS` = 8, filled with `xQueueSend(..., 0)` - a full queue
  drops the event silently - and the event task takes **one event per loop**, each loop a
  `vTaskDelay(10)` plus another 10 ms before the dispatch
  (`managed_components/dptechnics__walter-modem/src/WalterModem.cpp`,
  `_eventProcessingTask`). So a response that rings more than eight times inside ~160 ms loses
  events, and one of them can be the `+SQNSH` that a socket id is then owed for ever - which is
  what task 02 bounded rather than prevented. Two things could be read when there is a figure for
  how often the GM02SP rings: whether `AT+SQNSCFGEXT`'s ring mode 0 (a bare `+SQNSRING: <id>`,
  with no data amount) would make this firmware cheaper in events - it only uses the ring as a
  cue and polls anyway (`components/modem/walter_tls.cpp`, `receive`) - and whether a later
  library has a deeper queue or a blocking send. Neither is worth doing from a reading alone.

- [ ] **[later] The GM02SP's AT manual is still unread in prose.** The document the socket work
  rests on is QuickSpot's copy of the Monarch 2 LR 8.2 AT Commands Reference
  (`https://quickspot.io/docs/file/gm02s_at_commands.pdf`, 20 MB). Task 02 read its **examples and
  syntax lines** - six sockets (`+SQNSH: (1-6)`), `+SQNSRECV: (1-6),(1-1500)`, TLS per socket
  (`+SQNSSCFG:(1-6),(0-1),(1-6)`, and the query lists an enable flag and a profile for each of the
  six), and `+SQNSCFG: (1-6),(0-5),(0-1500),(0-65535),(0,10-1200),(0-255)`, which is the range
  every parameter `walter_tls.h` sets is inside - but **not its prose**: the pages use subset fonts
  with no extractable encoding, and the file is over the fetch tool's limit. So the two questions
  the task asked of the manual are still open: whether a TLS session (or a session cache) is tied
  to a socket id and inherited when the id is reused, and what `AT+SQNSD` answers on an id the
  modem still holds open or closing. A machine with a PDF reader, or Sequans' own portal, settles
  both in ten minutes.

## Found while doing 05 (8 November)

- [x] **[later] The wake still prices a local job as air.** `AgentLoop::wakeJobs` asks
  `transactionsOf(job)` and `estimateJobMs(job)` - `n x (minGapSeconds + loginMs)` - *before* it
  runs a claimed job, and reports it `failed`, *wake budget: about N s left of this wake and
  config.read needs about M s*, at no transaction when the wake's slice is smaller. A 34-line
  local `config.read` is priced at 34 x (20 s + 8 s) = **952 s** by that arithmetic, whatever the
  door does, because `spend` is a different gate from this one. It does not bite today: the
  exchange is given `kBackendBoundMs`, **1 200 s** (`core/cycle/wake_cycle.h`), and the
  transaction cap is sized to the dearest job at every band but `critical`
  (`agent::intent::kLargestJob`). But the margin is 248 s on the dearest job this unit really
  runs, so a shorter backend bound, a longer key list or a larger `minGapSeconds` from the server
  would refuse in seconds the very job task 05 made fast. The exit-node firmware's report
  p1.10-40 named this for the Walter's pile 9 and `AgentLoop` is in the hash-checked copy, so
  the fix is upstream's: price a job the mesh answers locally at the reply waits and not at the
  gaps. Named here with the arithmetic so that the next pile can take it upstream rather than
  rediscover it from a bench refusal.
  - **Taken upstream and closed by `tasks/phase-10/reports/01.md`.** The exit-node firmware's task
    p1.10-43 added `IMesh::answersLocallyFor(publicKeyHex)` — the local door asked for a *key*,
    before any job has begun — and `wakeJobs` now asks it once per job and reads the answer into
    both bounds. p10-01 synced that commit and overrode the virtual
    (`LinkMesh::answersLocallyFor`, `beginJob`'s own expression, so the two answers cannot drift
    apart). The 34-line local read is priced at **272 s** and not **952 s**, and three cases of
    `host_test/agent-backend/test_agent_local_door.cpp` hold the line it draws: the same job runs
    inside a 400 s wake through the door, is refused at 952 s for a neighbour, and is refused at
    272 s with *answered without the air* when the wake really is too small.

## Found while doing 07 (8 November)

- [x] **[p10-04] The Walter's own download does not resume, and the message says it does.** Writing
  `docs/flashing.md`'s LTE walk against the code turned up a difference between the two boxes that
  nothing says out loud. A radio image's download resumes: `Fetch::fetchInto` calls
  `target.beginWrite(expected)` and the store's `TransferTarget` answers `written() != 0` from its
  progress record, so the next wake continues the file (*fetch: resuming at N of M bytes already in
  flash*). The **Walter's own** slot does not: `SelfUpdate::beginWrite` sets `written_ = 0` and the
  platform's `beginWrite` erases the slot, and `abandonWrite()` - which every non-complete verdict
  calls - zeroes it again. So a `twinoak.fw.offer` of the Walter's product that the wake's budget
  cuts short starts the file again from zero at the next wake, and the plan pays for the bytes
  twice. Two things follow:
  - **The words are wrong in two places.** `core::update::describe(FetchVerdict::kBudget)` says
    *the wake's budget ran out; the download resumes next wake*, and `FirmwareJobs::runOffer`'s
    budget branch logs *no result, the lease is left to expire and the next wake continues it* and
    posts `reason: the wake's budget ended the download; the next wake resumes it`. Both are true
    of a radio image and false of the Walter's own, and the branch already knows which it is in
    (`walter`). One sentence each, and the server's job page stops promising a resume that will not
    happen. **Done in task p10-04**: `describe(FetchVerdict, bool own_image)` takes which box it is
    and `FetchResult::own_image` carries it (`runInto`'s target keeps no progress record), the
    offer's budget branch says *the next wake starts the file again from zero* and posts a `reason`
    that says the same, and four host tests hold the four sentences. `docs/flashing.md`,
    `field.md`, `cycle.md`, `ARCHITECTURE.md` §13 and `bench.md` §18 follow. One thing the
    reading turned up: a budget stop's result is **withheld** by `FirmwareJobs`' own result gate
    (unit-protocol.md §5's *no result, the lease expires*), so the corrected `reason` reaches the job's
    log and `out.data` and not meshmaster.dk's job page — the page's wording is the server's, and
    nothing here can promise on its behalf.
  - **Whether it should resume at all is a design question**, not a bug: the other application slot
    has no progress record of its own and ESP-IDF's `esp_ota_begin` erases what it opens, so a
    resume would need the journal to carry the bytes written and the slot to be reopened without an
    erase - `esp_ota_resume` exists in v5.5 for exactly that. It does not bite while the file fits
    one wake: 750 kB in twelve 64 kB ranges, against a megabyte and twenty minutes
    (`kWakeDownloadBytes`, `kBackendBoundMs`). `docs/bench.md` §18's new item is where the real
    figure comes from, and it is the figure that decides whether this is worth building.
    **Still open after p10-04**, which built no resume (its *Out of scope*): the figure is not on
    the bench yet — the 8 November offer over LTE failed in two seconds for a missing
    `X-Agent-Version` header (phase 10's task 05), so no download of the Walter's own image has
    ever run to completion or to a budget stop on a real plan. `docs/bench.md` §18 now asks for the
    seconds in so many words, and this item is the decision that waits on them.
