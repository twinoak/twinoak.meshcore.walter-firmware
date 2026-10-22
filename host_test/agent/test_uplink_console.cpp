// `agent url` and `agent probe` on the console (task p4-03): the probe of components/agent/ over
// ModemUplink and the HTTPS client, against the scripted server answering the contract
// fixture - so what is held here is the console's words: the URL and the version header, the
// token said *set* and never printed, a 200 with the config, a 401, a TLS failure, a build
// without an agent uplink, the uplink down, and the parser's refusals.
#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/console/parser.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/net/client.h"
#include "core/settings/settings.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_http.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"
#include "walter_agent/modem_uplink.h"
#include "walter_agent/uplink_probe.h"

using core::console::Console;
using core::console::parseLine;
using core::console::Verb;

namespace {

constexpr const char* kToken = "mma_zQ7x4LpR8vK2nT6yW1sB3dF5gH9jM0cV";
constexpr const char* kTokenBody = "zQ7x4LpR";
constexpr const char* kConfig =
    "{\"agentId\":3,\"label\":\"TwinOak Risskov companion\",\"transport\":\"native\","
    "\"budget\":{\"maxTransactionsPerMinute\":2,\"minGapSeconds\":20},\"pollIntervalS\":3600,"
    "\"serverTime\":\"2026-09-23T14:00:01Z\"}";

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::board::RadioPins radio{pins, clock, retained};
  core::health::Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, wall, slots};
  // The modem and the policy over it, so that `agent probe` is held to 'lte up' first.
  sim::FakeModem modem{clock};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{modem, clock, retained, modem_settings};
  // The HTTPS client over the scripted server, and the settings the uplink reads.
  sim::FakeHttp http{clock};
  core::net::Client client{http, clock};
  fakes::SimKeyValue kv;
  core::settings::Settings settings{kv};
  agent::ModemUplink server{client, settings, clock, wall, "0.4.0+p4test"};
  agent::ModemUplinkProbe probe{server};
  std::unique_ptr<Console> console;

  Rig() {
    pins.record_log = false;
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
    clock.advance(core::board::kKillTiming.settle_ms);
    radio.safe();
    uplink.begin();
    kv.put("srv_url", "https://server.example");
    kv.put("srv_token", kToken);
    settings.begin();
    wall.give(1790000000);
    http.server.serve("/agent/v1/config", std::vector<std::uint8_t>(kConfig, kConfig + std::strlen(kConfig)));
    full();
  }

  void full() {
    console.reset(new Console(out, radio, health, clock, restart, link, slots, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, &uplink, &client, nullptr, nullptr, &settings,
                              nullptr, nullptr, nullptr, nullptr, nullptr, &probe));
  }
  void withoutAgent() {
    console.reset(new Console(out, radio, health, clock, restart, link, slots, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, &uplink, &client, nullptr, nullptr, &settings));
  }
  // The uplink up, as `lte up` would leave it: the fake modem attaches inside the budget.
  void lteUp() {
    REQUIRE(uplink.bringUp(clock.nowMs() + 240000).up);
    server.setLinkUp(true);
  }

  std::string run(const char* line) {
    out.clear();
    console->runLine(line);
    return out.text;
  }
};

}  // namespace

TEST_CASE("the parser takes the two agent lines and refuses the rest") {
  CHECK(parseLine("agent url").command.verb == Verb::kAgentUrl);
  CHECK(parseLine("agent probe").command.verb == Verb::kAgentProbe);
  CHECK(parseLine("  agent   probe \r\n").command.verb == Verb::kAgentProbe);
  CHECK_FALSE(parseLine("agent").ok);
  CHECK(has(parseLine("agent").error, "agent needs a subcommand"));
  CHECK_FALSE(parseLine("agent token").ok);
  // Task p4-04 added three more words to the same verb, so the refusal names all of them.
  CHECK(has(parseLine("agent token").error, "unknown agent subcommand (url, probe, id, heartbeat, obs, obs pull): token"));
  CHECK_FALSE(parseLine("agent probe now").ok);
}

