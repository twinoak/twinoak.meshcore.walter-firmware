# 07 — What a call that worked looks like

**Depends on:** 05
**Spec:** `components/core/include/core/net/call_record.h` and `components/core/net/call_record.cpp` (the record of task 02: made for every call, kept only for one that failed; `describe` ~L255), `components/modem/walter_tls.cpp` (`socketEvent` ~L18–52, `receive` ~L205–260), `components/core/net/http_over_tls.cpp` (the status and the framing of a response ~L40–46; the loop ~L133), `components/interfaces/include/interfaces/http.h` (`HttpResponse`, the wanted headers), `components/agent/modem_uplink.cpp` (`call` ~L196, `wanted[]` ~L252), `components/agent/call_log.cpp`, `components/agent/agent_backend.cpp` (`agent calls` ~L810–830), `tasks/phase-15/reports/02.md`, `tasks/phase-15/FOLLOWUPS.md`, `docs/bench.md` §13, `spec/SYSTEM.md` S-12
**Size:** one session.
**Model:** claude-opus-5

## Goal

The unit has now said twice what the modem did in an observations call that failed:

```
wake 254, socket 1, 4161 bytes sent, 5 receives answered "nothing buffered yet"
  +320 ms: a ring: the modem announced 815 bytes
  +339 ms: a ring: the modem announced 5 bytes
  +359 ms: the other end closed
  +30310 ms: a receive failed after 30000 ms

wake 289, socket 6, 5941 bytes sent, 7 receives answered "nothing buffered yet"
  +397 ms: a ring: the modem announced 815 bytes
  +416 ms: a ring: the modem announced 5 bytes
  +436 ms: the other end closed
  +30398 ms: a receive failed after 30001 ms
```

The answer arrives whole, in two pieces, and the server closes behind it. The receive that
never comes back was sent at +310 ms in the first and at +397 ms in the second: before the
first ring was handed over, and in the same millisecond.

What cannot be said is what is different about a call that works. The config, the heartbeat,
the claim and a job's result are answered at every wake by the same server through the same
code, and nothing is kept of them. Does their answer come in two pieces? Does the server close
behind it? When is the receive sent, against the rings? Until a good call can be laid beside a
bad one, every cure is a guess.

A report is made in the middle of a wake, straight after calls that worked. Their record is
still in memory.

## Deliverables

1. **The latest call this boot that was answered, printed beside the failed one.** `agent
   calls` and the report's `calls` section: which call it was and the same lines the kept
   record has. From memory; nothing more is kept in flash.
2. **When each receive was sent, and when it came back**, for the receives that returned bytes
   as for the one that failed, so that a read can be placed against the rings in both.
3. **What the answer said of itself**, for a call that was answered: its status, whether it
   gave a length or came in chunks, how many bytes, whether it said `Connection: close`, and
   whether the other end's close was handed over before or after the last byte was read.
   Numbers and those words only (S-12).
4. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
5. **The words.** `docs/console.md`. `docs/bench.md` §13: the two blocks to lay side by side,
   and what each difference between them would mean. One line in `release-notes/unreleased/`.
   One line under the first item of `FOLLOWUPS.md`; the box stays unticked.

## Acceptance criteria

- [ ] A scripted wake whose config, heartbeat and claim are answered: `agent calls` prints the claim's record, with its rings, each receive's two times, the bytes, the status, the framing, and where the close fell (test).
- [ ] The same with a failed call kept from an earlier wake: both are printed and each is named (test).
- [ ] A boot in which nothing has been answered yet: it says so (test).
- [ ] After a sleep the answered call's record is gone and the failed one's is as it was (test).
- [ ] Neither block holds a byte the server or this board sent, nor a header's value beyond the facts of deliverable 3 (test).
- [ ] The kept record of a failed call written before this task still prints (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Keeping an answered observations call over the sleep: it would be a write to flash at every
good wake, and the calls a report can see come first. The cure. The library's `TX:` and `RX:`
lines: the trace build's. The board: no session opens a port. A release: Thomas presses the
button.
