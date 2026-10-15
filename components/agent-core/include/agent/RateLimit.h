// The agent policy's ceiling, kept on this side of the wire.
//
// The server's `agent` rate-limit policy is 120 requests a minute per token (API section 3, "as
// built in phase 2"), and every answer carries X-RateLimit-*. This firmware's own traffic is a
// heartbeat every 30 s and an observation batch every 60 s - four requests a minute at the most
// - so the limiter is not there to shape the normal loop but to make a bug that retried in a
// tight loop cost one wasted minute instead of a token's whole budget.
//
// An exact sliding window: the last 120 request times, and a request is allowed when the oldest
// of them is more than a minute old. 480 bytes, no arithmetic that can drift.
//
// Host-portable: millisecond counts in, nothing else.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

class RateLimiter {
 public:
  static constexpr size_t kMaxPerMinute = 120;
  static constexpr uint32_t kWindowMs = 60000;

  // 0 when a request may go now, else how long to wait for the window to make room.
  uint32_t waitMs(uint32_t nowMs) const;

  // Record a request that is about to go out.
  void took(uint32_t nowMs);

  // waitMs() == 0 and took() in one: true when the request may go, and then it is counted.
  bool take(uint32_t nowMs);

  uint32_t held() const { return held_; }
  size_t inWindow(uint32_t nowMs) const;

 private:
  uint32_t times_[kMaxPerMinute] = {};
  size_t first_ = 0;
  size_t count_ = 0;
  uint32_t held_ = 0;
};

}  // namespace mm
