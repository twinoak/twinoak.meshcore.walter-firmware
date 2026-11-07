# 02 — The sixth connection

**Depends on:** nothing in this phase.
**Spec:** `../phase-8/FOLLOWUPS.md`: the review's S2 (*a close that never comes leaves a socket id one close behind*) and S3 (*a check-then-set race in `SocketRotation`*); *The fifth connection of a wake still stalled* (7 November, two wakes); *`DEPRECATION: Use socketConfig(…)`*. `tasks/phase-8/03-one-connection-one-socket.md` and its report; `components/core/net/` (`SocketRotation`), `components/modem/walter_tls.cpp`, `components/modem/include/modem/walter_tls.h` (`kOwedCloseWaitMs`, *TO BE SET ON THE BENCH*); the library, pinned (`managed_components/dptechnics__walter-modem/src/WalterModem.cpp`: the `+SQNSH` URC, the event task, `_socketGet`; `WalterSocket.cpp`: `socketConfig`'s current signature, the ids it allows); the GM02SP's AT manual (`AT+SQNSD`, `AT+SQNSH`, `+SQNSH`, `AT+SQNSRECV`, how many TLS sessions at once); the 23 October trace build (phase-6 report 02, how it was made).
**Size:** one session.
**Model:** claude-opus-5

## Goal

v0.1.1's alternating sockets ended the failures of the *second* request of a wake. On 7 November it
was a later one that stalled, twice, and both times after five good connections:

- `http get` twice: four back-to-back connections, all fine;
- a wake: config, heartbeat, claim and a result went through, then *observations* hit
  `WalterModem: Command time-out (TX)` and *the connection closed before the body was complete
  after 30935 ms*;
- the next wake: heartbeat, claim, two results and a first observations batch, five connections,
  went through. The **second** batch then stalled the same way, 31055 ms;
- a third shape, 19:20 UTC: heartbeat and claim went through, then the job's result failed at the
  **dial**: `the modem refused the connection: the host was unreachable or the TLS handshake failed`,
  after 12532 ms, on the third connection of the wake, which is the first id again. The very next
  connection (observations) went through in 3.7 s. The result was kept and sent again, as designed.

The review of phase 8 had already named two ways a socket id can go wrong, S2 and S3. Neither was
fixed, and the counters that would tell which it was are printed nowhere.

## Deliverables

1. **S3:** the check-then-set between `end()` (the main task) and `onClose()` (the library's event
   task) is under a critical section, or made a single atomic transition. Add a test that drives the
   interleaving the review describes.
2. **S2:** a close owed on an id that never comes is not a debt for the modem's life.
   - Bound it: the debt expires at the id's next successful exchange, or after N connections. Choose
     one deterministic rule, and test it with a fake modem that never sends `+SQNSH` after our own
     `AT+SQNSH`.
   - Set `kOwedCloseWaitMs` from what the library and the AT manual say, with the reason, until the
     bench measures it.
3. **The counters on the console.** `http get` and `lte` print `SocketRotation`'s counters: which id
   each connection used, `owedTimeouts`, `discountedCloses` and `otherIdEvents`. With them, the next
   stall tells which id it was on and whether a close was owed.
4. **The stall itself.** Read the pattern against S2, S3 and the modem:
   - Is the sixth connection the third on one id?
   - Does the GM02SP keep a TLS session per id that a reused id inherits?
   - Is it the second POST in a row?
   - Which AT command timed out (`Command time-out (TX)`)? The library's trace build names it.
   - Was the refused dial `AT+SQNSD` on an id the modem still held open or closing? Does the dial
     need that id's `+SQNSH` first, or a wait?

   If the code alone does not settle it, write what the trace must show and the exact bench lines
   that get it (`docs/bench.md`). A fix that cannot be proved here is a follow-up, not a guess.
5. **The library's current `socketConfig`** signature, so the `DEPRECATION` line goes.

## Acceptance criteria

- [ ] S3 under a lock, with the interleaving test; S2's debt bounded by one written rule, tested against a modem that never answers a close.
- [ ] `http get` and `lte` print the rotation's counters; `socketConfig` is current.
- [ ] The stall: a cause shown, or the trace and bench lines that will show it.
- [ ] The `../phase-8/FOLLOWUPS.md` items ticked or moved here; `tools/verify.ps1` green.

## Out of scope

HTTP keep-alive (03's report proposed it; not here). The board.
