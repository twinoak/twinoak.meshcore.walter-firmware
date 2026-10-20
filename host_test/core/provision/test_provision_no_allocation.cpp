// No allocation in the sequence (task p5-03), as a test: every source under core/provision/ read
// for a word that allocates or an include that could bring one in (host_test/checks/
// no_allocation.h). One Provision is ~11 KB - the base's reader, its 4 KB window, two 1 KB run
// buffers - and belongs in static storage, which the console is.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kProvisionDirectories[] = {
    "components/core/provision",
    "components/core/include/core/provision",
};

// What core/provision/ may include: the freestanding headers and cstdio (the line to type is
// formatted with snprintf into a fixed buffer); the pins and the slot; the base's reader and
// the store; the link client; the nRF52 layer, the door and the grant (rail R3b: this is the
// door's one caller); the board profile; the supervisor (told that the slot awaits its first
// install); the battery and the update machine (step 1 refuses while either holds the radio);
// the debug port and the wire; and the clock.
const char* const kProvisionIncludes[] = {
    "cstddef",
    "cstdint",
    "cstdio",
    "core/board/radio_pins.h",
    "core/board/slot.h",
    "core/flasher/board_profile.h",
    "core/image/base.h",
    "core/image/store.h",
    "core/link/client/client.h",
    "core/nrf52/nrf52.h",
    "core/nrf52/provision_door.h",
    "core/nrf52/provision_grant.h",
    "core/provision/provision.h",
    "core/provision/variant.h",
    "core/supervisor/supervisor.h",
    "core/swd/dp.h",
    "core/swd/wire.h",
    "core/update/battery.h",
    "core/update/radio_update.h",
    "interfaces/clock.h",
};

}  // namespace

TEST_CASE("nothing under components/core/provision/ allocates") {
  checks::checkNoAllocation(
      "the sequence",
      checks::filesUnder(kProvisionDirectories,
                         sizeof kProvisionDirectories / sizeof kProvisionDirectories[0]),
      kProvisionIncludes, sizeof kProvisionIncludes / sizeof kProvisionIncludes[0], 4, 900);
}
