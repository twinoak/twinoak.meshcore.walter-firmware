#include "core/update/last_good_battery.h"

namespace core {
namespace update {

bool LastGoodBattery::read(BatteryReading* out) {
  BatteryReading live;
  if (under_.read(&live)) {
    kept_ = live;
    known_ = true;
    stale_ = false;
    taken_at_ms_ = clock_.nowMs();
    ++live_reads_;
    if (out != nullptr) *out = live;
    return true;
  }
  // No live reading. The kept one, while it is young enough to mean anything. The clock can
  // restart at zero (a deep sleep: interfaces/clock.h), which makes `now` smaller than the time
  // the reading was taken; that is a new epoch and the reading's age cannot be measured, so it
  // is not used - the same "assume the worst" the pins' reset spacing takes.
  const std::uint64_t now = clock_.nowMs();
  if (!known_ || now < taken_at_ms_ || now - taken_at_ms_ > kBatteryStaleAfterMs) {
    ++misses_;
    stale_ = false;
    return false;
  }
  stale_ = true;
  ++stale_reads_;
  if (out != nullptr) *out = kept_;
  return true;
}

std::uint32_t LastGoodBattery::ageMs() const {
  if (!known_) return 0;
  const std::uint64_t now = clock_.nowMs();
  if (now < taken_at_ms_) return 0;
  const std::uint64_t age = now - taken_at_ms_;
  return age > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(age);
}

void LastGoodBattery::forget() {
  known_ = false;
  stale_ = false;
  taken_at_ms_ = 0;
  kept_ = BatteryReading{};
}

}  // namespace update
}  // namespace core
