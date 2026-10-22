#include <agent/RateLimit.h>

namespace mm {

namespace {

// millis() wraps every 49.7 days; a signed difference is how every comparison in this firmware
// reads a time that may have wrapped.
bool notAfter(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }

}  // namespace

size_t RateLimiter::inWindow(uint32_t nowMs) const {
  size_t n = 0;
  for (size_t i = 0; i < count_; i++) {
    const uint32_t t = times_[(first_ + i) % kMaxPerMinute];
    if (!notAfter(t + kWindowMs, nowMs)) n++;
  }
  return n;
}

uint32_t RateLimiter::waitMs(uint32_t nowMs) const {
  if (count_ < kMaxPerMinute) return 0;
  const uint32_t oldest = times_[first_];
  if (notAfter(oldest + kWindowMs, nowMs)) return 0;
  return (oldest + kWindowMs) - nowMs;
}

void RateLimiter::took(uint32_t nowMs) {
  // Drop everything that has fallen out of the window, so `first_` is always the oldest that
  // still counts and waitMs() is one comparison.
  while (count_ > 0 && notAfter(times_[first_] + kWindowMs, nowMs)) {
    first_ = (first_ + 1) % kMaxPerMinute;
    count_--;
  }
  if (count_ == kMaxPerMinute) {  // should not happen: take() asked first
    first_ = (first_ + 1) % kMaxPerMinute;
    count_--;
  }
  times_[(first_ + count_) % kMaxPerMinute] = nowMs;
  count_++;
}

bool RateLimiter::take(uint32_t nowMs) {
  if (waitMs(nowMs) != 0) {
    held_++;
    return false;
  }
  took(nowMs);
  return true;
}

}  // namespace mm
