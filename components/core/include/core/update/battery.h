// What the radio's update asks of the battery (task p2-03): one reading, in millivolts, and the
// trend if anybody knows it. The Walter has no gauge of its own - the INA3221 is on the radio's
// I2C (spec/SYSTEM.md §2) and its battery channel comes over the link's POWER_GET (§5.8) - so the
// firmware's implementation asks the radio (core/update/link_battery.h) and knows no trend;
// phase 3's power component keeps a ring of readings and will answer with one. The tests hand
// over whatever reading they want.
#pragma once

#include <cstdint>

namespace core {
namespace update {

struct BatteryReading {
  std::uint16_t millivolts = 0;
  bool trend_known = false;
  std::int16_t mv_per_hour = 0;  // negative: falling
};

class BatterySource {
 public:
  virtual ~BatterySource() = default;
  // False when no reading can be had now (the link is down, the channel did not read).
  virtual bool read(BatteryReading* out) = 0;
};

}  // namespace update
}  // namespace core
