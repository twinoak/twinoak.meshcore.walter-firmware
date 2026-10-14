#include "core/update/link_battery.h"

namespace core {
namespace update {

bool LinkBattery::read(BatteryReading* out) {
  if (out == nullptr) return false;
  // §4: POWER_GET needs the POWER capability, which HELLO brings; without a session the client
  // answers kNoSession and nothing goes on the wire - a session is the caller's to have made.
  const link::client::Answer<link::PowerGetResponse> answer = radio_.powerGet();
  if (!answer.ok()) return false;
  // §5.8: channel 2 is the battery (positive current = charging); its valid bit is bit 1.
  if ((answer.message.valid & 0x02u) == 0) return false;
  out->millivolts = answer.message.channels[1].bus_mv;
  out->trend_known = false;
  out->mv_per_hour = 0;
  return true;
}

}  // namespace update
}  // namespace core
