# 03 — One connection, one socket

**Depends on:** nothing in this phase.
**Spec:** `components/modem/walter_tls.cpp` (`g_ring`/`g_closed`, written by the library's event task; `WalterTlsSocket::open` clears both before it dials; every connection is `kSocketId` 1), `components/modem/include/modem/walter_tls.h`; the library, pinned (`managed_components/dptechnics__walter-modem/src/WalterModem.cpp`): the `+SQNSH: ` URC (about line 3201: it sets the socket's state to FREE and **queues** a `DISCONNECTED` event) and the event task's loop (about line 1600: an event is taken off the queue, then `vTaskDelay(10)`, then dispatched); `WALTER_MODEM_MAX_SOCKETS` (6, `WalterModem.h`); the GM02SP's AT manual on `AT+SQNSD`, `AT+SQNSH` and the `+SQNSH` URC (how many sockets, and when the URC comes); `components/core/net/http_over_tls.cpp` (one connection a request: open, send, receive to the Content-Length, close); `components/interfaces/include/interfaces/tls.h` (or wherever `TlsSocket` lives), the host tests' fake of it; `../phase-6/FOLLOWUPS.md`, *A response body that stalls, and the job it orphans*.
**Size:** one session.
**Model:** claude-opus-5

## Goal

6 November, on the first unit (RSRP -86 dBm, TDC band 20). The first request of every wake worked;
the second did not, three wakes in a row:

```
/agent/v1/heartbeat -> http 200, 145 bytes, 1299 ms
/agent/v1/observations -> no reply: the request did not go out after 444 ms

/agent/v1/heartbeat -> http 200, 143 bytes, 4453 ms
WalterModem: Command time-out (TX) Attempt 1 of 1
/agent/v1/jobs/claim -> http 200 but the body did not arrive whole: the connection closed before the body was complete (31120 ms)

(after a full power cycle)
/agent/v1/config -> http 200, 180 bytes, 2691 ms
/agent/v1/heartbeat -> no reply: the request did not go out after 506 ms
```

The claim's case is the expensive one. meshmaster.dk had leased the job (21026) when it answered, and
the Walter never saw it, so the job died with its one attempt.

**The cause, read from the code.** A request ends when its Content-Length has been read; it does not
wait for the server's close. That close arrives as `+SQNSH: 1` a moment later. The library sets
socket 1's state to FREE and queues the event, and its event task dispatches it later still. By then
the next request has opened socket 1 again and cleared `g_closed`. The late event sets it, and the new
connection is taken for closed: `send` stops (*did not go out*), or `receive` ends early (*closed
before the body was complete*). The library may also now hold the live socket as FREE. Timing decides
who wins, which is why some wakes got through.

## Deliverables

1. **Consecutive connections never share a socket id.**
   - Alternate between two ids, with their events kept apart: a `DISCONNECTED` or a ring for the id
     not in use changes nothing about the one in use. Read the GM02SP's socket count and the
     library's.
   - Decide, and test, what happens when a close arrives **two** connections late, on the id that is
     about to be used again. For example, a new connection on an id waits a bounded time for that
     id's previous close to have been seen, then ignores the first close it gets if that one is
     still owed. Whatever the rule, it is deterministic and written down.
   - The flags are per id, and safe to write from the library's task and read from the main task.
2. **The logic where a host test can reach it.**
   - Which id, which events count, and what an owed close is: a small piece in `core/net/`, with the
     modem layer a thin caller.
   - The fake modem of the host tests learns the board's habit: it can deliver a connection's close
     **after** the next connection's dial, or after the one after that.
3. **Tests that fail first.**
   - Two `HttpOverTls` requests back to back, with the first connection's close delivered late:
     - before the change, the second fails as on the board (show it in the report);
     - after it, both succeed.
   - The same with the close two connections late.
   - A real early close by the server, during the body, is still reported as one.
4. **`docs/bench.md`**, for Thomas, after 06's release:
   - `http get https://meshmaster.dk/` twice in a row, twenty times: forty answers of 200;
   - `agent wake` with config, heartbeat, claim and observations all `done`;
   - a job claimed, run and its result posted in **one** wake.

## Acceptance criteria

- [ ] Consecutive connections use different ids; a late close of either earlier connection cannot end the current one (tests, failing before).
- [ ] The id and event logic is in `core/`, host-tested; the modem layer is thin.
- [ ] A genuine early close is still an error; `docs/bench.md` has the lines; the `../phase-6/FOLLOWUPS.md` item ticked.
- [ ] `tools/verify.ps1` green.

## Out of scope

Keeping one connection open across requests (HTTP keep-alive). It would also avoid the race, but it is
a change to how a wake talks to the server; propose it in the report if it is worth it. The server's
side of a lost claim answer is meshmaster.dk's pile 4.25.
