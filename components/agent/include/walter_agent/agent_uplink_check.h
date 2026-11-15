// The self-update trial's third condition, as the agent proves it (task p4-05; spec/SYSTEM.md
// S-6: a new image is confirmed only after it has booted, spoken to the RAK and **reached the
// server**). Phase 3 stood in with a TLS handshake against the configured server
// (core/update/tls_check.h: the strongest statement available while nothing talked to
// MeshMaster). The agent talks to it now, so the condition is what the task asked for: **one
// `POST /agent/v1/heartbeat` answered `ok`** in this boot - the unit attached, the chain was
// verified, the token was taken and the server answered - which `agent::AgentBackend` reports
// here from the wake's step 8 (docs/ARCHITECTURE.md §10), before the wake's step 9 drives the
// trial to its end.
//
// **`check()` still makes no call of its own, and the heartbeat is no longer only the wake's
// one** (task p14-05). Until 15 November 2026 it was: *the heartbeat is the wake's, once, and a
// trial that tried again every thirty seconds would be a second thing deciding when the modem
// speaks*. That held - and then v0.1.6 booted on trial with a wait an older image's failed call
// had left, its wake sent nothing, and the image rolled itself back for a server it had never
// been asked. One slow attach would have done the same. So the trial's own loop asks the seam for
// **one heartbeat and nothing else** while the condition is open (interfaces/backend.h, `Beat`;
// core/cycle/wake_cycle.cpp, `trialBeat`), the library's own wait is what paces it, and this
// class is still only the reading: `heartbeatOk` and `heartbeatFailed` are what `AgentBackend`
// says of every heartbeat of this boot, the wake's and the trial's alike, and `check()` answers
// what they left. A look that finds nothing new is `kNotTried` - **not** a failure, because
// *the server was not reached* and *nothing was ever sent* are two findings and the second is
// the one v0.1.6 was condemned by.
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
  void heartbeatOk() {
    reached_ = true;
    ++beats_;
    fresh_ = true;
  }
  // ... and one that went out and did not come back (task p14-05). Not the condition failing -
  // the window is not over - but news, so that the next `check()` says *the server was not
  // reached* rather than *nothing was sent*.
  void heartbeatFailed() {
    ++beats_;
    fresh_ = true;
  }
  bool reached() const { return reached_; }
  std::uint32_t checks() const { return checks_; }
  // How many heartbeats have gone out in this boot, answered or not.
  std::uint32_t beats() const { return beats_; }

  core::update::UplinkVerdict check() override {
    ++checks_;
    if (host_[0] == 0) return core::update::UplinkVerdict::kNotConfigured;
    if (reached_) return core::update::UplinkVerdict::kReached;
    // Nothing has gone out since the last look: there is nothing to report, and reporting a
    // failure would spend the trial's one word on a call nobody made (task p14-05).
    if (!fresh_) return core::update::UplinkVerdict::kNotTried;
    fresh_ = false;
    return core::update::UplinkVerdict::kFailed;
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
  // A heartbeat has gone out since the last `check()`: what makes a failure news once and not at
  // every turn of the trial's loop (task p14-05).
  bool fresh_ = false;
  std::uint32_t checks_ = 0;
  std::uint32_t beats_ = 0;
};

}  // namespace agent
