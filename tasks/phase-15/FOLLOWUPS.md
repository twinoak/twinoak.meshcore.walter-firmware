# Phase 15 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas, the bench] The cause of the lost answer is still not known.** Tasks 02 and 03
  make it readable and soften it. What makes one modem command go unanswered for 30 seconds
  after an observations post, and almost never after another call, is for the trace build to
  show (`docs/bench.md` §13, the ★ line; `tasks/phase-10/FOLLOWUPS.md`, first item). On
  8 November the three answers this board received were 876 bytes and the six it lost were 877
  or 878. On 16 November about one new batch in five was received during the morning, so that
  figure alone does not decide it.
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
