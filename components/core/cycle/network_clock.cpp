#include "core/cycle/network_clock.h"

#include "core/link/codec/wire.h"

namespace core {
namespace cycle {

bool NetworkClock::give(const interfaces::NetworkTime& time) {
  if (time.origin != interfaces::TimeOrigin::kNetwork) return false;
  return give(time.epoch);
}

bool NetworkClock::give(std::uint32_t epoch) {
  return take(epoch, link::time_source::kCellular);
}

// The server's, and only while the network has given none: the rule is in the header.
bool NetworkClock::giveFromServer(std::uint32_t epoch) {
  if (known_ && source_ == link::time_source::kCellular) return false;
  return take(epoch, link::time_source::kServer);
}

bool NetworkClock::take(std::uint32_t epoch, std::uint8_t source) {
  // The same window the uplink judges by, applied again here: this object is reachable from the
  // console, from a test and now from the agent's server, and a time that would not have been
  // believed on the way in must not be believable on the way round.
  if (uplink::judgeTime(true, epoch) != interfaces::TimeOrigin::kNetwork) return false;
  epoch_ = epoch;
  taken_ms_ = clock_.nowMs();
  source_ = source;
  known_ = true;
  return true;
}

bool NetworkClock::now(interfaces::WallTime* out) {
  if (!known_ || out == nullptr) return false;
  out->epoch = epoch_ + static_cast<std::uint32_t>((clock_.nowMs() - taken_ms_) / 1000u);
  out->source = source_;
  return true;
}

std::uint32_t NetworkClock::ageMs() const {
  if (!known_) return 0;
  return static_cast<std::uint32_t>(clock_.nowMs() - taken_ms_);
}

void NetworkClock::forget() {
  known_ = false;
  epoch_ = 0;
  taken_ms_ = 0;
  source_ = 0;
}

}  // namespace cycle
}  // namespace core
