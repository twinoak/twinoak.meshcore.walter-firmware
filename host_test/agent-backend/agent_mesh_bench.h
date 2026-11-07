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

// ---- the read the server really sends, and the 34 lines of it (task p9-01) -------------------
//
// `ConfigKeys.ReadListFor(null)` in meshmaster.dk at c9732682
// (src/MeshMaster.Core/Configuration/ConfigKeys.cs): every row of the key table that has a
// `ReadOrder`, in that order, with one of `dutycycle` / `af` and not both - thirty keys, "every
// key the Configuration tab shows". Since its task p4.9-102 every `config.read` the server makes
// carries this list, so the sixteen of `mm::kDefaultConfigKeys` are only what an older claim falls
// back to. Thirty is under the thirty-two a claim may carry (`agent/ConfigRead.h`,
// `kMaxConfigKeys`).
//
// **It is 34 transactions**, which is the "34-line local config.read" of
// tasks/phase-8/FOLLOWUPS.md: the login, `region` and `region default`, twenty-eight `get`s (the
// thirty less `regions` and `acl`, which are not `get`s), `ver`, `board`, and the binary
// access-list request. 1 + 2 + 28 + 2 + 1. The server prices the same read at
// `transactionsFor("config.read", …, 30)` + `kTransactionsForLogin` = 30 + 2 + 1 + 1.
constexpr int kServerReadKeyCount = 30;
inline const char* const* serverReadKeys() {
  static const char* const keys[kServerReadKeyCount] = {
      "name", "lat", "lon", "advert.interval", "flood.advert.interval", "repeat", "dutycycle",
      "flood.max", "flood.max.unscoped", "tx", "path.hash.mode", "multi.acks", "allow.read.only",
      "agc.reset.interval", "regions", "acl", "owner.info", "flood.max.advert", "loop.detect",
      "txdelay", "direct.txdelay", "rxdelay", "int.thresh", "cad", "radio", "radio.rxgain",
      "radio.fem.rxgain", "radio.fem.txgain", "powersaving", "guest.password"};
  return keys;
}

// The same job as `ownConfigRead`, with that list: the scheduled daily read of the unit's own
// repeater, which is what job 22032 was on 7 November 2026.
inline fake::Job ownConfigReadServerList(long long id) {
  std::string payload = "{\"keys\":[";
  for (int i = 0; i < kServerReadKeyCount; ++i) {
    if (i > 0) payload += ",";
    payload += "\"";
    payload += serverReadKeys()[i];
    payload += "\"";
  }
  payload += "]}";
  return fake::Job(id, "config.read")
      .forRepeater(17, kOwnName, kOwnKeyHex)
      .atClass("admin")
      .leasedUntil(0)
      .withAdminPassword("adm1n-pass")
      .withPayload(payload)
      .asDirectNeighbour();
}

// The unit's own repeater's console, as the local door reaches it (§5.7): what a stock repeater
// answers `get`, `set`, `ver`, `board`, `region`.
// The keys are every one `serverReadKeys()` asks a `get` for, so that the daily read is thirty
// answers and not fourteen `??:` lines (which would overflow `kMaxConfigRawLines` and make the
// read about the overflow note instead). The values are a stock repeater's defaults at d9296435;
// only `name` is asserted anywhere, and the read's cost does not depend on any of them.
inline bool ownConsole(const std::string& line, std::string* reply) {
  static std::map<std::string, std::string> config = {
      {"name", "TwinOak Risskov"}, {"advert.interval", "240"}, {"flood.max", "64"}, {"freq", "869.618"},
      {"radio", "869.618,62.5,8,8"}, {"lat", "56.4167"}, {"lon", "10.8833"}, {"repeat", "on"},
      {"flood.advert.interval", "12"}, {"dutycycle", "10.0%"}, {"flood.max.unscoped", "3"},
      {"tx", "22"}, {"path.hash.mode", "1"}, {"multi.acks", "0"}, {"allow.read.only", "off"},
      {"agc.reset.interval", "0"}, {"owner.info", ""}, {"flood.max.advert", "3"},
      {"loop.detect", "on"}, {"txdelay", "0"}, {"direct.txdelay", "500"}, {"rxdelay", "0"},
      {"int.thresh", "0"}, {"cad", "off"}, {"radio.rxgain", "0"}, {"radio.fem.rxgain", "0"},
      {"radio.fem.txgain", "0"}, {"powersaving", "off"}, {"guest.password", ""},
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
