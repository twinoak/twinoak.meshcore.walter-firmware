# 02 — What a wake did

**Depends on:** 01.
**Spec:** `components/core/cycle/wake_cycle.cpp` (`journalWake`: *news, and nothing else*; the flag `kWakeFlagExchanged`, set whenever the exchange was not `kFailed`), `components/interfaces/include/interfaces/backend.h` (`Exchanged`: `kAsked`, `kNothing`, `kFailed`), `components/agent/agent_backend.cpp` (`if (!ran) return interfaces::Exchanged::kNothing`: a loop that sent nothing is *nothing to do*), agent-core's `AgentLoop::wake` (`report.why`), `components/core/cycle/wake_reason.h` (the last ten wakes in RTC memory), the console's `wake` and `journal` (`console.cpp`, the line *the backend was reached* ~L3885), `docs/console.md`; `tasks/phase-9/FOLLOWUPS.md`, *`wake` says how long each wake was, not what it did*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Two readings of the same console went wrong on 7 and 8 November, both in this firmware's words.

- **`wake` gives each wake's length and nothing else.** Three wakes of 40 s and four of 6 s were read
  first as wakes that skipped the modem, then as a failed connection. `lte` showed both were wrong.
- **The journal says *the backend was reached* when nothing was sent.** The flag is set for every
  exchange that did not fail, and a loop that sent nothing because of a hold is *nothing to do*, not
  a failure. On 8 November wakes that sent nothing to meshmaster.dk said they had reached it.

## Deliverables

1. **Three answers, not two.** The seam says whether the backend was **answered**, **sent nothing**
   (with the loop's own reason: a hold, no uplink, not configured, halted) or **failed**. Either a
   value of `Exchanged`, or the reason carried beside `kNothing`; choose, and say why. Every caller
   of the seam reads it the same way.
2. **The journal.** *the backend was reached* only when a heartbeat was answered. A wake that sent
   nothing because of a wait says so (*nothing sent: a wait still owed, N s*). Whether such a wake is
   news is yours to decide against the journal's flash budget; say how many entries a night of them
   would cost.
3. **`wake`'s list**: one short word or letter per wake beside its length, for whether the modem came
   up, whether the server answered, and whether jobs ran. Legible on a 100-column terminal.
4. **Tests** against the fakes: a held wake, an answered wake with jobs, a heartbeat that failed, and
   what `wake` and `journal` print for each.
5. **`docs/console.md`**: `wake` and `journal`'s new words. `tasks/phase-9/FOLLOWUPS.md`'s item
   ticked.

## Acceptance criteria

- [ ] The seam tells answered, sent nothing and failed apart; the journal says *reached* only for an answered heartbeat (tests).
- [ ] `wake` says per wake what it did; docs updated; `tools/verify.ps1` green.

## Out of scope

The hold itself (01).
