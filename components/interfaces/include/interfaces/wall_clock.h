// The time of day, as core/ sees it (task p1-02): Unix seconds and where they came from, or
// "I have no time to give". Separate from interfaces::Clock, which is a monotonic millisecond
// counter and says nothing about what day it is.
//
// It exists because of spec/LINK-PROTOCOL.md §5.4: the first-batch boards have no RTC fitted, so
// the radio's clock is lost at every reset and the Walter sets it after every restart it sees,
// before anything else it asks the radio to do. The link client (core/link/client) asks this
// interface for a time at that moment. Phase 3 fills it from the cellular network; until then the
// implementation in main/ answers false, which is correct - a Walter that does not know the time
// must not tell the radio one.
#pragma once

#include <cstdint>

namespace interfaces {

struct WallTime {
  std::uint32_t epoch = 0;  // Unix seconds
  // Where it came from, as the link's TIME_SET carries it (spec/LINK-PROTOCOL.md §5.4,
  // core::link::time_source): 1 the cellular network, 2 the server, 3 a person at a console.
  // A raw byte and not that header's constant: interfaces/ knows nothing of the link.
  std::uint8_t source = 0;
};

class WallClock {
 public:
  virtual ~WallClock() = default;

  // The time now, with its source. False, and `out` untouched, when this firmware does not know
  // what time it is - which is the normal state of a Walter that has not attached yet.
  virtual bool now(WallTime* out) = 0;
};

}  // namespace interfaces
