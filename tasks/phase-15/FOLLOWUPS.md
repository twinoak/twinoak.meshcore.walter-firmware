# Phase 15 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas, the bench] The cause of the lost answer is still not known.** Tasks 02 and 03
  make it readable and soften it. What makes one modem command go unanswered for 30 seconds
  after an observations post, and almost never after another call, is for the trace build to
  show (`docs/bench.md` §13, the ★ line; `tasks/phase-10/FOLLOWUPS.md`, first item). On
  8 November the three answers this board received were 876 bytes and the six it lost were 877
  or 878. On 16 November about one new batch in five was received during the morning, so that
  figure alone does not decide it.
  **Task 07** (17 November) gives the record something to be read against: `agent calls` and the
  report now print the latest call of the boot that **was** answered under the newest that failed,
  with every receive's two times in both blocks and, for the answered one, what the answer said of
  itself. Nothing of the modem changed and the cause is still not known; what changed is that the
  question can be asked from a report. `docs/bench.md` §13's new item lists the differences to read
  between the two blocks and what each of them would mean. **The box stays unticked.**
- [ ] **[later] The failure the newest record shows is the one shape task 03's rule leaves alone.**
  Task 03 asks the modem once more after a receive that failed **with the close seen**, which is the
  shape task 02's task file and phase 10's follow-up describe (*the connection closed before the
  body was complete*, 8 November). The record quoted in `tasks/phase-15/reports/02.md`, from wake
  1214 on 16 November, ends as **the modem stopped answering for this socket** — no close seen at
  the moment the command failed, although the record shows the close handed over at +514 ms — so on
  that wake nothing would have been asked again. A close that the rotation discounts as an older
  connection's debt looks exactly like that from `receive()`, and `lte` prints the discount count.
  **Read `agent calls`'s own `asked again` line against the list first** (`docs/bench.md` §13): N
  of 0 after failed wakes says the unit's failures are all of this second shape, and then the
  question is whether a discounted close should still count as *the close was seen* for this one
  decision — which is a change to `core/net/socket_rotation.h`'s accounting and not to be made
  without the figures.
- [ ] **[later] The firmware asks the modem for bytes every 50 ms and is answered `+CME ERROR: 4`
  until the first byte is there** (`components/modem/walter_tls.cpp`, `receive`). Whether a
  command sent in the instant the answer arrives is the one that hangs is the first thing to
  read in the trace. Not changed until it is known.
- [ ] **[later] A failing observations call costs the wake 30 seconds.** It is the library's
  command timeout and one value for every command. A shorter one for a receive is the library's
  to offer.
- [ ] **[later] meshmaster.dk holds the adverts this unit posted twice and more.** Its *Heard*
  counts for this unit are too high for the days before task 04 is on it. Retention removes the
  rows in its own time.
- [ ] **[later] Phase 14's and phase 13's follow-ups are still open.**

## From the review of 01 to 03 (16 November)

- [ ] **[reviewer] The block at the top of report 02 is not a reading from the unit.** It says
  *On the unit in service that reads:* and prints a record: wake 1214, 878 bytes announced at
  +102 ms, the close at +514 ms, a receive failed at +30150 ms. That firmware has been on no
  unit: the block is the example `docs/bench.md` §13 prints, and its epoch is a day in October.
  Report 03 argues from it twice as *the record from the unit*. Nothing in the code depends on
  it, and the rule of 03 is the one the unit's real list calls for (every entry there is *the
  connection closed before the body was complete*, a failed receive with the close seen). But it
  is not evidence of anything: the first real record is the one read after this release is on
  the unit, and nothing about the lost answer is to be concluded before then. `docs/bench.md`
  should say *for example* over that block.
- [ ] **[later] One more ask can cost a wake 30 seconds more** (report 03, its second deviation).
  The ask is not held to the call's deadline, because the command that failed has always overrun
  it. If the second ask hangs too, the call takes 60 seconds. To settle from the unit's count of
  answers read again: if it stays 0 over a day of failed calls, the ask buys nothing and goes.

## From the first night of 0.1.8 (17 November)

