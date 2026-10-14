// No allocation in the radio's update or its journal (task p2-03), as a test: every source under
// core/update/ - the journal, the machine, the battery over the link - read line by line for a
// word that allocates or an include that could bring one in (host_test/checks/no_allocation.h,
// task p1-01's check). The journal is one 64-byte buffer and a few words of state; the machine's
// buffers (a record, the store's view of an image) are members; the console holds the story's
// sixteen entries in static storage. That stays true here.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kUpdateDirectories[] = {
    "components/core/update",
    "components/core/include/core/update",
};

// What core/update/ may include: the freestanding headers, its own headers, the slot, the
// flasher and its board profile, the store and the descriptor (the record's copy of it is what
// the health check holds HELLO against), SHA-256's size, the link client and the protocol's
// numbers, and the two interfaces the journal and the machine need of the world - a flash
// region and a clock, both pure virtual.
const char* const kUpdateIncludes[] = {
    "cstddef",
    "cstdint",
    "core/board/slot.h",
    "core/flasher/board_profile.h",
    "core/flasher/flasher.h",
    "core/image/crypto/sha256.h",
    "core/image/descriptor.h",
    "core/image/store.h",
    "core/link/client/client.h",
    "core/link/codec/wire.h",
    "core/update/battery.h",
    "core/update/journal.h",
    "core/update/link_battery.h",
    "core/update/radio_update.h",
    "interfaces/clock.h",
    "interfaces/flash_region.h",
};

}  // namespace

TEST_CASE("nothing under components/core/update/ allocates") {
  // Seven files: the journal, the machine and the battery, each a header and a source, and the
  // battery interface.
  checks::checkNoAllocation("the radio's update",
                            checks::filesUnder(kUpdateDirectories,
                                               sizeof kUpdateDirectories / sizeof kUpdateDirectories[0]),
                            kUpdateIncludes, sizeof kUpdateIncludes / sizeof kUpdateIncludes[0], 7, 1200);
}
