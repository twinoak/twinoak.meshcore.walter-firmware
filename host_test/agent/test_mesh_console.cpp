// `rak mesh status <key>` on the console (task p4-02): the probe of components/agent/ over the
// mesh client, against the scripted radio of task p1-02 answering out of the specification's
// fixtures - so what is held here is the console's words and the requests on the wire, with the
// reply the fixtures' own (`mesh reply: a response that came back as a path`, whose twenty bytes
// are not a status reply the library reads, which the console says).
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/wire.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "sim/scripted_radio.h"
#include "walter_agent/link_mesh.h"
#include "walter_agent/mesh_probe.h"

namespace link = core::link;
using core::board::kKillTiming;
using core::board::RadioPins;
using core::console::Console;
using core::health::Health;
using core::health::ResetReason;
using core::health::WakeCause;

namespace {

constexpr const char* kKey = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";

struct Rig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client client{port, clock, wall, slots};
  agent::LinkMesh mesh{client, clock};
  agent::LinkMeshProbe probe{mesh};
  Console console{out,     radio,   health,  clock,   restart, client,  slots,   nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr, &probe};

  Rig() {
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(kKillTiming.settle_ms);
    radio.safe();
    REQUIRE(client.hello().ok());
  }

  std::string run(const std::string& line) {
    out.clear();
    console.runLine(line.c_str());
    return out.text;
  }

  std::size_t requestsOf(std::uint8_t type) const {
    std::size_t n = 0;
    for (const sim::ScriptedRadio::Seen& one : rak.requests) n += one.type == type;
    return n;
  }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("rak mesh status: the key is checked before anything is sent") {
  Rig rig;
  const std::size_t before = rig.rak.requests.size();
  CHECK(contains(rig.run("rak mesh"), "rak mesh needs an operation"));
  CHECK(contains(rig.run("rak mesh ping"), "unknown operation"));
  CHECK(contains(rig.run("rak mesh status"), "needs the neighbour's key"));
  CHECK(contains(rig.run("rak mesh status a1b2"), "64 hex characters"));
  CHECK(contains(rig.run("rak mesh status zz" + std::string(62, 'a')), "64 hex characters"));
  CHECK(rig.rak.requests.size() == before);
}

TEST_CASE("rak mesh status: a reply that came is printed with the record's own fields and client_tx") {
  Rig rig;
  // The fixtures' reply event carries the tag the fixtures' send response carries (1791720051),
  // so the radio's answer to SEND_REQUEST is the one this event answers.
  rig.rak.sendEvent("mesh reply: a response that came back as a path");
  const std::string text = rig.run(std::string("rak mesh status ") + kKey);
  CHECK(contains(text, "one status request over the mesh client"));
  CHECK(contains(text, "sent as a direct packet, tag 0x6acb7a73"));
  CHECK(contains(text, "reply after 4210 ms (via a returned path, snr 25/4 dB, rssi -97 dBm)"));
  CHECK(contains(text, "not a status reply this firmware reads (20 bytes)"));
  CHECK(contains(text, "client_tx 42 -> 42 (+0)"));  // the fixtures' counters are constant
  // The wire: counters, the job, the send, the counters again. No PATH_GET: the fixtures' send
  // response says the request went *direct*, and only a flood's reply can have taught a path.
  CHECK(rig.requestsOf(link::type::kJobBegin) == 1);
  CHECK(rig.requestsOf(link::type::kSendRequest) == 1);
  CHECK(rig.requestsOf(link::type::kJobEnd) == 1);
  CHECK(rig.requestsOf(link::type::kMeshCounters) == 2);
  CHECK(rig.requestsOf(link::type::kReplyGet) == 0);
  CHECK(rig.requestsOf(link::type::kPathGet) == 0);
  CHECK(rig.client.eventsQueued() == 0);
}

TEST_CASE("rak mesh status: silence is the runner's flood wait, then REPLY_GET, then the wait ended") {
  Rig rig;
  const std::uint64_t before = rig.clock.now_ms;
  const std::string text = rig.run(std::string("rak mesh status ") + kKey);
  CHECK(contains(text, "no reply: the record says pending after 1200 ms"));  // the fixtures' REPLY_GET
  CHECK(contains(text, "client_tx 42 -> 42 (+0)"));
  // Sixteen seconds and the margin on this end's clock, then one REPLY_GET, then JOB_BEGIN again
  // so that the RAK is not left waiting, then the job ended.
  const std::uint64_t waited = rig.clock.now_ms - before;
  CHECK(waited >= 16000 + agent::kReplyMarginMs);
  CHECK(waited < 16000 + agent::kReplyMarginMs + 2000);
  CHECK(rig.requestsOf(link::type::kReplyGet) == 1);
  CHECK(rig.requestsOf(link::type::kJobBegin) == 2);
  CHECK(rig.requestsOf(link::type::kJobEnd) == 1);
  CHECK(rig.mesh.linkCounters().cancelled == 1);
}

TEST_CASE("rak mesh status: the link's refusals are the console's words") {
  Rig rig;
  rig.rak.refuseType(link::type::kJobBegin, link::status::kDenied, 1);
  CHECK(contains(rig.run(std::string("rak mesh status ") + kKey),
                 "error: rak mesh status: JOB_BEGIN: refused: denied: this role refuses it"));
  rig.rak.refuseType(link::type::kSendRequest, link::status::kFailed, 1);
  CHECK(contains(rig.run(std::string("rak mesh status ") + kKey),
                 "error: rak mesh status: SEND_REQUEST: refused: failed: tried and failed"));
  CHECK_MESSAGE(rig.requestsOf(link::type::kJobEnd) == 1, "the job that was begun is ended");
}

TEST_CASE("rak mesh status: a build without a mesh client says so") {
  Rig rig;
  Console bare{rig.out, rig.radio, rig.health, rig.clock, rig.restart, rig.client, rig.slots};
  rig.out.clear();
  bare.runLine((std::string("rak mesh status ") + kKey).c_str());
  CHECK(contains(rig.out.text, "this build has no mesh client"));
}
