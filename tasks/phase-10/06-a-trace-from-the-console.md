# 06 — A trace from the console

**Depends on:** 05 (the request lines it changes are the ones a trace shows).
**Spec:** `FOLLOWUPS.md`, *The 30-second stall is reproducible, and it is the answer that is lost* (8 November, with meshmaster.dk's log: every lost answer was sent with a 200 in under a second); `tasks/phase-9/reports/02.md` (*The stall, read against the code*: the unanswered `AT+SQNSRECV`) and `docs/bench.md` §13 (the trace build today: `CONFIG_LOG_DEFAULT_LEVEL_DEBUG` and `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`, flashed by cable, *never on a unit in service*, and the four things the trace must show); `sdkconfig.defaults` (`CONFIG_LOG_DEFAULT_LEVEL_INFO`); the modem library, `managed_components/dptechnics__walter-modem/src/WalterModem.cpp` (its `ESP_LOGD` lines, `_logEscapeBuffer` under `CONFIG_LOG_MAXIMUM_LEVEL >= ESP_LOG_DEBUG`, and what it prints of the bytes it sends and receives); `components/modem/walter_tls.cpp`; the console's `dump` ring (`docs/console.md`: *a secret never reaches a printed line*); spec/SYSTEM.md S-12.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The stall that silences the unit is now reproducible on the bench in minutes. With adverts waiting,
`agent wake` loses the answer to an observations POST almost every time. meshmaster.dk's log shows
that answer was sent, a 200 in under a second. What happened to it is in the modem library's TX and
RX lines, and today those lines exist only in a special build that Thomas would have to make and
flash by cable. This task puts the trace into the release build, switched on from the console.

## Deliverables

1. **The modem library's debug lines compiled into the release build**, and off by default.
   - Either the global maximum log level, or only the library's own files at debug, so the rest of
     the firmware is not made bigger by strings nobody switches on. Choose, and give the image size
     before and after.
   - The default level stays where it is. A unit that nobody switches the trace on for prints
     exactly what it prints today.
2. **`trace modem on|off`**, or whatever name fits the console's grammar.
   - It sets the library's log level at runtime.
   - It is not a setting: it ends at the next boot, and `status` says while it is on.
   - It prints what it does, and one line saying that the trace is for the bench.
3. **No secret in a trace line.** The request that goes to the modem carries `Authorization: Bearer
   <token>` in plaintext: TLS is inside the modem, so the UART carries the HTTP head as it is.
   - Read what the library prints of the bytes it sends and receives. If any line can contain a
     request's head, that line must not reach the console or `dump` with the token in it.
   - The token is this firmware's own value and its position in the head is this firmware's own
     doing, so it can be replaced exactly before the line is printed (a log hook), or the payload
     lines kept out altogether. That is not a pattern-guess. Choose, and say which in the report.
   - A test sends a request with a known token through the hook and finds it nowhere in what was
     printed or kept.
4. **The `dump` ring** keeps trace lines or not: decide, with the 4 kB in mind and a stall's trace
   being longer, and say so.
5. **`docs/bench.md` §13 rewritten**: no special build. `trace modem on`, `lte up`, `agent wake` with
   adverts waiting, `trace modem off`. Then the four things the trace must show, and one more: the
   `+SQNSRING`/`+SQNSRECV` lengths of an answer that arrived against one that was lost. (The log's
   response sizes on 8 November were 876 bytes for every one received, and 877 or 878 for every one
   lost.)
6. **Tests:** the command, the end at boot, the token hook.

## Acceptance criteria

- [ ] The release build can trace the modem library from the console, off by default and gone at the next boot; the image size before and after is in the report.
- [ ] No token in any trace line or in `dump` (test); `docs/bench.md` §13 needs no special build.
- [ ] `tools/verify.ps1` green.

## Out of scope

Fixing the stall. That needs the trace this task makes possible.
