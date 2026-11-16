# 02 — What the modem said

**Depends on:** —
**Spec:** `components/modem/walter_tls.cpp` (`socketEvent` ~L18, `send` ~L129, `receive` ~L147–187), `components/core/net/http_over_tls.cpp` (the receive loop ~L133–160), `components/core/include/core/net/socket_rotation.h` (`onRing`, `onClose`), `components/interfaces/include/interfaces/http.h` (`kClosed`, `kSocketFailed` and the paragraph between them), `components/interfaces/include/interfaces/tls_socket.h`, `components/agent/include/walter_agent/call_log.h` (nine bytes an entry, eight in one value), `components/agent/modem_uplink.cpp` (`call` ~L196, `noteFailedCall`), `components/agent/agent_backend.cpp` (`agent calls` ~L810–830), `tasks/phase-10/FOLLOWUPS.md` (its first item), `tasks/phase-13/FOLLOWUPS.md` (*a second key, not a longer entry*), `docs/bench.md` §13 (the ★ line), `spec/SYSTEM.md` S-12
**Size:** one session.
**Model:** claude-opus-5

## Goal

At nearly every wake the unit's observations call fails: *nothing answered, the connection
closed before the body was complete*. It has failed 1089 times. On 16 November all eight calls
in `agent calls` were that one.

What is known:

- The request goes out whole. meshmaster.dk stores the batch, and its log of 8 November has a
  `200` in under a second for every post this firmware called lost.
- The call lasts 30 seconds, which is the modem library's command timeout. A wake with it is
  38 to 40 seconds awake; a wake without it is 9.
- The other calls of the same wake are answered in about a second each.
- It is this board only. The Heltec exit nodes use the same route over WiFi and lose nothing.

What is not known is what the modem did in those 30 seconds: whether it announced the answer,
which command went unanswered, when the close came. The library's own trace would say, and it
does not fit a release image (`docs/bench.md` §13). Nothing this firmware writes down says, and
a deep sleep takes the console.

This task makes the firmware write it down, small enough for a release image, so that the unit
in service says it in its report.

## Deliverables

1. **A record of one call's receiving side**, made as the call runs. For each thing that
   happens after the request has gone out, how long after and what:
   - a receive that returned bytes, and how many;
   - a receive that said *not yet*: counted, not listed;
   - a receive that failed: the modem's error code, or *no answer*, and how long the command took;
   - a ring, with the length the modem announced when the library hands it over;
   - the close.

   And once for the call: the socket id, the bytes sent, how it ended. A dozen events and a
   count of the rest. No byte of a payload, a header, a token or a name (S-12).
2. **Kept when the call failed.** The record of the newest call that did not come back 2xx
   survives the sleep, a restart and a power cut, under a key of its own in the settings store.
   A call that succeeds leaves the kept record as it is.
3. **Read where the list is read.** `agent calls` prints it under the list, and with it the
   report's `calls` section, inside the section's bytes.
4. **The record is core's.** The events are taken where they happen, in the modem component;
   what a record holds, how it is packed and how it is printed has no modem in it and is tested
   on the host.
5. **Room.** Static storage this adds is paid for. `tools/heap_at_start.py` over this task's
   build; the margin is not below main's.
6. **The words.** `docs/console.md`. `docs/bench.md` §13: how to read the record beside the
   trace build's lines. One line in `release-notes/unreleased/`. One line under the first item of
   `tasks/phase-10/FOLLOWUPS.md` saying what this task did; the box stays unticked.

## Acceptance criteria

- [ ] A scripted call whose receive is not answered for 30 s, with a ring and the close arriving meanwhile: the record holds the count of *not yet*, the failed receive with its duration, the ring with its length and time, the close with its time, the socket id and the bytes sent (test).
- [ ] The same record after a sleep, after a restart and after a power cut (test).
- [ ] A call answered 2xx after a failed one: the kept record is still the failed one's (test).
- [ ] A call with more events than the record holds: the first dozen and a count of the rest (test).
- [ ] `agent calls` and the report's `calls` section print the record; neither prints a byte the server or this board sent (test).
- [ ] The heap margin is not below main's. Rail R22 green.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

Why the answer is lost: this task makes it readable and cures nothing. The library's `TX:` and
`RX:` lines: the trace build's. A second read after a failed one: task 03. The library: not
changed here. The board: no session opens a port. A release: Thomas presses the button.
