// `agent id`, `agent heartbeat` and `agent obs` on the console (task p4-04): the probe of
// components/agent/ over the identity, the platform and the observation drain, against the
// scripted radio answering §5.1, §5.6, §5.7 and §7.
//
// What is held here is the console's words and what does **not** reach them: no token, and no
// advert name.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/cycle/network_clock.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/wire.h"
#include "core/settings/settings.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_entropy.h"
#include "fakes/fake_heap.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/sim_key_value.h"
#include "sim/quiet_repeater.h"
#include "sim/sim_rak.h"
#include "walter_agent/agent_probe.h"
#include "walter_agent/identity.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/observations.h"
#include "walter_agent/walter_platform.h"

namespace link = core::link;
namespace cycle = core::cycle;
using core::console::Console;
using core::health::Health;
using core::health::ResetReason;
using core::health::WakeCause;

namespace {

constexpr const char* kKeyHex =
    "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c";
// What nothing of this console may print, however it is asked (spec/SYSTEM.md S-12): a token,
// and the name a stranger put in an advert.
constexpr const char* kToken = "tok-abcdef0123456789";
constexpr const char* kAdvertName = "Grenaa #01";

std::vector<std::uint8_t> hex(const std::string& text) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
    const auto digit = [](char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return 0;
    };
    out.push_back(static_cast<std::uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
  }
  return out;
}

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::FakeEntropy entropy;
  fakes::FakeHeap heap;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  fakes::SimKeyValue nvs;
  core::settings::Settings settings{nvs};
  sim::QuietRepeater repeater;
  sim::SimRak rak{clock, repeater};
  cycle::NetworkClock wall{clock};
  core::board::RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};
  mm::ObservationRing ring;
  agent::Identity identity{client, "0.4.0+p4test"};
  agent::WalterPlatform platform{clock, wall, entropy, heap};
  agent::Observations observations{client, mesh};
  agent::IdentityProbe probe{identity, mesh, observations, platform};
  // The positions after `slots`: images, receiver, update, journal, self, supervisor, uplink,
  // http, fetch, trust, **settings**, server, duty, cycle, battery, mesh, agent, **identity**.
  Console console{out,     radio,  health, clock, restart, client, slots,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  &settings, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &probe};

  Rig() {
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    settings.begin();
    // Typed as a person types it, so that the echo is in the capture too: nothing of this
    // console may print it back, however it is asked.
    console.runLine((std::string("set server.token ") + kToken).c_str());
    REQUIRE(settings.isSet(core::settings::Key::kServerToken));
    mesh.setRing(&ring);
    rak.console = [](const std::string& line, std::string* reply) {
      *reply = line == "get radio" ? "> 869.618,62.5,8,8" : "Unknown command";
      return true;
    };
    heap.say(176880, 160224);
    REQUIRE(client.hello().ok());
  }

  std::string run(const std::string& line) {
    out.clear();
    console.runLine(line.c_str());
    return out.text;
  }
};

bool holds(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("agent id: every field, its source, and never the token") {
  Rig rig;
  const std::string text = rig.run("agent id");
  CHECK(holds(text, "agent: version 0.4.0+p4test, board walter, transport native"));
  CHECK(holds(text, kKeyHex));
  CHECK(holds(text, "name \"TwinOak Risskov\""));
  CHECK(holds(text, "firmware 0.1.0+abc1234"));
  CHECK(holds(text, "board rak4630-single"));
  CHECK(holds(text, "built on repeater-v1.17.1+d9296435"));
  CHECK(holds(text, "radio: 869.618 MHz, bw 62.5 kHz, sf 8, cr 8"));
  CHECK(holds(text, "job types (13): status, telemetry, neighbours"));
  CHECK(holds(text, "heap: 176880 free, 160224 the least ever"));
  // The token is *set* or *unset*, as `get server.token` says it, and its value is nowhere.
  CHECK(holds(text, "server.token set"));
  CHECK_FALSE(holds(text, kToken));
  // And what it cost the link: one §5.6 request and one §5.7 line.
  CHECK(holds(text, "1 identity read(s), 1 radio read(s)"));
}

TEST_CASE("agent id with no time of day says so, and the heartbeat's `at` is 0") {
  Rig rig;
  const std::string text = rig.run("agent id");
  CHECK(holds(text, "clock: this unit does not know what time it is"));
  CHECK(holds(rig.run("agent heartbeat"), "\"at\":\"1970-01-01T00:00:00Z\""));
}

TEST_CASE("agent id with a time of day names where it came from") {
  Rig rig;
  REQUIRE(rig.wall.give(1791936000u));
  CHECK(holds(rig.run("agent id"), "clock: 1791936000, from the network"));
}

TEST_CASE("agent id before anything was asked of the radio says there is no session") {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::FakeEntropy entropy;
  fakes::FakeHeap heap;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  sim::QuietRepeater repeater;
  sim::SimRak rak{clock, repeater};
  cycle::NetworkClock wall{clock};
  core::board::RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};
  agent::Identity identity{client, "0.4.0+p4test"};
  agent::WalterPlatform platform{clock, wall, entropy, heap};
  agent::Observations observations{client, mesh};
  agent::IdentityProbe probe{identity, mesh, observations, platform};
  Console console{out,   radio, health, clock, restart, client, slots,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &probe};
  health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
  out.clear();
  console.runLine("agent id");
  CHECK(holds(out.text, "the radio has answered nothing in this boot"));
  CHECK(holds(out.text, "job types (0): none"));
}

