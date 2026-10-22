// The heartbeat's bytes (task p4-04): what this unit would post to
// `POST /agent/v1/heartbeat`, held against **meshmaster.dk's own contract fixture** - and held
// equal to what the copied library's loop posts when it is driven over the same identity, so
// that the body `agent heartbeat` prints is the body a wake sends.
//
// -- the fixture, and the six fields that are this unit's ------------------------------------
//
// `tests/fixtures/contracts/AgentHeartbeat.json` is the Heltec's heartbeat: `onboard` on a
// `heltec_v3`, with the two optional fields of an agent that paces itself. It is
// meshmaster.dk's and **nothing here regenerates it**: the expected bytes are made from it by
// substituting, one at a time and with a reason, the fields whose source is *this* unit. The
// substitution is textual and the field order, the spacing and the escaping are the fixture's,
// so what is held is the shape as well as the values.
//
//   transport                 `native`: an exit node that *is* its repeater (EXIT-NODES §2)
//   at                        this unit's clock, which is not the day the fixture was written
//   companionPublicKey        the repeater's own key, from §5.6 - 64 hex and not the fixture's
//                             four characters
//   companionFirmware         HELLO's `fw_version`, the repeater firmware's release version
//   errors                    the six counters the library's own `doHeartbeat` writes, where the
//                             fixture has one example counter
//   board                     `walter`
//   maxTransactionsThisWake   the band's own cap (walter_agent/identity.h's table)
//
// Everything else is the fixture's, byte for byte: the thirteen job types in the server's order,
// the radio block - whose four numbers the specification's own `cli response` fixture answers
// with - the default budget, `companionConnected`, `queueDepth`, and `nextCheckInS`, which at the
// `normal` band is the 3600 the fixture carries.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <agent/AgentLoop.h>
#include <agent/JobRunner.h>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/cycle/network_clock.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/power/interval_policy.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_entropy.h"
#include "fakes/fake_heap.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fixtures/heartbeat_no_key.h"
#include "sim/scripted_radio.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/observations.h"
#include "walter_agent/walter_platform.h"

namespace link = core::link;
namespace cycle = core::cycle;
namespace power = core::power;

namespace {

constexpr const char* kKeyHex =
    "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c";
// The first epoch core/uplink/clock_window.h believes, which is this firmware's build date:
// 2026-10-14T00:00:00Z.
constexpr std::uint32_t kEpoch = 1791936000u;
constexpr const char* kEpochIso = "2026-10-14T00:00:00Z";

std::string readFixture(const char* name) {
  const std::string path = std::string(FIXTURES_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "the copied contract fixture is missing");
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  // The fixtures are one line; a trailing newline is the file's and not the body's.
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// One documented substitution. A replacement that found nothing is a fixture that has moved, and
// that must fail here rather than quietly compare something else.
std::string substitute(std::string text, const std::string& from, const std::string& to,
                       const char* why) {
  const std::size_t at = text.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, why);
  return text.replace(at, from.size(), to);
}

// The six counters the library writes, as JSON, in the library's own order (AgentLoop.cpp's
// `doHeartbeat`). Every one is 0 for a unit that has not run a wake yet.
const char* const kZeroErrors =
    "\"errors\":{\"overheard\":0,\"wifiReconnects\":0,\"httpFailures\":0,"
    "\"observationsDropped\":0,\"resultsDropped\":0,\"reboots\":0}";

// A `mm::IUplink` that answers and keeps what it was given: what the loop posted, verbatim.
class CapturingUplink final : public mm::IUplink {
 public:
  std::string heartbeat_body;  // not available: the loop hands a struct, not bytes
  mm::Heartbeat posted;
  int heartbeats = 0;

