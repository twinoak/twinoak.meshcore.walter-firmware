// The session: §5.1's HELLO and what it learned, §4's capability gate, and §5.4's rule that the
// radio's clock is set after every restart this end notices - before anything else it asks for
// (task p1-02).
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/link/client/client.h"
#include "core/version.h"
#include "fixtures/link_fields.h"
#include "sim/link_bench.h"

namespace link = core::link;
namespace lc = core::link::client;

namespace {

// Somebody who wants to be told that the radio restarted (§5.1).
class Watcher final : public lc::RestartWatcher {
 public:
  int told = 0;
  std::uint32_t boot_id = 0;
  std::uint8_t reason = 0;

  void onRadioRestart(std::uint32_t id, std::uint8_t why) override {
    ++told;
    boot_id = id;
    reason = why;
  }
};

std::vector<std::uint8_t> typesOnTheWire(const sim::LinkBench& bench) {
  std::vector<std::uint8_t> out;
  for (const sim::ScriptedRadio::Seen& one : bench.radio.requests) {
    out.push_back(one.type);
  }
  return out;
}

}  // namespace

TEST_CASE("hello brings the session, and the request carries this firmware's own version") {
  sim::LinkBench bench;
  CHECK_FALSE(bench.client.session().up);
  CHECK(bench.client.mode() == lc::Mode::kUnknown);

  REQUIRE(bench.client.hello().ok());
  const lc::Session& session = bench.client.session();
  CHECK(session.up);
  CHECK(bench.client.mode() == lc::Mode::kFrames);
  CHECK(session.proto_major == link::kProtoMajor);
  CHECK(session.proto_minor == link::kProtoMinor);
  CHECK(session.role == link::role::kRepeater);
  CHECK(session.slot == 0);
  CHECK(session.max_payload == link::kMaxPayload);
  CHECK(session.caps == bench.radio.caps);
  CHECK(session.boot_id == bench.radio.boot_id);
  CHECK(std::string(session.fw_version) == "0.1.0+abc1234");
  CHECK(std::string(session.board) == "rak4630-single");
  CHECK(std::string(session.upstream) == "repeater-v1.17.1+d9296435");
  CHECK(bench.client.hasCapability(link::cap::kCli));
  CHECK(bench.client.hasCapability(link::cap::kCli | link::cap::kPower));
  CHECK_FALSE(bench.client.hasCapability(link::cap::kTimePersist));
  // §5.1's `fw_version` in the request is the sender's own, which here is the build's.
  CHECK(fixtures::asString(bench.client.firmwareVersion()) == std::string(core::version()));
}

TEST_CASE("a version string longer than the session keeps is truncated and counted") {
  sim::LinkBench bench;
  bench.radio.fw_version = std::string(lc::kVersionMax + 20, 'v');
  REQUIRE(bench.client.hello().ok());
  CHECK(std::string(bench.client.session().fw_version).size() == lc::kVersionMax - 1);
  CHECK(bench.client.counters().long_versions == 1);
  // The answer itself is whole: it is the copy that is bounded, not the decoding.
  CHECK(bench.client.session().up);
}

TEST_CASE("a capability the radio does not advertise fails without a byte on the wire") {
  sim::LinkBench bench;
  bench.radio.caps = link::cap::kPower;  // no CLI, no mesh client, no observations
  REQUIRE(bench.client.hello().ok());
  const std::size_t sent = bench.radio.requests.size();

  CHECK(bench.client.cli(link::text("get radio")).fault == lc::Fault::kUnsupported);
  CHECK(bench.client.lppGet(link::perm::kBase).fault == lc::Fault::kUnsupported);
  CHECK(bench.client.envGet().fault == lc::Fault::kUnsupported);
  CHECK(bench.client.jobEnd().fault == lc::Fault::kUnsupported);
  CHECK(bench.client.obsPull(4).fault == lc::Fault::kUnsupported);
  CHECK(bench.client.meshCounters().fault == lc::Fault::kUnsupported);
  CHECK(bench.radio.requests.size() == sent);

  // The one it does have goes out, and so does everything §4 calls "always".
  CHECK(bench.client.powerGet().ok());
  CHECK(bench.client.status().ok());
  CHECK(bench.client.timeGet().ok());
  CHECK(bench.radio.requests.size() == sent + 3);
}