TEST_CASE("agent heartbeat prints the body and sends nothing") {
  Rig rig;
  const std::string text = rig.run("agent heartbeat");
  CHECK(holds(text, "POST /agent/v1/heartbeat"));
  CHECK(holds(text, "built and not sent"));
  CHECK(holds(text, "\"transport\":\"native\""));
  CHECK(holds(text, "\"board\":\"walter\""));
  CHECK(holds(text, std::string("\"companionPublicKey\":\"").append(kKeyHex).append("\"").c_str()));
  CHECK(holds(text, "\"radio\":{\"freqMHz\":869.618,\"bwKHz\":62.5,\"sf\":8,\"cr\":8}"));
  CHECK_FALSE(holds(text, kToken));
  // It is the identity's own writer, so the bytes are the ones a wake would post: the test of
  // that equality is host_test/agent/test_heartbeat_body.cpp.
  std::size_t length = 0;
  const char* body = rig.probe.heartbeat(&length);
  REQUIRE(body != nullptr);
  CHECK(holds(text, body));
}

TEST_CASE("agent obs: the ring, the radio's figures, and never an advert's name") {
  Rig rig;
  sim::SimRak::Advert heard;
  heard.pubkey = hex("a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90");
  heard.age_s = 41;
  heard.snr_q4 = 29;
  heard.rssi = -84;
  heard.app_data = hex("9248cb5c0330fca5004772656e616120233031");  // the name is "Grenaa #01"
  rig.rak.observations.push_back(heard);
  rig.rak.obs_dropped = 0;

  SUBCASE("without `pull` the radio is asked nothing and the ring is empty") {
    const std::string text = rig.run("agent obs");
    CHECK(holds(text, "0 of 64 waiting"));
    CHECK(holds(text, "nothing in the ring"));
    CHECK(rig.rak.obs_pulls == 0u);
  }

  SUBCASE("`agent obs pull` drains §7 and prints the row - with no name in it") {
    const std::string text = rig.run("agent obs pull");
    CHECK(holds(text, "the radio's ring drained: 1 record(s) kept"));
    CHECK(holds(text, "1 of 64 waiting"));
    CHECK(holds(text, "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"));
    CHECK(holds(text, "41 s ago, 0 hop(s)"));
    CHECK(holds(text, "snr 7.25 dB"));
    CHECK(holds(text, "rssi -84 dBm"));
    CHECK(holds(text, "repeater"));
    CHECK(holds(text, "with a position"));
    CHECK_MESSAGE(!holds(text, kAdvertName),
                  "§7: the name inside app_data is something a stranger chose, and neither end "
                  "logs it - and every line of this console is kept in the ring `dump` prints");
    CHECK(rig.rak.obs_pulls == 1u);

    // And not in the four kilobytes `dump` prints either (S-12, and the rail R9 the whole
    // console is held to): `agent obs` is the one command that handles an advert, so the ring
    // is where a name would have leaked into if anything printed it.
    const std::string kept = rig.run("dump");
    CHECK_FALSE(holds(kept, kAdvertName));
    CHECK_MESSAGE(holds(kept, "agent obs"), "the command itself is in the ring, so the grep looked at something");
  }
}

TEST_CASE("agent obs: a negative SNR reads as a negative number") {
  Rig rig;
  sim::SimRak::Advert heard;
  // Any key but this unit's own, which is the fixture's `7f3a9c02...`: an advert about the
  // repeater this exit node *is* would be dropped, which the drain's own test holds.
  heard.pubkey = hex("0011223344556677889900112233445566778899001122334455667788990011");
  heard.age_s = 300;
  heard.snr_q4 = -10;
  heard.rssi = 0x7FFF;
  heard.path_len = 66;
  heard.path = hex("a1b27f3a");
  heard.app_data = hex("8157696e646f77");
  rig.rak.observations.push_back(heard);
  const std::string text = rig.run("agent obs pull");
  CHECK(holds(text, "snr -2.50 dB"));
  CHECK_MESSAGE(!holds(text, "rssi"), "§7's 0x7FFF is no RSSI, and absent is not zero");
  CHECK(holds(text, "2 hop(s)"));
  CHECK(holds(text, "companion"));
}

TEST_CASE("a build with no agent identity answers in one line") {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  sim::QuietRepeater repeater;
  sim::SimRak rak{clock, repeater};
  cycle::NetworkClock wall{clock};
  core::board::RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  link::client::LinkPort port{radio, rak};
  link::client::Client client{port, clock, wall, slots};
  Console console{out, radio, health, clock, restart, client, slots};
  health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
  for (const char* line : {"agent id", "agent heartbeat", "agent obs", "agent obs pull"}) {
    out.clear();
    console.runLine(line);
    CHECK(holds(out.text, "this build has no agent identity"));
  }
}
