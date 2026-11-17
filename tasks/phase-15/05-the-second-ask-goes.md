# 05 — The second ask goes

**Depends on:** —
**Spec:** `components/core/include/core/net/read_again.h` and `components/core/net/read_again.cpp` (the rule of task 03, its two totals, `kReadAgainKey` ~L118), `components/modem/walter_tls.cpp` (`receive` ~L205–260: the rule's call ~L243–257; the ask itself ~L179–201), `components/core/include/core/net/call_record.h` (`CallMoment::kReadAgain`, `again()` ~L208), `components/agent/call_log.cpp` (~L63, ~L175, ~L221), `components/agent/agent_backend.cpp` (`agent calls` ~L822), `host_test/core/net/test_read_again.cpp`, `host_test/sim/fake_tls_socket.h`, `tasks/phase-15/reports/03.md`, `tasks/phase-15/FOLLOWUPS.md` (*From the review of 01 to 03*)
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

Task 03 gave a receive that failed with the close seen one more ask. The unit has had it since
the evening of 16 November. On the morning of the 17th its report said:

*asked again after a failed receive: 35 since this unit was made, 0 of them answered*

and in both records read so far the second ask came back at once with `+CME ERROR: 4`: the
modem holds nothing by then. The bytes are gone when the first command gives up.

It is also not free. A second command sent straight after one the modem did not answer can go
unanswered too: the first timer wake on that firmware was 68 seconds awake where the others are
about 40. And in that first night two wakes ran into the five-minute bound, which had not
happened in the three nights before. Whether the second ask had a part in those is not known.
It has cured nothing in 35 tries, so it goes.

## Deliverables

1. **No second ask.** A receive that failed with the close seen answers *closed*, with no
   further command to the modem: what this firmware did before task 03. A failing call costs
   one command timeout again.
2. **What task 02 made stays.** The record, its key and its lines are as they are. A record
   kept by the firmware of task 03 that holds a second-ask moment still prints.
3. **The count goes with the rule.** `agent calls` and the report's `calls` section no longer
   print the *asked again* line, and the key it was kept under is removed from the store once.
4. **The bench socket keeps what it learned.** A failed receive with the close delivered still
   answers *closed* there, as the board does.
5. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The words.** `docs/` and `docs/bench.md` where task 03 is described. One line in
   `release-notes/unreleased/`. One line under the review's item in `FOLLOWUPS.md`; the box
   stays unticked.

## Acceptance criteria

- [ ] A scripted socket whose receive fails with the close seen and whose next receive would return a whole response: the call ends *the connection closed before the body was complete* after one command, and no second is sent (test).
- [ ] A receive that fails with no close seen: `kSocketFailed`, as today (test).
- [ ] A record stored by the firmware of task 03, second-ask moment and all: `agent calls` prints it as that firmware did (test, with the stored hex as a fixture).
- [ ] After a boot the store holds no key of the rule's totals, and `agent calls` prints no *asked again* line (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Why the first receive goes unanswered. Asking the modem at another moment or in another way:
not before a call that works has been seen beside one that does not (task 07). The library. The
board: no session opens a port. A release: Thomas presses the button.
