// components/agent/ allocates nothing and includes only what it is written against (task p4-02):
// the same check core/ is held to (checks/no_allocation.h), over the mesh client and the
// console's probe. The library's headers it may include are the three it is an implementation
// of or reads replies with; nothing of ESP-IDF, nothing of the standard library but the two
// freestanding headers (tools/cmake/no_idf_include.cmake holds the first half of that at build
// time too, for the firmware build and the host build alike).
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kAgentDirectories[] = {
    "components/agent",
    "components/agent/include/walter_agent",
};

const char* const kAgentIncludes[] = {
    "cstddef",
    "cstdint",
    "agent/IMesh.h",
    "agent/ReplyCodec.h",
    "core/console/mesh_probe.h",
    "core/link/client/client.h",
    "core/link/codec/messages.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "interfaces/clock.h",
    "walter_agent/link_mesh.h",
    "walter_agent/mesh_probe.h",
};

}  // namespace

TEST_CASE("components/agent/ allocates nothing and includes only what it is written against") {
  checks::checkNoAllocation("components/agent/",
                            checks::filesUnder(kAgentDirectories, sizeof kAgentDirectories / sizeof kAgentDirectories[0]),
                            kAgentIncludes, sizeof kAgentIncludes / sizeof kAgentIncludes[0], 4, 400);
}
