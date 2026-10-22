// `agent url` and `agent probe` behind the console's seam (task p4-03): the configured server
// and the version header as ModemUplink has them, and one `GET /agent/v1/config` through it -
// the bench's first look at a server (docs/bench.md §17: a 200 and the config printed with a
// token from `/exit-nodes/new`, a 401 with a wrong one, a TLS failure against a host whose
// chain this firmware does not carry). The token is read for the call and wiped, as for every
// call; nothing of it reaches the result.
#pragma once

#include "core/console/uplink_probe.h"
#include "walter_agent/modem_uplink.h"

namespace agent {

class ModemUplinkProbe final : public core::console::UplinkProbe {
 public:
  explicit ModemUplinkProbe(ModemUplink& uplink) : uplink_(uplink) {}

  bool configured() const override { return uplink_.configured(); }
  bool tokenSet() const override { return uplink_.tokenSet(); }
  const char* url() override { return uplink_.url(); }
  const char* version() const override { return uplink_.version(); }
  bool probe(core::console::UplinkProbeResult* out) override;

 private:
  ModemUplink& uplink_;
};

}  // namespace agent
