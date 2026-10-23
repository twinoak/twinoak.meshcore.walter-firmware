// The self-update trial's third condition, as the agent proves it (task p4-05; spec/SYSTEM.md
// S-6: a new image is confirmed only after it has booted, spoken to the RAK and **reached the
// server**). Phase 3 stood in with a TLS handshake against the configured server
// (core/update/tls_check.h: the strongest statement available while nothing talked to
// MeshMaster). The agent talks to it now, so the condition is what the task asked for: **one
// `POST /agent/v1/heartbeat` answered `ok`** in this boot - the unit attached, the chain was
// verified, the token was taken and the server answered - which `agent::AgentBackend` reports
// here from the wake's step 8 (docs/ARCHITECTURE.md §10), before the wake's step 9 drives the
// trial to its end. `check()` makes no call of its own: the heartbeat is the wake's, once, and a
// trial that tried again every thirty seconds would be a second thing deciding when the modem
// speaks.
//
// The rule that keeps a factory image from rejecting itself is unchanged
// (core/update/uplink_check.h): with no `server.url` there is nothing to reach, the condition
// does not apply, and the trial confirms on the first two. A server configured and no heartbeat
// answered inside the trial window is a rollback, which is S-6 doing its job.
//
// Logic: no ESP-IDF include, no heap.
#pragma once

#include <cstdint>

#include "core/update/uplink_check.h"

namespace agent {

class AgentUplinkCheck final : public core::update::UplinkCheck {
 public:
  AgentUplinkCheck() = default;

  // The agent's wake posted a heartbeat the server answered `ok`: the condition is met for this
  // boot. Said by AgentBackend after each wake of the loop.
  void heartbeatOk() { reached_ = true; }
  bool reached() const { return reached_; }
  std::uint32_t checks() const { return checks_; }

  core::update::UplinkVerdict check() override {
    ++checks_;
    if (host_[0] == 0) return core::update::UplinkVerdict::kNotConfigured;
    return reached_ ? core::update::UplinkVerdict::kReached : core::update::UplinkVerdict::kFailed;
  }
  const char* serverHost() const override { return host_; }
  std::uint16_t serverPort() const override { return port_; }
  // `server.url` at the console (task p3-03): the settings' own buffer, alive as long as this is.
  void setServer(const char* host, std::uint16_t port) override {
    host_ = host != nullptr ? host : "";
    port_ = port;
  }

 private:
  const char* host_ = core::update::kSelfUpdateServerHost;
  std::uint16_t port_ = core::update::kSelfUpdateServerPort;
  bool reached_ = false;
  std::uint32_t checks_ = 0;
};

}  // namespace agent
