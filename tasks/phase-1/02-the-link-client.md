# 02 — The link client

**Depends on:** 01.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §1–§5, §8–§10; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §3; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `software/firmware/noisescope-rak4630/PROTOCOL.md` (what the *other* firmware says on this UART).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The Walter talks to the radio. One request at a time, with the retries the
protocol asks for; the radio's restarts noticed; its events delivered; and
when the firmware over there is not a repeater but the scanner, the client says
so instead of failing mysteriously. From the console: `rak hello`.

## Deliverables

- **`components/hal/`**: a byte-stream interface (read what is there, write,
  flush, "has the line been idle") and its ESP-IDF implementation in
  `platform/` on a UART at 115200 8N1, GPIO7 TX and GPIO8 RX — enabled and
  released **through `RadioPins`** (phase 0, task 03): the UART owns its pins
  only while the radio is believed powered.
- **`components/core/link/client/`**:
  - **requests**: one in flight; a `seq` that differs from the last; the
    response awaited 1000 ms; **the same frame, the same `seq`**, sent again at
    most twice; then *link down*, as a result, not an action. A response with
    another type or `seq` is counted and ignored.
  - **typed calls** over the codec for the session and local-management
    messages: `hello`, `ping`, `status`, `timeSet`, `timeGet`, `reboot`,
    `identity`, `sleepNotice`, `cli`, `powerGet`, `envGet`, `lppGet` — each
    returning the decoded response or the reason there is none (link down,
    refused with which status, malformed).
  - **the mesh client's calls** (`jobBegin` … `meshCounters`, `obsPull`,
    `replyGet`) as the same kind of typed call. They are thin, the codec does
    the work, and having them now means the whole protocol is exercised from
    this end; nothing uses them until the agent exists.
  - **the session**: `HELLO` and what it learned — the protocol version agreed,
    role, slot, capabilities, versions, `boot_id`. A capability the radio does
    not advertise makes the corresponding call fail fast without sending. A
    **changed `boot_id`**, or an `EV_BOOT`, raises "the radio restarted" to
    whoever subscribed, and the client's first act after it is `TIME_SET` when
    it has a time to give (the clock is an interface; phase 3 fills it).
  - **events**: `EV_BOOT`, `EV_MESH_REPLY`, `EV_ATTENTION` decoded and queued
    (a small bounded queue; overflow counted), their own sequence numbers
    watched for gaps.
  - **which firmware is it** (§9): `HELLO` three times with no valid response;
    then, if lines beginning with `{` were seen — or one arrives in answer to
    `info\n` — the radio is running the scanner: the client reports role
    *scanner (text)*, stops sending frames, and offers the lines one by one to
    a subscriber. A reset of the radio returns it to "unknown, try `HELLO`".
    Frames and text are never mixed in one session.
- **The console**: `rak hello`, `rak ping`, `rak status`, `rak id`, `rak time`
  / `rak time set <epoch>`, `rak power`, `rak env`, `rak cli <line>`,
  `rak events` (print what is queued), `rak text` (print scanner lines). Each
  prints the decoded answer in words, or why there is none.
- **`host_test/sim/ScriptedRadio`**: a fake radio on the other end of a fake
  stream, driven by the fixtures — it answers a request with the fixture's
  response, can be told to stay silent N times, to answer late, to answer with
  a wrong `seq`, to corrupt a byte, to restart (new `boot_id`), to raise an
  attention, or to behave as the scanner. Later phases reuse it.

## Tests (host)

- Every request case of the fixtures, sent through the client to the scripted
  radio and answered with the fixture's response: the decoded result equals the
  fixture's fields.
- Silence once, twice (recovered: three frames on the wire, **byte-identical**),
  three times (link down). A late response after the retry was sent. A response
  with the wrong `seq`. A corrupted response.
- A restart: the next `hello` shows a new `boot_id`, the subscriber is told,
  and `TIME_SET` is the next request on the wire.
- A capability missing: the call fails without a byte sent.
- Events between a request and its response; an event queue overflow; a gap in
  event numbers.
- The scanner: no `HELLO` answer, lines with `{`; and the `info\n` probe.
- An attention's leading `0x00 0x00` and repeats do not confuse the receiver.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The retry test proves the resent frames are byte-identical to the first.
- [ ] Nothing in the client blocks longer than one response timeout, and the report says how the firmware's main task schedules it.
- [ ] The UART's pins are only driven between `uartEnable()` and `uartRelease()` (show it from the code's structure).

## Not verified here (Thomas, with the rig)

Add to `docs/bench.md`, "3. The link":

- With the repeater firmware on the RAK: `rak hello` prints its version, role
  and capabilities; `rak ping` answers; `rak cli get radio`; `rak power`.
- Reset the RAK by its button: `rak events` shows the boot, and the RAK's clock
  is set straight afterwards (`rak time`).
- With NoiseScope on the RAK: `rak hello` says *scanner (text)* and `rak text`
  shows its `stat` lines.
- Through both bulkhead filters, an hour of `rak ping` once a second: no
  retries counted.

## Out of scope

Acting on a dead link (phase 2, the supervisor). The agent.
