// components/agent/ allocates nothing and includes only what it is written against (task p4-02):
// the same check core/ is held to (checks/no_allocation.h), over the mesh client, the uplink
// (task p4-03) and the console's two probes. The library's headers it may include are the ones it
// is an implementation of, reads replies with, writes bodies with or logs through; nothing of
// ESP-IDF, nothing of the standard library but the freestanding headers and <cstdio>/<cstring>
// for snprintf and memcpy (tools/cmake/no_idf_include.cmake holds the first half of that at
// build time too, for the firmware build and the host build alike).
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
    "cstdarg",
    "cstdio",
    "cstring",
    "agent/AgentLoop.h",
    "agent/Airtime.h",
    "agent/ConfigApply.h",
    "agent/ConfigRead.h",
    "agent/ConfigReply.h",
    "agent/Credentials.h",
    "agent/Envelopes.h",
    "agent/IJobExtension.h",
    "agent/IMesh.h",
    "agent/IPlatform.h",
    "agent/IUplink.h",
    "agent/JobEnvelopes.h",
    "agent/Log.h",
    "agent/Observations.h",
    "agent/RateLimit.h",
    "agent/ReplyCodec.h",
    "core/console/agent_probe.h",
    "core/console/log_ring.h",
    "core/console/mesh_probe.h",
    "core/console/uplink_probe.h",
    "core/cycle/agent_attention.h",
    "core/cycle/duty_cycle.h",
    "core/cycle/network_clock.h",
    "core/link/client/client.h",
    "core/link/codec/messages.h",
    "core/link/codec/view.h",
    "core/link/codec/wire.h",
    "core/net/client.h",
    "core/power/interval_policy.h",
    "core/settings/settings.h",
    "interfaces/clock.h",
    "interfaces/entropy.h",
    "interfaces/heap.h",
    "interfaces/http.h",
    "interfaces/wall_clock.h",
    "walter_agent/agent_probe.h",
    "walter_agent/identity.h",
    "walter_agent/link_mesh.h",
    "walter_agent/observations.h",
    "walter_agent/mesh_probe.h",
    "walter_agent/modem_uplink.h",
    "walter_agent/uplink_probe.h",
    "walter_agent/walter_platform.h",
    // Task p4-05: the backend, the firmware jobs, the kept image's store, the trial's heartbeat,
    // the payload reader, and the library headers they are written against.
    "agent/AgentLoop.h",
    "agent/IRetained.h",
    "agent/JobEnvelopes.h",
    "agent/JobRunner.h",
    "agent/JsonWriter.h",
    "agent/Iso8601.h",
    "agent/TextOut.h",
    "agent/Version.h",
    "core/board/radio_pins.h",
    "core/link/client/words.h",
    "core/board/slot.h",
    "core/console/agent_loop_probe.h",
    "core/cycle/duty_cycle.h",
    "core/cycle/wake_cycle.h",
    "core/cycle/wake_guard.h",
    "core/image/descriptor.h",
    "core/image/store.h",
    "core/image/crypto/sha256.h",
    "core/power/interval_policy.h",
    "core/supervisor/supervisor.h",
    "core/update/fetch.h",
    "core/update/journal.h",
    "core/update/radio_update.h",
    "core/update/self_update.h",
    "core/update/uplink_check.h",
    "core/version.h",
    "interfaces/backend.h",
    "interfaces/flash_region.h",
    "interfaces/key_value.h",
    "interfaces/text_out.h",
    "walter_agent/agent_backend.h",
    "walter_agent/agent_uplink_check.h",
    "walter_agent/firmware_jobs.h",
    "walter_agent/json_scan.h",
    "walter_agent/region_store.h",
    // Task p13-01: the seam `twinoak.report` reads the console's own sections through.
    "core/console/report_source.h",
};

}  // namespace

TEST_CASE("components/agent/ allocates nothing and includes only what it is written against") {
  checks::checkNoAllocation("components/agent/",
                            checks::filesUnder(kAgentDirectories, sizeof kAgentDirectories / sizeof kAgentDirectories[0]),
                            kAgentIncludes, sizeof kAgentIncludes / sizeof kAgentIncludes[0], 16, 1600);
}
