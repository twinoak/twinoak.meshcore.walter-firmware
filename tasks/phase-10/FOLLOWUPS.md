# Phase 10 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–07 goes below them when it
is written.

## Found on the bench (8 November, Walter and radio at 0.1.2)

- [ ] **[Thomas, the trace build] The 30-second stall is reproducible, and it is the answer that is
  lost, not the request.** In every `agent wake` with a backlog of adverts, an
  `/agent/v1/observations` POST hangs for the library's whole 30 s (`WalterModem: Command time-out
  (TX)`, then *the connection closed before the body was complete*). It happened after the 5th, 6th
  and 7th connection of a wake, so it is not a count. At 14:32 UTC the first batch of 20 went through
  (*http 200, 3665 ms*) and the second one stalled. And the server answered *20 posted, 0 accepted
  (the server had this batch already)*: an earlier "failed" POST had **reached the server and been
  stored**, so the request went out and its answer never came back. The small POSTs (heartbeat,
  claim, results, about 1 s at the server) have not stalled once in two days; the observations
  (about 3.7 s at the server, several kB) are the only ones that do. `lte` after one: ids 1 and 6
  turn and turn about (`1 6 1 6 1`), 0 owed closes waited for, timed out or written off, 2
  discounted. This makes pile 9's trace build (`docs/bench.md` §13) a five-minute job: flash it,
  `lte up`, `agent wake` with adverts waiting.
  - **Three more wakes (14:35–14:37 UTC) made it exact.** Each wake's *first* observations POST is
    the resend of the batch whose answer was lost, and the server answers *0 accepted, had it
    already* in 1.4–1.5 s. The *second* carries a new batch, and it stalls every time. The backlog
    went 64 → 44 → 26 → 6, so the server stores what it was sent. **The stall follows the server
    doing real work on a batch.** The first thing to read is meshmaster.dk's own log for those
    POSTs (14:35:53 and 14:37:09 UTC): the status it answered and how long it took. If it took
    tens of seconds, the fix is the server's. If it answered in a few, the modem lost the answer,
    and the trace build is next.
  - **meshmaster.dk's log answered it: the server is not slow.** Every one of the Walter's
    observations POSTs on 8 November got `200` from the server in under a second, the stalled ones
    included: 09:09:05 (0.87 s), 13:54:18 (0.94 s), 14:29:51 (0.87 s), 14:32:40 (0.95 s), 14:35:55
    (0.73 s), 14:37:10 (0.39 s). The one that *was* received at 14:32:37 took the longest (2.9 s).
    So the answer was written and lost between the server and this firmware: the modem, or our
    receive. One pattern to put in front of the trace: the log's response size. The three answers the
    Walter received were **876** bytes (*0 accepted, had it already*), and all six it lost were
    **877 or 878**. The Heltec exit nodes receive 877-byte answers of the same route without
    trouble, over WiFi. It may be chance, but it is the first thing to look for in the trace: the
    `+SQNSRING`/`+SQNSRECV` lengths of an 876-byte answer against an 878-byte one.

## From the reports of 01–07 (placed by the reviewer, 9 November)

Each is written out in full in the report it names, under *For `FOLLOWUPS.md`*.

- [ ] **[Thomas, both sides] A fifth apply outcome for a node fault** (report 03). An apply onto a
  radio that refuses its identity reports `stuck`, with `node-fault` in the journal. The server has
  four outcome words and none fits; a word of its own would let the page say *a person is needed at
  the radio*. Also in meshmaster.dk's `tasks/phase-4.28/FOLLOWUPS.md`.
- [ ] **[later] `rak status` could say the attention too** (report 03): nothing fills
  `IdentityReading::attention_identity_lost` yet.
- [ ] **[later] `spec/proposals/02`'s *downloads must resume* is half true** (report 04): true of a
  radio image, not of the Walter's own. Decide with the bench's download figure.
- [ ] **[later] A budget-stopped offer's `reason` is written and never sent** (report 04): the result
  gate withholds it on purpose, so the corrected sentence reaches the job log and not the server's
  page.
- [ ] **[later] An agent call sends two `Accept` headers** (report 05).
- [x] **[later] The library's escape buffer is 3 080 bytes of `.bss` in every build now** (report 06).
  Settled by task p11-01 (`tasks/phase-11/reports/01.md`): the buffer is in *every* build and
  always was - it is not guarded by `CONFIG_LOG_MAXIMUM_LEVEL`, and v0.1.2's map has the same
  `0xc08`. What the option really cost in static DRAM was 680 bytes, and that was enough to stop
  v0.1.3 starting; it is out of `sdkconfig.defaults` again and the trace is a build of its own.
- [ ] **[later] Once the trace has been on, every log line of that boot goes through the hook**
  (report 06).
- [ ] **[later] A `+SQNSRECV` payload longer than 4 096 bytes would be cut in the trace** (report 06).