TEST_CASE("without a session a capability cannot be checked, so the call is not made") {
  sim::LinkBench bench;
  CHECK(bench.client.cli(link::text("x")).fault == lc::Fault::kNoSession);
  CHECK(bench.client.replyGet().fault == lc::Fault::kNoSession);
  CHECK(bench.radio.requests.empty());
  // §4's "always" types need no session: PING is how the supervisor finds a radio at all.
  CHECK(bench.client.ping(7).ok());
  CHECK(bench.radio.requests.size() == 1);
}

TEST_CASE("every type's capability is §4's own column") {
  // The table as code (codec/wire.h's capabilityOf) against §4's table, read off the document.
  CHECK(link::capabilityOf(link::type::kHello) == 0);
  CHECK(link::capabilityOf(link::type::kPing) == 0);
  CHECK(link::capabilityOf(link::type::kStatus) == 0);
  CHECK(link::capabilityOf(link::type::kTimeSet) == 0);
  CHECK(link::capabilityOf(link::type::kTimeGet) == 0);
  CHECK(link::capabilityOf(link::type::kReboot) == 0);
  CHECK(link::capabilityOf(link::type::kIdentity) == 0);
  CHECK(link::capabilityOf(link::type::kSleepNotice) == 0);
  CHECK(link::capabilityOf(link::type::kCli) == link::cap::kCli);
  CHECK(link::capabilityOf(link::type::kPowerGet) == link::cap::kPower);
  CHECK(link::capabilityOf(link::type::kEnvGet) == link::cap::kEnv);
  CHECK(link::capabilityOf(link::type::kLppGet) == link::cap::kCli);
  CHECK(link::capabilityOf(link::type::kJobBegin) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kJobEnd) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kPathSet) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kPathGet) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kSendLogin) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kSendRequest) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kSendCli) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kSendDiscovery) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kSendTrace) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kReplyGet) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kObsPull) == link::cap::kObservations);
  CHECK(link::capabilityOf(link::type::kMeshCounters) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kEvBoot) == 0);
  CHECK(link::capabilityOf(link::type::kEvMeshReply) == link::cap::kMeshClient);
  CHECK(link::capabilityOf(link::type::kEvAttention) == link::cap::kAttention);
  // A type from a later minor version: nothing this firmware can check.
  CHECK(link::capabilityOf(0x5E) == 0);
}

TEST_CASE("a changed boot_id is a restart: the subscriber is told and TIME_SET goes first") {
  sim::LinkBench bench;
  Watcher watcher;
  bench.client.subscribeRestart(&watcher);
  bench.wall.give(1791720000, link::time_source::kCellular);

  REQUIRE(bench.client.hello().ok());
  const std::uint32_t first = bench.client.session().boot_id;
  CHECK(watcher.told == 0);  // the first hello of a session is not news of a restart
  CHECK(bench.client.timePending());
  // §5.4: the clock is set before anything else this end asks for.
  REQUIRE(bench.client.status().ok());
  std::vector<std::uint8_t> wire = typesOnTheWire(bench);
  REQUIRE(wire.size() == 3);
  CHECK(wire[0] == link::type::kHello);
  CHECK(wire[1] == link::type::kTimeSet);
  CHECK(wire[2] == link::type::kStatus);
  CHECK_FALSE(bench.client.timePending());
  CHECK(bench.client.counters().time_sets == 1);

  // The radio restarts without saying so (the Walter was asleep): the next hello shows a new
  // boot_id, the subscriber is told, and TIME_SET is the next request on the wire.
  bench.radio.restart(false);
  REQUIRE(bench.client.hello().ok());
  CHECK(bench.client.session().boot_id != first);
  CHECK(watcher.told == 1);
  CHECK(watcher.boot_id == bench.client.session().boot_id);
  CHECK(watcher.reason == link::reset_reason::kUnknown);  // a HELLO does not carry one
  CHECK(bench.client.timePending());
  const std::size_t before = bench.radio.requests.size();
  REQUIRE(bench.client.ping(1).ok());
  wire = typesOnTheWire(bench);
  REQUIRE(wire.size() == before + 2);
  CHECK(wire[before] == link::type::kTimeSet);
  CHECK(wire[before + 1] == link::type::kPing);
}