TEST_CASE("agent url: the server, the version header and whether a token is set - never the token") {
  Rig rig;
  const std::string text = rig.run("agent url");
  CHECK(has(text, "agent url: https://server.example"));
  CHECK(has(text, "X-Agent-Version: 0.4.0+p4test"));
  CHECK(has(text, "server.token set"));
  CHECK_FALSE(has(text, kTokenBody));
  CHECK_FALSE(has(text, "not configured"));
  CHECK(rig.http.socket.opens == 0);  // it reads; it does not call
}

TEST_CASE("agent url without a token or a URL says what is missing") {
  Rig rig;
  rig.kv.powerCut();
  rig.settings.begin();
  std::string text = rig.run("agent url");
  CHECK(has(text, "agent url: server.url unset"));
  CHECK(has(text, "server.token unset"));
  CHECK(has(text, "not configured"));
  rig.lteUp();
  text = rig.run("agent probe");
  CHECK(has(text, "error: agent probe: not configured"));
  CHECK(rig.http.socket.opens == 0);
}

TEST_CASE("agent probe: one GET /agent/v1/config, the status and the config printed, the token nowhere") {
  Rig rig;
  rig.lteUp();
  const std::string text = rig.run("agent probe");
  CHECK(has(text, "agent probe: GET https://server.example/agent/v1/config with the token"));
  CHECK(has(text, "agent probe: /agent/v1/config -> http 200 ok"));
  CHECK(has(text, "via lte"));
  CHECK(has(text, "agent 3 \"TwinOak Risskov companion\", transport native, budget 2/min gap 20 s, poll 3600 s, "
                  "serverTime 2026-09-23T14:00:01Z"));
  CHECK_FALSE(has(text, kTokenBody));
  REQUIRE(rig.http.server.requests.size() == 1);
  CHECK(rig.http.server.requests[0].path == "/agent/v1/config");
  CHECK(rig.http.server.requests[0].header("Authorization") == std::string("Bearer ") + kToken);
  CHECK(rig.http.server.requests[0].header("X-Agent-Version") == "0.4.0+p4test");
  // The uplink's own buffers hold no byte of it afterwards, as every call leaves them.
  const std::string image(reinterpret_cast<const char*>(&rig.server), sizeof rig.server);
  CHECK(image.find(kTokenBody) == std::string::npos);
}

TEST_CASE("agent probe: a 401 is printed as the status and the sentence, and a TLS failure as no reply") {
  Rig rig;
  rig.lteUp();
  rig.http.server.statuses["/agent/v1/config"] = sim::StatusRule{401, "", "{\"title\":\"Unauthorized\"}"};
  std::string text = rig.run("agent probe");
  CHECK(has(text, "-> http 401 not ok"));
  CHECK(has(text, "http 401: the token was refused"));
  CHECK_FALSE(has(text, "Unauthorized"));  // the body is never printed
  CHECK_FALSE(has(text, kTokenBody));

  rig.http.server.statuses["/agent/v1/config"] = sim::StatusRule{429, "45", ""};
  text = rig.run("agent probe");
  CHECK(has(text, "-> http 429 not ok"));
  CHECK(has(text, "Retry-After: 45 s"));

  rig.http.server.statuses.clear();
  rig.http.socket.refuse_tls = true;
  text = rig.run("agent probe");
  CHECK(has(text, "error: agent probe: no reply: the TLS handshake failed"));
  CHECK_FALSE(has(text, kTokenBody));
}

TEST_CASE("agent probe is refused while the uplink is down, and a build without an agent uplink says so") {
  Rig rig;
  std::string text = rig.run("agent probe");
  CHECK(has(text, "error: the uplink is"));
  CHECK(has(text, "'lte up' first"));
  CHECK(rig.http.socket.opens == 0);

  rig.withoutAgent();
  text = rig.run("agent url");
  CHECK(has(text, "this build has no agent uplink"));
  text = rig.run("agent probe");
  CHECK(has(text, "this build has no agent uplink"));
}
