// interfaces::WallClock on the host: a Walter that knows the time only when a test says so, which
// is also the firmware's own state until phase 3 attaches to a network (main/main.cpp).
#pragma once

#include <cstdint>

#include "core/link/codec/wire.h"
#include "interfaces/wall_clock.h"

namespace fakes {

class FakeWallClock final : public interfaces::WallClock {
 public:
  bool known = false;
  interfaces::WallTime when{};
  int asked = 0;

  bool now(interfaces::WallTime* out) override {
    ++asked;
    if (!known || out == nullptr) {
      return false;
    }
    *out = when;
    return true;
  }

  void give(std::uint32_t epoch,
            std::uint8_t source = core::link::time_source::kCellular) {
    known = true;
    when.epoch = epoch;
    when.source = source;
  }

  void forget() { known = false; }
};

}  // namespace fakes
