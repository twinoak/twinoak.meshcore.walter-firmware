// What one answer from meshmaster.dk means for the loop, as a pure function of the status code
// and the Retry-After it carried.
//
// The rules are EXIT-NODES §6 and API §3 as the .NET agent reads them (ServerAnswer in
// Uplink.cs), and tasks/phase-1/02 names three of them outright: a 401 stops everything but the
// console, a 429 honours Retry-After, a 5xx backs off. They live here rather than inside
// lib/agent-core/src/AgentLoop.cpp so that test/test_server_policy can hold every status code against the
// action it earns without a board.
//
// Task p1.2-10 corrected two of them against the first day on the air. A release of
// meshmaster.dk answers 503 for a minute or two, and five of those cost the board 480 s of
// doubling with the server long since back - the exit node read *quiet* while nothing was wrong
// (bench, 3 October 2026). So: a **5xx that says when to come back is believed**, exactly as the
// 429 is, and the doubling stops at **two minutes once the board's own link is proven**, because
// then the thing that is not answering is the server and a server that comes back deserves a
// call within minutes.

#pragma once

#include <cstdint>

namespace mm {

enum class ServerAction : uint8_t {
  Ok,              // 2xx: take what the reply says and reset the backoff
  Halt,            // 401: the token is revoked or wrong, and retrying fixes neither
  WaitRetryAfter,  // 429, or a 5xx that carried Retry-After: the wait is the answer's own
  Discard,         // a 4xx that will not get better: what was being sent is dropped, not retried
  BackOff,         // 5xx, or no reply at all: try again later, more slowly each time
};

// `reachable` is false when no reply arrived at all - DNS, TCP, the TLS handshake, a timeout -
// and then `httpStatus` is not read. `retryAfterS` is the answer's Retry-After in seconds
// (IUplink::Call::retryAfterS), 0 when it carried none this client could parse: it is read
// for the statuses of `saysWhenToComeBack` below and ignored for every other.
ServerAction actionFor(bool reachable, int httpStatus, uint32_t retryAfterS);

// The statuses whose Retry-After is believed: the 429 - whose wait the header *is* - and the
// 502, 503 and 504 a deploy or a restart of meshmaster.dk answers, which say how long they will
// be. Nothing else: a 500 is a bug and not a schedule, and a 4xx has nowhere to come back to.
bool saysWhenToComeBack(int httpStatus);

// The wait such an answer earns: its Retry-After in milliseconds, bounded at an hour, and one
// minute when the header was absent or was an HTTP date rather than a count of seconds.
uint32_t retryAfterMs(uint32_t retryAfterS);

// The ceiling the doubling stops at. `linkProven` is this board's own link having been shown to
// work - WiFi associated *and* SNTP answered (task p1.2-10) - which is what tells a server that
// is away from a WiFi that associates and goes nowhere, a captive portal, a DNS that answers
// nothing. The proven link waits two minutes at most; the unproven one ten, as before.
uint32_t maxBackoffMs(bool linkProven);

// The wait a failure earns: half a minute, doubling, never more than `maxBackoffMs(linkProven)`.
// 0 in means "the first failure since the last answer". This is the sequence, without jitter;
// what the loop actually waits is this through `withJitter` (agent/Backoff.h), so a hundred boards
// that lost the same server do not come back at the same second (task 06).
uint32_t nextBackoffMs(uint32_t previousMs, bool linkProven);

constexpr uint32_t kFirstBackoffMs = 30000;
constexpr uint32_t kMaxBackoffMs = 10 * 60000;
constexpr uint32_t kMaxBackoffWhenLinkUpMs = 2 * 60000;
constexpr uint32_t kRetryAfterWhenUnstatedMs = 60000;
constexpr uint32_t kMaxRetryAfterMs = 60 * 60000;

}  // namespace mm