  bool configured() const override { return true; }
  LinkState link() const override { return LinkState{true, 1, 0}; }
  bool fetchConfig() override {
    // What a server answers before it has been told otherwise: AgentBudget.Default, two
    // transactions a minute twenty seconds apart - the budget the contract fixture carries.
    config_.budget.maxTransactionsPerMinute = mm::kDefaultMaxTransactionsPerMinute;
    config_.budget.minGapSeconds = mm::kDefaultMinGapSeconds;
    has_config_ = true;
    return true;
  }
  bool postHeartbeat(const mm::Heartbeat& hb, mm::HeartbeatReply& reply) override {
    ++heartbeats;
    posted = hb;
    // The bytes, written by the library's own writer - which is what `ModemUplink` does with the
    // struct it is handed (task p4-03).
    static char body[2048];
    const std::size_t n = mm::writeHeartbeat(hb, body, sizeof body);
    heartbeat_body.assign(body, n);
    reply = mm::HeartbeatReply{};
    reply.pollIntervalS = static_cast<int>(hb.nextCheckInS);
    return true;
  }
  bool postObservations(const char*, std::size_t, int& accepted, bool& duplicate) override {
    accepted = 0;
    duplicate = false;
    return true;
  }
  bool claimJobs(mm::ClaimedJob*, int, int& count) override {
    count = 0;
    return true;
  }
  bool postJobResult(long long, const char*, std::size_t) override { return true; }
  bool extendLease(long long, int, std::uint32_t&) override { return false; }
  bool postProgress(long long, const char*, std::size_t) override { return true; }
  bool hasConfig() const override { return has_config_; }
  const mm::AgentConfig& config() const override { return config_; }
  const Outcome& last() const override { return last_; }
  const Call& lastCall() const override { return call_; }
  const mm::RateLimiter& limiter() const override { return limiter_; }

 private:
  bool has_config_ = false;
  mm::AgentConfig config_;
  Outcome last_;
  Call call_;
  mm::RateLimiter limiter_;
};

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::FakeEntropy entropy;
  fakes::FakeHeap heap;
  sim::ScriptedRadio rak{clock};
  cycle::NetworkClock wall{clock};
  core::board::RadioPins radio{pins, clock, retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};
  agent::Identity identity{client, "0.3.1"};
  agent::WalterPlatform platform{clock, wall, entropy, heap};

  Rig() {
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    // §5.7's CLI is answered out of the specification's own fixtures, whose `cli request` is
    // `get radio` and whose `cli response` is `> 869.618,62.5,8,8` - the four numbers
    // meshmaster.dk's RadioParameters.json carries.
    REQUIRE(wall.give(kEpoch));
  }
};

// A radio whose HELLO says `v1.17.1`, which is the fixture's own `companionFirmware`, so that one
// fewer field has to be substituted - and the one that is not substituted is the one whose source
// this task had to establish (the repeater firmware's release version, not the MeshCore tag).
struct FirmwareRig : Rig {
  FirmwareRig() {
    rak.fw_version = "v1.17.1";
    REQUIRE(client.hello().ok());
    REQUIRE(identity.refresh());
  }
};

agent::HeartbeatExtras extrasAt(std::uint32_t epoch) {
  agent::HeartbeatExtras out;
  out.at = epoch;
  return out;
}

}  // namespace

TEST_CASE("the heartbeat body is the contract fixture's, with the six fields that are this unit's") {
  FirmwareRig rig;
  REQUIRE(rig.identity.known());
  REQUIRE(rig.identity.radio().known);

  // The intent of a `normal` wake: an hour, which is the fixture's own 3600.
  const mm::WakeIntent intent = agent::intentFor(power::Band::kNormal, power::kNormalMinutes * 60u);
  REQUIRE(intent.nextCheckInS == 3600u);

  char body[2048] = {};
  const std::size_t written =
      rig.identity.writeHeartbeat(&rig.mesh, nullptr, extrasAt(kEpoch), intent, body, sizeof body);
  REQUIRE(written != 0);
  const std::string ours(body, written);

  std::string expected = readFixture("AgentHeartbeat.json");
  expected = substitute(expected, "\"transport\":\"onboard\"", "\"transport\":\"native\"",
                        "an exit node that is its repeater says `native` (EXIT-NODES §2)");
  expected = substitute(expected, "\"at\":\"2026-09-23T14:00:00Z\"",
                        std::string("\"at\":\"") + kEpochIso + "\"", "this unit's own clock");
  expected = substitute(expected, "\"companionPublicKey\":\"a1b2\"",
                        std::string("\"companionPublicKey\":\"") + kKeyHex + "\"",
                        "the repeater's own 32-byte key, from §5.6");
  expected = substitute(expected, "\"errors\":{\"uplink\":0}", kZeroErrors,
                        "the six counters the library's doHeartbeat writes");
  expected = substitute(expected, "\"board\":\"heltec_v3\"", "\"board\":\"walter\"", "this board");
  expected = substitute(expected, "\"maxTransactionsThisWake\":12",
                        "\"maxTransactionsThisWake\":" + std::to_string(agent::intent::kNormal),
                        "the `normal` band's own cap");

  CHECK_MESSAGE(ours == expected, ours);
  // And the fields the fixture and this unit share were not substituted: the thirteen job types
  // in the server's order, and the radio block the specification's own CLI fixture answers with.
  CHECK(ours.find("\"radio\":{\"freqMHz\":869.618,\"bwKHz\":62.5,\"sf\":8,\"cr\":8}") !=
        std::string::npos);
  CHECK(ours.find("\"jobTypes\":[\"status\",\"telemetry\",\"neighbours\"") != std::string::npos);
  CHECK(ours.find("\"companionFirmware\":\"v1.17.1\"") != std::string::npos);
  CHECK(ours.find("\"companionConnected\":true") != std::string::npos);
}

