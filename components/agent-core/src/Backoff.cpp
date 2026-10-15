#include <agent/Backoff.h>

namespace mm {

uint32_t nextBackoffMs(uint32_t previousMs, uint32_t firstMs, uint32_t maxMs) {
  if (maxMs < firstMs) maxMs = firstMs;
  if (previousMs == 0) return firstMs > maxMs ? maxMs : firstMs;
  if (previousMs >= maxMs) return maxMs;
  const uint32_t doubled = previousMs > maxMs / 2 ? maxMs : previousMs * 2;
  return doubled;
}

uint32_t withJitter(uint32_t ms, uint32_t random) {
  if (ms == 0) return 0;
  // The span is [-p%, +p%] of the base, inclusive at both ends, so the arithmetic is on the
  // width (2p% + 1 values) and the base moves down by p% first. Everything is done in 64 bits
  // because ms*2p can be larger than 32 bits for a wait of hours.
  const uint64_t base = ms;
  const uint64_t swing = base * kJitterPercent / 100u;
  if (swing == 0) return ms;
  const uint64_t width = swing * 2u + 1u;
  const uint64_t offset = static_cast<uint64_t>(random) % width;
  return static_cast<uint32_t>(base - swing + offset);
}

}  // namespace mm
