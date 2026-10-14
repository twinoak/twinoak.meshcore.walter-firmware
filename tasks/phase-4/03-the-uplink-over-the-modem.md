# 03 — The uplink over the modem

**Depends on:** 01.
**Spec:** the exit-node firmware at the pinned commit: `docs/library.md` (*IUplink*, the seven calls and the `Call` record), `lib/agent-core/include/agent/IUplink.h`, `src/agent/AgentClient.cpp` (the reference implementation's behaviour: one request per call, the `X-Agent-Version` header, `Retry-After` and `X-RateLimit-Remaining` read, the body bounded, the token read per call and wiped), `test/support/FakeUplink.*`; meshmaster.dk at today's `master`: `spec/API.md` §3 (every route the seven calls hit, the status codes, the rate limit) and `spec/EXIT-NODES.md` §6; this repository's `components/core/net/Client` (phase 3, task 02: `https://` only, redirects bounded, the token never in a URL or a log), `components/core/settings/` (`server.url`, `server.token`), `host_test/sim/FakeHttp`; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-12.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

`IUplink` is the agent's seven HTTPS calls — config, heartbeat,
observations, claim, result, extend, progress — each returning a `Call` the
server policy reads. The reference implementation is WiFi and `HTTPClient`;
this one is phase 3's client over the modem. The token crosses in a header,
is read from settings for the length of one call and wiped, and appears in
no log, no URL and no result — rails the host tests prove with the bytes.

## Deliverables

- **`components/agent/ModemUplink : IUplink`** over `net::Client`:
  - the seven calls, each one request to the route API §3 names under the
    configured `server.url`, with `Authorization: Bearer <token>`,
    `X-Agent-Version: <this firmware's version>` (the reference agent sends
    its own; this unit sends the Walter's, and the heartbeat body says the
    board — read what 4.14's 120 decided a `native` agent reports and follow
    it), `Content-Type: application/json`, the body from the library's
    writers (the bytes the fixtures expect — the library produces them; this
    class only carries them);
  - the `Call` record filled as `AgentClient` fills it: `reachable`, `ok`,
    `httpStatus`, `retryAfterS` from `Retry-After` (seconds or a date — the
    library's parser; find it), `rateLimitRemaining`, `error` text (never a
    body), `atEpoch`/`atMillis` from the platform, `via` = `"lte"`;
  - bodies **bounded** by the library's buffers; a response longer than the
    bound is cut and the call is `ok = false` with a reason;
  - the token: read from `server.token` into a stack buffer at the start of
    the call, placed in the header, **wiped** after the request is sent
    (the client's header buffer too — extend `net::Client` with a
    sensitive-header path as 02 extended the link client, and a test that
    reads the buffers back);
  - deadlines: each call takes the wake's remaining uplink budget as its
    deadline (phase 3's budget); a call that cannot finish is `reachable =
    false` with *budget*.
- **What a status means**, as the library's `ServerPolicy` expects and
  `AgentClient` does: 2xx `ok`; 401 `ok = false` with the status (the policy
  halts the loop — *the token is wrong*; 04 raises attention so a person
  hears of it); 429 and 5xx with `retryAfterS`; a TLS failure or no route is
  `reachable = false`. Say in the report where the library's policy and the
  reference implementation disagree, if they do.
- **No redirect is followed on agent routes** (the reference agent follows
  none; a redirect of `/agent/v1/…` is `ok = false` — the unit's server is
  one host). `https://` only is the client's rule already; assert it holds
  for `server.url`.
- **The console**: `agent url` (the configured server, the version header
  it sends), `agent probe` (a `GET /agent/v1/config` now, printing the
  `Call` — the bench's first look at a server; the token is not printed).
- **Tests (host)** against `FakeHttp` scripted with the contract fixtures'
  bytes as replies (the copied `tests/fixtures/contracts/`):
  - each of the seven calls: the right route, method, headers (the token
    present, the version header present), the body equal to the fixture
    where the library writes one, the reply parsed by the library's parser
    into the record the fixture describes;
  - 401, 429 with `Retry-After` in both forms, 503, a TLS failure, no route,
    a body one byte over the bound, a redirect — each `Call` as stated;
  - the token's bytes zero in every buffer after each call (read back); the
    log capture and every `Call.error` searched for the token and the URL
    searched for it;
  - a deadline that cannot be met: `reachable = false`, *budget*, the
    modem taken down cleanly by the caller.

## Acceptance criteria

- [ ] All seven calls implemented and tested against the fixtures' bytes; the report maps each to its route and the status handling to `ServerPolicy`'s expectations.
- [ ] The token is read per call, wiped after, and found in no buffer, log line, error text or URL (tests).
- [ ] The library's `test_loop` suite (copied in 01) passes with `ModemUplink` + `FakeHttp` substituted for `FakeUplink` — the loop does not know which uplink it has.
- [ ] `tools/verify.ps1` green, the TLS rail included.

## Not verified here (Thomas, with a SIM)

Add to `docs/bench.md`, "15. The agent": `agent probe` against meshmaster.dk
with a token from `/exit-nodes/new` → `200` and the config printed; with a
wrong token → `401`; with the URL of a host whose chain this firmware does
not carry → a TLS failure, not a connection.

## Out of scope

Deciding when to call (05). The heartbeat's content (04).