TEST_CASE("the body the library's own loop posts is the body this identity builds") {
  FirmwareRig rig;
  REQUIRE(rig.identity.known());

  // The loop, wired as docs/library.md §6 says, over this task's platform and this unit's
  // identity - and a capturing uplink in place of the modem, so that what is held is the bytes.
  // This is the proof that `agent heartbeat` is not a second opinion: the console prints what a
  // wake sends.
  static mm::JobRunner jobs;  // ~66 kB on a host: never a local, as the library says
  static mm::AgentLoop agent;
  CapturingUplink server;
  jobs.begin(rig.mesh, server, rig.platform, nullptr);
  agent.begin(server, rig.platform, rig.mesh, jobs, rig.identity.heartbeatIdentity());
  agent.setIdentity(rig.identity.publicKeyHex());

  const mm::WakeIntent intent = agent::intentFor(power::Band::kNormal, power::kNormalMinutes * 60u);
  mm::Budget budget;
  budget.wakeSeconds = 240;
  budget.uplinkSeconds = 60;
  const mm::WakeReport report = agent.wake(intent, budget);
  (void)report;
  REQUIRE(server.heartbeats == 1);

  // The `at` is the loop's own: it reads the clock at the moment it builds the heartbeat, and
  // what is held here is that the other fields - every one of them - are the identity's.
  char ours[2048] = {};
  const std::size_t written = rig.identity.writeHeartbeat(&rig.mesh, nullptr,
                                                          extrasAt(server.posted.at), intent,
                                                          ours, sizeof ours);
  REQUIRE(written != 0);
  CHECK_MESSAGE(std::string(ours, written) == server.heartbeat_body, server.heartbeat_body);
}

TEST_CASE("a unit before its repeater: no key, no radio block, no job types - and it still checks in") {
  Rig rig;
  // A RAK with no application answers §5.6 with a key of all zeros, and the link carries no mesh
  // client either, so `supports()` is false for every type and the list is empty.
  rig.rak.caps = link::cap::kCli;
  rig.rak.on_request = [](const link::Packet& packet, std::vector<std::uint8_t>* payload) {
    if (packet.type != link::type::kIdentity) return false;
    payload->assign(1 + link::kPubkeySize + 1, 0);
    return true;
  };
  REQUIRE(rig.client.hello().ok());
  CHECK_FALSE(rig.identity.refresh());

  const mm::WakeIntent intent = agent::intentFor(power::Band::kNormal, power::kNormalMinutes * 60u);
  char body[2048] = {};
  const std::size_t written =
      rig.identity.writeHeartbeat(&rig.mesh, nullptr, extrasAt(kEpoch), intent, body, sizeof body);
  REQUIRE(written != 0);
  const std::string ours(body, written);

  // meshmaster.dk's own example of this heartbeat (its task p4.16-132), with the three fields
  // that are this unit's substituted: see host_test/fixtures/heartbeat_no_key.h for where those
  // bytes come from and why they are not in the copied fixture set.
  std::string expected = fixtures::kAgentHeartbeatNoKey;
  expected = substitute(expected, "\"at\":\"2026-09-23T14:00:00Z\"",
                        std::string("\"at\":\"") + kEpochIso + "\"", "this unit's own clock");
  expected = substitute(expected, "\"errors\":{\"uplink\":0}", kZeroErrors,
                        "the six counters the library's doHeartbeat writes");
  expected = substitute(expected, ",\"nextCheckInS\":3600", ",\"board\":\"walter\",\"nextCheckInS\":3600",
                        "this unit names its board whether or not its radio has a key");
  expected = substitute(expected, "\"maxTransactionsThisWake\":12",
                        "\"maxTransactionsThisWake\":" + std::to_string(agent::intent::kNormal),
                        "the `normal` band's own cap");

  CHECK_MESSAGE(ours == expected, ours);
  CHECK_MESSAGE(ours.find("companionPublicKey") == std::string::npos,
                "the server keeps the key it had: Envelopes.cs makes the field optional");
  CHECK_MESSAGE(ours.find("\"jobTypes\":[]") != std::string::npos,
                "a unit without a radio can run no job, and an empty list is how it says so");
  CHECK(ours.find("companionFirmware") == std::string::npos);
  CHECK(ours.find("\"radio\"") == std::string::npos);
  CHECK(ours.find("\"companionConnected\":false") != std::string::npos);
}

