#include "core/uplink/failure.h"

namespace core {
namespace uplink {

namespace {

// The table of the header's comment, once, in the enum's order. Index 0 is kNone, so the enum's
// value is the index.
constexpr Row kRows[] = {
    {Failure::kNone, "none", "the uplink is up", Retry::kNotNeeded},
    {Failure::kModemSilent, "modem-silent", "no answer to the first command",
     Retry::kAfterModemReset},
    {Failure::kNoSim, "no-sim", "no card, or it does not answer", Retry::kNextWake},
    {Failure::kSimLocked, "sim-locked",
     "a PIN is wanted and none is set, or the one set was refused", Retry::kNever},
    {Failure::kNotRegistered, "not-registered",
     "searching, denied, or no network within the deadline", Retry::kGrowing},
    {Failure::kNoData, "no-data", "registered, no packet data", Retry::kGrowing},
};
static_assert(sizeof kRows / sizeof kRows[0] == kFailureCount + 1,
              "every Failure has a row, and only those");

}  // namespace

const Row& row(Failure failure) {
  const std::size_t index = static_cast<std::size_t>(failure);
  return kRows[index <= kFailureCount ? index : 0];
}

const char* describe(Failure failure) { return row(failure).name; }

const char* means(Failure failure) { return row(failure).means; }

const char* describe(Retry retry) {
  switch (retry) {
    case Retry::kNotNeeded: return "no need";
    case Retry::kAfterModemReset: return "after a modem reset, once per wake";
    case Retry::kNextWake: return "next wake; not sooner (a person has to act)";
    case Retry::kNever: return "never automatically";
    case Retry::kGrowing: return "growing: next wake, then every second, fourth ... up to a cap";
  }
  return "?";
}

std::uint32_t intervalAfter(Retry retry, std::uint32_t failures) {
  switch (retry) {
    case Retry::kNever:
      return 0;
    case Retry::kNotNeeded:
    case Retry::kAfterModemReset:
    case Retry::kNextWake:
      return 1;
    case Retry::kGrowing:
      break;
  }
  if (failures < 2) return 1;
  // 2 -> 2, 3 -> 4, 4 -> 8 ...: a doubling, stopped at the cap before the shift could overflow.
  std::uint32_t interval = 1;
  for (std::uint32_t i = 1; i < failures; ++i) {
    if (interval >= kBackoffCapWakes) return kBackoffCapWakes;
    interval *= 2;
  }
  return interval < kBackoffCapWakes ? interval : kBackoffCapWakes;
}

}  // namespace uplink
}  // namespace core
