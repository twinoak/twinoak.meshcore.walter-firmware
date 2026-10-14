// The battery as the radio reports it (task p2-03): POWER_GET's channel 2 - the battery, in
// millivolts, the shunt already applied by the radio (spec/LINK-PROTOCOL.md §5.8; the standard
// B-board's channel order, hardware/pcb/CURRENT-SENSE.md). No trend: one reading has none, and
// phase 3's power component is where a history lives. False when the link is down, the radio
// does not advertise POWER, or the channel's valid bit is clear.
#pragma once

#include "core/link/client/client.h"
#include "core/update/battery.h"

namespace core {
namespace update {

class LinkBattery final : public BatterySource {
 public:
  explicit LinkBattery(link::client::Client& radio) : radio_(radio) {}
  bool read(BatteryReading* out) override;

 private:
  link::client::Client& radio_;
};

}  // namespace update
}  // namespace core