TEST_CASE("the identity appearing between two wakes: the next heartbeat carries the key") {
  Rig rig;
  bool no_key = true;
  rig.rak.on_request = [&no_key](const link::Packet& packet,
                                 std::vector<std::uint8_t>* payload) {
    if (packet.type != link::type::kIdentity) return false;
    if (!no_key) return false;
    payload->assign(1 + link::kPubkeySize + 1, 0);
    return true;
  };
  REQUIRE(rig.client.hello().ok());
  CHECK_FALSE(rig.identity.refresh());

  const mm::WakeIntent intent = agent::intentFor(power::Band::kNormal, power::kNormalMinutes * 60u);
  char first[2048] = {};
  REQUIRE(rig.identity.writeHeartbeat(&rig.mesh, nullptr, extrasAt(kEpoch), intent, first,
                                      sizeof first) != 0);
  CHECK(std::string(first).find("companionPublicKey") == std::string::npos);

  // The radio has a key now - a person provisioned the base and the application between two
  // wakes, which is the whole point of registering a unit before its repeater.
  no_key = false;
  CHECK(rig.identity.refresh());
  char second[2048] = {};
  REQUIRE(rig.identity.writeHeartbeat(&rig.mesh, nullptr, extrasAt(kEpoch), intent, second,
                                      sizeof second) != 0);
  const std::string with_key(second);
  CHECK(with_key.find(std::string("\"companionPublicKey\":\"") + kKeyHex + "\"") !=
        std::string::npos);
  CHECK(with_key.find("\"radio\":{\"freqMHz\":869.618") != std::string::npos);
  CHECK(with_key.find("\"jobTypes\":[\"status\"") != std::string::npos);
}

TEST_CASE("the band moves what the heartbeat promises, and `critical` promises nothing") {
  FirmwareRig rig;
  char body[2048] = {};
  const auto bodyFor = [&](power::Band band, std::uint32_t seconds) {
    const mm::WakeIntent intent = agent::intentFor(band, seconds);
    const std::size_t n =
        rig.identity.writeHeartbeat(&rig.mesh, nullptr, extrasAt(kEpoch), intent, body, sizeof body);
    REQUIRE(n != 0);
    return std::string(body, n);
  };

  const std::string plenty = bodyFor(power::Band::kPlenty, power::kPlentyMinutes * 60u);
  CHECK(plenty.find("\"nextCheckInS\":900") != std::string::npos);
  CHECK(plenty.find("\"maxTransactionsThisWake\":" + std::to_string(agent::intent::kPlenty)) !=
        std::string::npos);

  const std::string saving = bodyFor(power::Band::kSaving, power::kSavingMinutes * 60u);
  CHECK(saving.find("\"nextCheckInS\":21600") != std::string::npos);
  CHECK(saving.find("\"maxTransactionsThisWake\":" + std::to_string(agent::intent::kSaving)) !=
        std::string::npos);

  // `critical` does not use the modem at all, so the cap is 0 - and the library writes neither 0
  // nor the field: an agent that promises nothing is an agent as before, byte for byte.
  const std::string critical = bodyFor(power::Band::kCritical, power::kCriticalMinutes * 60u);
  CHECK(critical.find("\"nextCheckInS\":86400") != std::string::npos);
  CHECK(critical.find("maxTransactionsThisWake") == std::string::npos);
}
