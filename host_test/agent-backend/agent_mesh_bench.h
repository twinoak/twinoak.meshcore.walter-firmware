// The agent on the wake bench with a repeater on the air (task p4-05): the library's own
// simulated stock repeater (host_test/agent-core/test/support/FakeMesh.h) behind the RAK of §6,
// its clock tied to the world's, so that a job for a neighbour goes the whole road - the runner,
// LinkMesh, the UART framing, the RAK, the repeater - inside a wake. GoogleTest, because the
// repeater is.
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <agent/Credentials.h>
#include <agent/IMesh.h>
#include <agent/JobEnvelopes.h>

#include <support/FakeMesh.h>
#include <support/FakePlatform.h>
#include <support/Jobs.h>

#include "agent/agent_bench.h"
#include "fakes/fake_clock.h"

namespace bench {

// The library's fake platform, whose clock is the world's: its waits move the world's clock and
// the world's moves it (the shim of host_test/shim/ does the same for a clock of its own).
class WorldPlatform final : public fake::FakePlatform {
 public:
  explicit WorldPlatform(fakes::FakeClock& world) : world_(world) {}
  void delayMs(uint32_t ms) override {
    delays_++;
    world_.delayMs(ms == 0 ? 1 : ms);  // the world's hook advances this platform too
  }
  void tick(uint32_t ms) { fake::FakePlatform::advance(ms); }
  uint32_t delays() const { return delays_; }

 private:
  fakes::FakeClock& world_;
  uint32_t delays_ = 0;
};

// The repeater a neighbour is, and the unit's own repeater's console for the local door.
struct Neighbourhood {
  WorldPlatform platform;
  fake::FakeMesh repeater;

  explicit Neighbourhood(fakes::FakeClock& world) : platform(world), repeater(platform) {
    repeater.setKey(fake::repeaterKey());
    repeater.setSelfKey(kOwnKeyHex);
    repeater.setAdminPassword("adm1n-pass");
    repeater.grant(kOwnKeyHex, mm::kPermReadOnly);
    platform.fakeClock().setFromNetwork(1791936000u);
  }
};

// A `status` on the neighbour at the class the grant gives.
inline fake::Job neighbourStatus(long long id) {
  return fake::Job(id, "status")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{\"version\":false}")
      .asDirectNeighbour();
}
inline fake::Job neighbourTelemetry(long long id) {
  return fake::Job(id, "telemetry")
      .forRepeater(18, "Grenaa Chimney #1", fake::repeaterKey())
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{}")
      .asDirectNeighbour();
}
// The own repeater's probes, over the local door: a telemetry at class `acl`, and a config.read
// at class admin with the password the local door never sends anywhere.
inline fake::Job ownTelemetry(long long id) {
  return fake::Job(id, "telemetry")
      .forRepeater(17, kOwnName, kOwnKeyHex)
      .atClass("acl")
      .leasedUntil(0)
      .withAclLevel(mm::kPermReadOnly)
      .withPayload("{}")
      .asDirectNeighbour();
}
inline fake::Job ownConfigRead(long long id) {
  return fake::Job(id, "config.read")
      .forRepeater(17, kOwnName, kOwnKeyHex)
      .atClass("admin")
      .leasedUntil(0)
      .withAdminPassword("adm1n-pass")
      .withPayload("{\"keys\":[\"name\",\"advert.interval\",\"flood.max\"]}")
      .asDirectNeighbour();
}

// The unit's own repeater's console, as the local door reaches it (§5.7): what a stock repeater
// answers `get`, `set`, `ver`, `board`, `region`.
inline bool ownConsole(const std::string& line, std::string* reply) {
  static std::map<std::string, std::string> config = {
      {"name", "TwinOak Risskov"}, {"advert.interval", "240"}, {"flood.max", "64"}, {"freq", "869.618"},
      {"radio", "869.618,62.5,8,8"}, {"lat", "56.4167"}, {"lon", "10.8833"}, {"repeat", "on"},
  };
  if (line.rfind("reboot", 0) == 0) return false;
  if (line.rfind("get ", 0) == 0) {
    const auto it = config.find(line.substr(4));
    *reply = it == config.end() ? "??: " + line.substr(4) : "> " + it->second;
    return true;
  }
  if (line.rfind("set ", 0) == 0) {
    const std::size_t sp = line.find(' ', 4);
    if (sp != std::string::npos) config[line.substr(4, sp - 4)] = line.substr(sp + 1);
    *reply = "OK";
    return true;
  }
  if (line.rfind("setperm ", 0) == 0) {
    *reply = "OK";
    return true;
  }
  if (line == "ver") {
    *reply = "twinoak-repeater 0.3.0 on MeshCore repeater-v1.17.1 (d9296435)";
    return true;
  }
  if (line == "board") {
    *reply = "rak4630-single";
    return true;
  }
  if (line == "region") {
    *reply = "* F\n dk F\n";
    return true;
  }
  if (line == "region default") {
    *reply = " default scope is dk";
    return true;
  }
  *reply = "Unknown command";
  return true;
}

// The bench: the agent, a chip, the neighbourhood behind the RAK, the own console on it. The
// world's clock is the bench's, so the neighbourhood is built over it after the bench and the
// Walter is built once more over the RAK that then stands in front of the radio.
struct MeshBench {
  AgentBench agent{nullptr, true};
  std::unique_ptr<Neighbourhood> hood;

  MeshBench() {
    hood.reset(new Neighbourhood(agent.world.clock));
    agent.world.on_tick = [this](std::uint32_t ms) { hood->platform.tick(ms); };
    agent.world.meshBehind(hood->repeater);
    agent.world.simrak->console = ownConsole;
    agent.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kPowerOn);
  }

  sim::SimRak& rak() { return *agent.world.simrak; }
  fake::FakeMesh& repeater() { return hood->repeater; }
};

}  // namespace bench