TEST_CASE("an EV_BOOT is a restart too, and it brings the session with it") {
  sim::LinkBench bench;
  Watcher watcher;
  bench.client.subscribeRestart(&watcher);
  bench.wall.give(1791720000);

  // The port is open from boot in the firmware (main/main.cpp), which is what makes an event
  // from a radio nobody has spoken to yet audible at all: poll() reads the port, it does not
  // take it - taking a pin is never a side effect of listening.
  REQUIRE(bench.port.open() == core::board::Outcome::kOk);

  // §5.1: EV_BOOT is sent once when the radio's link comes up after a reset. Nothing has said
  // hello, and the event's body is the HELLO body - so the session is known without a round trip.
  bench.radio.restart(true);
  bench.client.poll();
  CHECK(watcher.told == 1);
  CHECK(watcher.reason == link::reset_reason::kWatchdog);  // the fixture's own reset reason
  CHECK(bench.client.session().up);
  CHECK(bench.client.session().role == link::role::kRepeater);
  CHECK(bench.client.session().boot_id == bench.radio.boot_id);
  CHECK(bench.client.mode() == lc::Mode::kFrames);
  CHECK(bench.client.timePending());
  CHECK(bench.client.eventsQueued() == 1);

  // And the clock is the first thing it asks for.
  REQUIRE(bench.client.status().ok());
  const std::vector<std::uint8_t> wire = typesOnTheWire(bench);
  REQUIRE(wire.size() == 2);
  CHECK(wire[0] == link::type::kTimeSet);
  CHECK(wire[1] == link::type::kStatus);

  // A second EV_BOOT with the same boot_id - a repeat, not a second restart.
  bench.radio.sendEvent("boot event");
  bench.client.poll();
  CHECK(watcher.told == 1);
}

TEST_CASE("with no time to give, nothing is sent and the clock stays pending") {
  sim::LinkBench bench;
  bench.wall.forget();
  REQUIRE(bench.client.hello().ok());
  CHECK(bench.client.timePending());
  REQUIRE(bench.client.status().ok());
  const std::vector<std::uint8_t> wire = typesOnTheWire(bench);
  REQUIRE(wire.size() == 2);
  CHECK(wire[1] == link::type::kStatus);  // no TIME_SET in between
  CHECK(bench.client.timePending());
  CHECK(bench.wall.asked > 0);
  CHECK(bench.client.counters().time_sets == 0);

  // Once there is a time, the next call carries it.
  bench.wall.give(1791720000, link::time_source::kServer);
  REQUIRE(bench.client.timeGet().ok());
  CHECK_FALSE(bench.client.timePending());
  CHECK(bench.radio.requests[2].type == link::type::kTimeSet);
  // §5.4: the source the Walter gives is the one it learned the time from.
  CHECK(bench.radio.requests[2].payload.size() == 6);
  CHECK(bench.radio.requests[2].payload[4] == link::time_source::kServer);
  CHECK(bench.radio.requests[2].payload[5] == 0);  // never *force*, unasked
}

TEST_CASE("a clock the radio refuses is not asked for again and again") {
  // §5.4: the radio refuses an epoch earlier than its own clock without *force*. It has said so;
  // the Walter does not spend three attempts on it at every call.
  sim::LinkBench bench;
  bench.wall.give(1715770400);
  REQUIRE(bench.client.hello().ok());
  bench.radio.answerWith("time set refused: backwards without force");
  REQUIRE(bench.client.status().ok());
  CHECK_FALSE(bench.client.timePending());
  const std::vector<std::uint8_t> wire = typesOnTheWire(bench);
  REQUIRE(wire.size() == 3);
  CHECK(wire[1] == link::type::kTimeSet);
  REQUIRE(bench.client.status().ok());
  CHECK(typesOnTheWire(bench).size() == 4);  // no second TIME_SET
}

TEST_CASE("a hello the radio refuses: the major it does not speak") {
  sim::LinkBench bench;
  bench.radio.answerWith("hello refused: major not spoken");
  const lc::Answer<link::HelloResponse> answer = bench.client.hello();
  CHECK(answer.fault == lc::Fault::kRefused);
  CHECK(answer.status == link::status::kUnsupported);
  CHECK_FALSE(bench.client.session().up);
  // It answered, so it speaks frames - it just does not speak this major (§5.1).
  CHECK(bench.client.mode() == lc::Mode::kFrames);
}

TEST_CASE("radioWasReset forgets everything about the radio without sending anything") {
  sim::LinkBench bench;
  REQUIRE(bench.client.hello().ok());
  const std::size_t sent = bench.radio.requests.size();
  bench.client.radioWasReset();
  CHECK_FALSE(bench.client.session().up);
  CHECK(bench.client.session().boot_id == 0);
  CHECK(bench.client.mode() == lc::Mode::kUnknown);
  CHECK(bench.radio.requests.size() == sent);
  // And a call that needs a capability has nothing to check it against until the next hello.
  CHECK(bench.client.cli(link::text("x")).fault == lc::Fault::kNoSession);
}
