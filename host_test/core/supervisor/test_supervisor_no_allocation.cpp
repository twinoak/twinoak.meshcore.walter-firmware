// No allocation in the supervisor (task p2-05), as a test: every source under core/supervisor/
// read line by line for a word that allocates or an include that could bring one in
// (host_test/checks/no_allocation.h, task p1-01's check). One Supervisor is a few hundred bytes
// of state - the day's counts, three incidents, four attentions - and belongs in static storage,
// which is what main/ gives it. That stays true here.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kSupervisorDirectories[] = {
    "components/core/supervisor",
    "components/core/include/core/supervisor",
};

// What core/supervisor/ may include: the freestanding headers, the pins and the slot (rungs 2
// and 3 are RadioPins', and nothing else may touch those wires), the flasher (rung 4's read-only
// verify), the store and the update machine (rung 4's reflash through the rollback path), the
// journal, the link client and the protocol's numbers, health (did this Walter just power up),
// the battery, and the three interfaces it needs of the world - a clock, a time of day and
// retained storage, each pure virtual.
const char* const kSupervisorIncludes[] = {
    "cstddef",
    "cstdint",
    "core/board/radio_pins.h",
    "core/board/slot.h",
    "core/flasher/flasher.h",
    "core/health/health.h",
    "core/image/store.h",
    "core/link/client/client.h",
    "core/link/codec/messages.h",
    "core/link/codec/wire.h",
    "core/supervisor/supervisor.h",
    "core/update/battery.h",
    "core/update/journal.h",
    "core/update/radio_update.h",
    "interfaces/clock.h",
    "interfaces/retained.h",
    "interfaces/wall_clock.h",
};

}  // namespace

TEST_CASE("nothing under components/core/supervisor/ allocates") {
  checks::checkNoAllocation(
      "the supervisor",
      checks::filesUnder(kSupervisorDirectories,
                         sizeof kSupervisorDirectories / sizeof kSupervisorDirectories[0]),
      kSupervisorIncludes, sizeof kSupervisorIncludes / sizeof kSupervisorIncludes[0], 2, 900);
}
