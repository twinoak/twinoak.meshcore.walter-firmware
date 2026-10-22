// `rak mesh status <key>` behind the console's seam (task p4-02): one status request to a
// neighbour through LinkMesh - begin a job, send, await, read the repeater's stats, end the job -
// and `client_tx` read before and after it. What the bench holds against the air: one packet, one
// reply, `client_tx` up by one (docs/bench.md, *The agent*).
#pragma once

#include <cstdint>

#include "core/console/mesh_probe.h"
#include "walter_agent/link_mesh.h"

namespace agent {

class LinkMeshProbe final : public core::console::MeshProbe {
 public:
  explicit LinkMeshProbe(LinkMesh& mesh) : mesh_(mesh) {}

  bool status(const char* keyHex, std::uint32_t timeout_ms, core::console::MeshProbeResult* out) override;

 private:
  LinkMesh& mesh_;
};

}  // namespace agent
