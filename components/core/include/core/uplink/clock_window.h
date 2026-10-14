// What time a time has to be before this firmware believes the network gave it (task p3-01).
//
// This is the time that sets the radio's clock (spec/LINK-PROTOCOL.md §5.4's `TIME_SET`, sent by
// core/link/client), and §5.4 is explicit about why a wrong one is worse than none: a MeshCore
// node whose clock goes backwards makes its own traffic look replayed. The modem answers
// `AT+CCLK?` with something whatever happens - 1970, or its own build date - until the network
// has told it otherwise, so a time is believed only inside a window:
//
//   not before   this firmware existed. A time from before the firmware that is reading it was
//                built cannot have come from a network that knows what day it is.
//   not after    this firmware plus a working life. A modem that answers 2089 is answering with
//                a default or with a parse that went wrong (WalterModem.cpp:2585 ignores the
//                return of its own sscanf over `+CCLK`, so a malformed report is used as if it
//                had parsed - reported in tasks/phase-3/reports/01.md).
//
// The library has a floor of its own - WALTER_MODEM_MIN_VALID_TIMESTAMP, 1672531200
// (2023-01-01), below which it answers -1 (WalterModem.cpp:140 and :2605) - and this one is
// above it on purpose: the library's floor is about the modem, this one is about this build.
#pragma once

#include <cstdint>

#include "interfaces/uplink.h"

namespace core {
namespace uplink {

// 2026-10-14T00:00:00Z, the day this task was written: this firmware's build date, as a
// constant. It only ever needs raising, and raising it only ever narrows the window - so a
// build that is years old still refuses 1970, which is the case that matters. TO BE SET ON THE
// BENCH is the wrong label for this one: it is to be raised by whichever later task gives the
// build a date of its own.
constexpr std::uint32_t kEarliestEpoch = 1791936000u;

// Twenty years after it. A unit on a Danish mast is not expected to outlive this, and a modem
// that says 2047 has not been told the time by anybody.
constexpr std::uint32_t kLatestEpoch = kEarliestEpoch + 20u * 365u * 86400u + 5u * 86400u;

static_assert(kEarliestEpoch < kLatestEpoch, "the window has to be a window");

// Where a time the modem gave can have come from. A time inside the window is the network's; one
// outside it, and the absence of one, are both "not a time" - and are told apart so that the
// console can say which happened.
inline interfaces::TimeOrigin judgeTime(bool answered, std::uint32_t epoch) {
  if (!answered) return interfaces::TimeOrigin::kNone;
  if (epoch < kEarliestEpoch || epoch > kLatestEpoch) return interfaces::TimeOrigin::kDefault;
  return interfaces::TimeOrigin::kNetwork;
}

}  // namespace uplink
}  // namespace core
