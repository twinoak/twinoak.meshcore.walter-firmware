# 02 — What is kept for the asking

**Depends on:** 01 (the report's `calls` section, and the lines of `wake`).
**Spec:** `components/agent/modem_uplink.cpp` and `include/walter_agent/modem_uplink.h` (every call to the server passes here), `components/agent-core/src/AgentLoop.cpp` (`noteFailure`, `state`, `restore`; **a copy: not edited here**), `components/agent/agent_backend.cpp` and `include/walter_agent/agent_backend.h` (the loop's state as hex under `agent_state`; strings of 160), `components/interfaces/include/interfaces/key_value.h` and `retained.h` (RTC memory has no slot left: `tasks/phase-3/reports/05.md`), `components/core/include/core/cycle/wake_reason.h` and `wake_reason.cpp` (the last ten wakes, eight bytes each; `timeAwayMs()`), `components/core/cycle/wake_cycle.cpp` (`WakePayload`: `reached`, `owed_s`; what is *news* ~L1236–1255), `components/agent/identity.cpp` and `include/walter_agent/identity.h` (the heartbeat's counters, `kErrorCount`), `components/core/console/console.cpp` (`wake`, `agent state`), `docs/console.md`, `docs/cycle.md`; **meshmaster.dk, read only:** `docs/dev/unit-protocol.md` §2 (`errors`), `src/MeshMaster.Core/Transport/AgentProtocol.cs` (~L370, the bound on the counters' JSON).
**Size:** one session.
**Model:** claude-opus-5

## Goal

On 13 November meshmaster.dk showed *httpFailures 512* for the unit and nobody could say which
calls had failed, with what, or when: the counter is a total with no start date, and the reason
for each failure is one log line on a port nobody was reading.

Task 01 sends what the unit knows. This task makes it know three things it forgets today.

## Deliverables

1. **The last failed calls.** The unit keeps the last eight calls to the server that did not come
   back 2xx: which call (config, heartbeat, claim, a result, observations, progress, a lease, a
   download range), what came back (the HTTP status, or that nothing answered and how far the
   uplink had got), the wake's number, and the time when the clock was set. Kept across a deep
   sleep and a power cut. Recorded where the Walter's own uplink makes the call. `agent calls`
   prints them newest first, and the report's `calls` section is those lines.
   - The library's counter leaves 401 and 429 out. This list keeps them: the question is what
     came back.
2. **Why a wake sent nothing, in the list of ten.** `wake` already marks a wake that sent nothing
   with three dashes. It says why as well: a wait still owed and how many seconds, the uplink's
   backoff, no uplink, a precondition. Today only a timer wake inside a wait leaves that anywhere
   (the journal, as news); a wake by the radio's line inside a wait leaves nothing, and that is
   the wake a person asked for.
3. **Four counters in the heartbeat**, beside the ones it carries: `wakesTimer`, `wakesRadio`
   (the radio's line, with a valid attention), `wakesSpurious`, `wakesSilent` (a wake that sent
   nothing, whatever woke it). Kept as `httpFailures` is. Say what the server's bound on the
   counters is and that the body stays inside it.
4. **Flash is not spent on it.** Say how many writes a wake makes for these three, and that a wake
   with no failed call and nothing new to say writes no more than a wake does today.
5. **The words.** `docs/console.md` (`agent calls`, the new words of `wake`), `docs/cycle.md`; a
   line in `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] A scripted wake: the heartbeat is answered 503 with `Retry-After`, the claim gets no answer. `agent calls` lists both, newest first, each with its call, what came back and the wake's number; the list is the same after a simulated deep sleep and after a simulated power cut (tests).
- [ ] A ninth failed call pushes out the oldest (test). A 401 and a 429 are in the list (test).
- [ ] A scripted wake by the radio's line inside a wait: its line in `wake` says nothing was sent, that a wait was owed, and the seconds (test). The same for the uplink's backoff (test).
- [ ] After a timer wake, a wake by the radio's line, a spurious wake and a silent one, the heartbeat's body carries the four counters with the right values, and they survive a deep sleep and a power cut (tests).
- [ ] The heartbeat's counters, with the four, serialise inside the server's bound, which the report names (test).
- [ ] A claimed `twinoak.report` for `calls` returns the lines of `agent calls`, and `unknown` is empty (test).
- [ ] A wake with no failed call and no change makes no flash write it did not make before (test).
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

What a wake does with a wait it owes. A wake by the radio's line believes no time has passed
(`timeAwayMs()` is 0 for anything but the timer), so the whole wait is still owed and nothing is
sent. That may be why 13 November's wake request did nothing; it is not known, and it is decided
after the first report has been read (FOLLOWUPS). The library copy. The board.
