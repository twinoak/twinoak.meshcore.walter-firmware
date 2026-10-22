// The one backoff this firmware retries with: a doubling sequence, and jitter on the wait.
//
// Two things back off - the WiFi link (src/net/WifiLink.cpp) and the server loop
// (lib/agent-core/src/AgentLoop.cpp, through agent/ServerPolicy.h) - and before task 06 they doubled the
// same way with no jitter at all. A board is one of many on a router and one of many exit nodes
// on meshmaster.dk: after a power cut or a server restart every one of them wakes at the same
// second and retries on the same schedule, which is the thundering herd the spec's own agent
// avoids by never having a fixed schedule in the first place. The sequence stays the readable
// one - 30 s, 60 s, 120 s ... - and what is *waited* is that number plus or minus a quarter, so
// two boards that failed together drift apart by the second attempt.
//
// Host-portable: no Arduino. The randomness is the caller's (esp_random() on the board, a
// counter in the tests), so the function is pure and test/test_buffers can hold the whole
// sequence and the bounds of the jitter.

#pragma once

#include <cstdint>

namespace mm {

// How far either side of the base the wait may land, as a percentage of the base.
constexpr uint32_t kJitterPercent = 25;

// The next step of a doubling sequence: `firstMs` when `previousMs` is 0, then twice the last,
// never above `maxMs`. The sequence is the ceiling and carries no jitter - `withJitter` does.
uint32_t nextBackoffMs(uint32_t previousMs, uint32_t firstMs, uint32_t maxMs);

// `ms` moved by up to kJitterPercent either way, from `random` (any value; only the low bits are
// read). 0 in is 0 out - "no wait" is not something to jitter - and the result is always within
// [ms - ms*p/100, ms + ms*p/100].
uint32_t withJitter(uint32_t ms, uint32_t random);

}  // namespace mm
