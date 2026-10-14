// No allocation in the uplink's policy (task p3-01), as a test: every source under core/uplink/
// read line by line for a word that allocates or an include that could bring one in
// (host_test/checks/no_allocation.h, task p1-01's check). One UplinkPolicy is its retained
// record and its Status - a few hundred bytes - and lives in static storage, which is what
// main/ gives it; it runs in the wake that has the least battery to spare, so a heap is the
// last thing it should want.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kUplinkDirectories[] = {
    "components/core/uplink",
    "components/core/include/core/uplink",
};

// What core/uplink/ may include: the freestanding headers, its own four, and the three
// interfaces it needs of the world - a clock, retained storage for the backoff, and the uplink
// itself, each pure virtual. **No ESP-IDF and no WalterModem**: that is
// tools/cmake/no_idf_include.cmake's and tools/verify.sh's business as well, and this is the
// third lock on the same door.
const char* const kUplinkIncludes[] = {
    "cstddef",
    "cstdint",
    "core/uplink/clock_window.h",
    "core/uplink/failure.h",
    "core/uplink/policy.h",
    "core/uplink/psm.h",
    "interfaces/clock.h",
    "interfaces/retained.h",
    "interfaces/uplink.h",
};

}  // namespace

TEST_CASE("nothing under components/core/uplink/ allocates") {
  checks::checkNoAllocation(
      "the uplink's policy",
      checks::filesUnder(kUplinkDirectories, sizeof kUplinkDirectories / sizeof kUplinkDirectories[0]),
      kUplinkIncludes, sizeof kUplinkIncludes / sizeof kUplinkIncludes[0], 2, 900);
}