- [ ] **[reviewer] The second ask of task 03 answered nothing in 35 tries**, and the first timer
  wake that made it was 68 seconds awake. Task 05 takes it out. The item above it (*One more ask
  can cost a wake 30 seconds more*) is settled by that.
  **Done in task 05** (17 November): the ask, the two totals, their key and the rule's file are
  out of the firmware; a receive that failed with the close seen ends the call *closed* after one
  command, as it did before task 03. What stays is the record of task 02, which still prints the
  *asked again* moment of a record v0.1.8 wrote - the only reading anybody has of what the modem
  answered that ask. `agent_reread` is erased from the store at the first boot of the next
  release. Whether the wakes are back to about 40 s is `docs/bench.md` §13's new line.
- [ ] **[reviewer] Two real records, and one idea withdrawn.** Wake 254: receive sent at +310 ms,
  rings handed over at +320 (815 bytes) and +339 (5 bytes), the close at +359. Wake 289: receive
  sent at +397 ms, rings at +397 and +416, the close at +436. The first looked like a blind ask
  meeting the answer; the second is the read the ring itself set off. So *ask only after a ring*
  is not the cure, and nothing is to be changed in how the modem is asked until task 07 has put
  a call that worked beside these.
- [ ] **[later] Two wakes were ended by the bound in that night** (01:29 and 02:05 UTC, each
  holding meshmaster.dk job 32763, `status`). Cause not known; task 06 is how the next one is
  read. No job of this unit had expired unanswered in the three nights before.
- [ ] **[later] A call that worked, of the kind that fails.** Task 07 prints the calls a report
  can see, which are the ones before it in the wake. An answered observations call is after it
  and is not kept: keeping it would be a write to flash at every good wake.

## The review of piles 12 to 15 (19 November)

A lighter review than the piles of this week got, and it says so: these four have been in
service since 0.1.9 (18 November, on the unit at TwinOak Bellevue), so they are held here to
their reports and to what the unit has shown, not read line by line. Read: every report's
deviations and open criteria of 12/01–02, 13/01–02, 14/01–06 and 15/01–07; `firmware_jobs.cpp`
where a lease is extended (~L307–318, ~L915–935); the waiting fixtures against meshmaster.dk's
master at 28daabd. Nothing found that asks for a task.

- [ ] **[later] `FirmwareApplyProgress.json` is one name for two things** (pile 14, task 04).
  The file waiting in `host_test/fixtures/contracts-ahead/` is the whole post,
  `{"data":{"journal":[…]},"note":"flashing-candidate"}`; meshmaster.dk's file of that name is
  the `data` member alone, because its fixtures are one a record and the post's record is
  `ProgressReport`. The wire is the same: the file here is byte for byte
  `{"data":` + theirs + `,"note":"flashing-candidate"}` (checked). Neither side is wrong. When
  the waiting fixtures are reconciled, the test here makes the post from the copied
  `FirmwareApplyProgress.json` and does not keep a second file under that name.
- [ ] **[Thomas] 15/04 has not run**: it waits for a release of the exit-node firmware's
  library, which is now the release after that repository's pile 1.16. The bump then brings
  the batch id kept over a sleep (its 55), a listing's `total` (56) and the update members of
  the heartbeat (61, which a unit does not send).
- [ ] **[none] What the unit has shown.** 14/04: the apply of 18 November (job 34584, the
  Walter's own image) went through and its trial was confirmed; no apply of a radio image has
  been watched since the task. A lease that cannot be extended is logged and the apply goes on
  (~L310–316). 14/06: woken on request on 17 November, heard six minutes later. 13/01–02: the
  reports of 18 November, read whole. 15/06: `wakesMidway` 0 since the update. 15/07: the
  answered call beside the failed one, which is what `claude/walter-observation-stall.md` in
  the project was written from.
- [ ] **[none, parked] 15/03 did not end the lost answer.** `httpFailures` still rises by one a
  heartbeat (+88 in a day on 18 November). It waits for the bench with the new adapter board
  (Thomas, 18 November), and nothing here should be read as a cure.
- [ ] **[none] 15/06's two open criteria:** the heap margin 80 bytes below main's is long
  overtaken by pile 17's 11 608 bytes, and *CI green* was the pull request's, which merged.
