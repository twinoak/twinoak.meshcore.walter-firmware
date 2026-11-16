# 03 — Read again before giving up

**Depends on:** 02
**Spec:** `components/modem/walter_tls.cpp` (`receive` ~L147–187: a failed receive with a close seen answers *closed*), `components/core/net/http_over_tls.cpp` (~L133–160), `components/core/include/core/net/socket_rotation.h` (the rule with no modem in it, and why it is written that way), `components/interfaces/include/interfaces/tls_socket.h` (`TlsRecv`), `components/interfaces/include/interfaces/http.h` (`kClosed`, `kSocketFailed`), task 02 of this pile (the record), `host_test/core/` (the tests of the rotation and of HTTP over TLS)
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

When a receive command fails and the connection's close has been seen, `receive` answers
*closed by the other end* and the call ends: *nothing answered*. It does not ask the modem
again.

In the failure of task 02 the server had answered, every time. The close that was seen is the
server hanging up after its answer. Whether the modem still holds those bytes when the failed
command gives up, nobody has looked. If it does, one more ask is the whole cure for the data:
the call ends well, the batch is taken off the ring, and nothing is posted twice. It is still
30 seconds late.

## Deliverables

1. **One more ask.** After a receive that failed, with the close seen: ask the modem once more
   for what it holds. Bytes are the answer, and the response is read on from there as if the
   first ask had returned them. Nothing, or a second failure: *closed*, as today. Once, not a
   loop, and inside the call's deadline.
2. **Nothing else moves.** A failed receive with no close seen is `kSocketFailed`, as today. A
   close seen with nothing failed is the end of the response, as today.
3. **The rule where a host test drives it.** As the rotation's rule is: no modem in it, and the
   modem component only does what it says.
4. **Seen.** The record of task 02 says a read after a failed receive and what it returned. A
   count of answers read that way is kept over the sleep and printed by `agent calls` and the
   report's `calls` section, so that a unit in service says whether this ever does anything.
5. **Room.** `tools/heap_at_start.py` over this task's build; the margin is not below main's.
6. **The words.** `docs/` where a call's outcomes are listed. One line in
   `release-notes/unreleased/`.

## Acceptance criteria

- [ ] A scripted socket whose receive fails with the close seen and whose next receive returns a whole response: the call is answered 2xx, the body is whole, and the count is one (test).
- [ ] The same with nothing at the second ask: *the connection closed before the body was complete*, as today, after one more ask and no more (test).
- [ ] The same with the second ask failing too: *closed*, and no third (test).
- [ ] A receive that fails with no close seen: `kSocketFailed`, and no second ask (test).
- [ ] A response already half read when a receive fails with the close seen: the second ask's bytes complete it (test).
- [ ] The record of task 02 shows the second ask and its bytes (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The 30 seconds: they are the library's command timeout, and what makes a command go unanswered
is not known yet. Asking the modem less often, or only after a ring: not without the cause. The
library: not changed here. The board: no session opens a port. A release: Thomas presses the
button.
