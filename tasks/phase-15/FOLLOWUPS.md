# Phase 15 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas, the bench] The cause of the lost answer is still not known.** Tasks 02 and 03
  make it readable and soften it. What makes one modem command go unanswered for 30 seconds
  after an observations post, and almost never after another call, is for the trace build to
  show (`docs/bench.md` §13, the ★ line; `tasks/phase-10/FOLLOWUPS.md`, first item). On
  8 November the three answers this board received were 876 bytes and the six it lost were 877
  or 878. On 16 November about one new batch in five was received during the morning, so that
  figure alone does not decide it.
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
