// The mesh over the link, held to the things the runner's scenarios do not say (task p4-02):
// the lost event, the lost request, the RAK's job timeout, a password read back as zeros from
// every buffer it crossed, the local door for each core type, client_tx exact, the radio
// restarting under a job, and the shape difference between §6.2's timeout at the send and the
// library's at the await.
//
// The harness is the runner suite's own - the library's JobRunner, FakeUplink and FakePlatform -
// with its FakeMesh shadowed by shim/support/FakeMesh.h: the same simulated stock repeater, now
// behind a simulated RAK (host_test/sim/sim_rak.h) and the agent's LinkMesh. `mesh` is the whole
// road; `mesh.world()` is the link under it, `mesh.repeater()` the repeater on the far side.
#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <agent/IMesh.h>
#include <agent/JobEnvelopes.h>
#include <agent/JobRunner.h>
#include <agent/ReplyCodec.h>

#include <support/FakeMesh.h>
#include <support/FakePlatform.h>
#include <support/FakeUplink.h>
#include <support/Fixtures.h>
#include <support/Jobs.h>

#include "core/link/codec/messages.h"
#include "core/link/codec/wire.h"
#include "core/version.h"
#include "sim/local_door.h"
#include "sim/mesh_world.h"
#include "walter_agent/link_mesh.h"

namespace {

// Inside the anonymous namespace: at file scope `link` would be POSIX's link(2), which <gtest>
// brings in through <unistd.h>.
namespace link = core::link;
namespace lc = core::link::client;

constexpr uint32_t kNow = 1790000000;
constexpr uint32_t kLease = kNow + 1800;

const std::string& repeater() { return fake::repeaterKey(); }
const std::string& self() { return fake::selfKey(); }

// One top-level scalar or string member of a result envelope, without a parser. A string's
// escapes are kept as they are, so a needle with a quote in it is not found; none here has one.
std::string member(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  size_t at = found + needle.size();
  if (json[at] == '"') {
    size_t end = at + 1;
    while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
    return json.substr(at + 1, end - at - 1);
  }
  size_t end = at;
  while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
  return json.substr(at, end - at);
}

// The whole object or array of a member, braces and all.
std::string objectOf(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  size_t at = found + needle.size();
  if (json[at] != '{' && json[at] != '[') return "";
  int depth = 0;
  bool inString = false;
  for (size_t i = at; i < json.size(); i++) {
    const char c = json[i];
    if (inString) {
      if (c == '\\') i++;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      if (--depth == 0) return json.substr(at, i - at + 1);
    }
  }
  return "";
}

bool holds(const void* object, size_t size, const std::string& needle) {
  const std::string image(static_cast<const char*>(object), size);
  return image.find(needle) != std::string::npos;
}

bool keyBytes(const std::string& hex, uint8_t out[32]) {
  if (hex.size() != 64) return false;
  for (size_t i = 0; i < 32; i++) {
    out[i] = static_cast<uint8_t>((fake::hexDigit(hex[i * 2]) << 4) | fake::hexDigit(hex[i * 2 + 1]));
  }
  return true;
}

// The unit's own repeater's console, as the local door reaches it (§5.7): what CommonCLI and
// MyMesh answer at the pin, in the words FakeMesh.h cites for the same lines, over the same
// default configuration. `reboot` restarts the node and never answers.
std::map<std::string, std::string> g_config = {
    {"name", "Grenaa Chimney #1"}, {"lat", "56.4167"}, {"lon", "10.8833"}, {"advert.interval", "240"},
    {"flood.advert.interval", "12"}, {"repeat", "on"}, {"dutycycle", "10.0%"}, {"flood.max", "64"},
    {"flood.max.unscoped", "3"}, {"tx", "22"}, {"path.hash.mode", "1"}, {"multi.acks", "0"},
    {"allow.read.only", "off"}, {"agc.reset.interval", "0"}, {"freq", "869.618"}, {"guest.password", ""},
};
std::vector<std::string> g_setperms;
int g_adverts = 0;
uint32_t g_clockSet = 0;

bool localConsole(const std::string& line, std::string* reply) {
  if (line.rfind("reboot", 0) == 0) return false;
  if (line.rfind("get ", 0) == 0) {
    const std::string key = line.substr(4);
    const auto it = g_config.find(key);
    *reply = it == g_config.end() ? "??: " + key : "> " + it->second;
    return true;
  }
  if (line.rfind("set ", 0) == 0) {
    const size_t sp = line.find(' ', 4);
    if (sp != std::string::npos) g_config[line.substr(4, sp - 4)] = line.substr(sp + 1);
    *reply = "OK";
    return true;
  }
  if (line.rfind("setperm ", 0) == 0) {
    g_setperms.push_back(line);
    *reply = "OK";
    return true;
  }
  if (line.rfind("password ", 0) == 0) {
    *reply = "password now: " + line.substr(9);
    return true;
  }
  if (line.rfind("advert.zerohop", 0) == 0) {
    g_adverts++;
    *reply = "OK - zerohop advert sent";
    return true;
  }
  if (line.rfind("advert", 0) == 0) {
    g_adverts++;
    *reply = "OK - Advert sent";
    return true;
  }
  if (line.rfind("time ", 0) == 0) {
    g_clockSet = static_cast<uint32_t>(std::strtoul(line.c_str() + 5, nullptr, 10));
    *reply = "OK - clock set: 14:00 - 23/9/2026 UTC";
    return true;
  }
  if (line == "ver") {
    *reply = "v1.17.1 (Build: 27 Sep 2025)";
    return true;
  }
  if (line == "board") {
    *reply = "Heltec";
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

// For the restart-under-a-wait case: the pump the library hands the mesh is a plain function.
sim::MeshWorld* g_restartWorld = nullptr;
bool g_restarted = false;
void restartTheRakOnce() {
  if (g_restartWorld != nullptr && !g_restarted) {
    g_restarted = true;
    g_restartWorld->rak.restart(true);
  }
}

class LinkMeshTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::LogCapture::the().begin();
    platform.fakeClock().setFromNetwork(kNow);
    mesh.setKey(repeater());
    mesh.setSelfKey(self());
    mesh.setAdminPassword("adm1n-pass");
    mesh.setRepeaterClock(kNow - 600);
    server.setLinkUp(true);
    server.setServerTime(kNow);
    runner.begin(mesh, server, platform, nullptr);
    runner.setIdentity(self().c_str());
    mm::AgentBudget budget;
    budget.maxTransactionsPerMinute = 60;
    budget.minGapSeconds = 1;
    runner.setBudget(budget);
    g_setperms.clear();
    g_adverts = 0;
    g_clockSet = 0;
    g_restartWorld = nullptr;
    g_restarted = false;
  }

  std::string run(const fake::Job& job) {
    server.queue(job.json());
    const int ran = runner.claimAndRun();
    EXPECT_EQ(ran, 1) << "the claim did not hand over the job";
    EXPECT_FALSE(server.results().empty()) << "no result was posted";
    return server.results().empty() ? std::string() : server.results().back().body;
  }

  std::string outcomeOf(const std::string& body) const { return member(body, "outcome"); }
  std::string logOf(const std::string& body) const { return member(body, "log"); }
  int transactionsOf(const std::string& body) const { return std::atoi(member(body, "transactions").c_str()); }

  sim::MeshWorld& world() { return mesh.world(); }
  agent::LinkMesh& link() { return mesh.link(); }
  sim::SimRak& rak() { return mesh.world().rak; }

  // The unit's own repeater is the one the jobs address: every job goes through the local door.
  void makeItLocal() {
    uint8_t key[32];
    ASSERT_TRUE(keyBytes(repeater(), key));
    link().setOwnKey(key);
    rak().console = localConsole;
  }

  // ... and the radio is one at **link 1.2** (task p9-06): the capability bit in the session, and
  // the repeater's own request handler behind the RAK (§5.9). `makeItLocal()` alone leaves a 1.1
  // radio, which is every case written before this task and the first unit's own radio until its
  // pile 3.9 reaches it.
  void makeIt12(int neighbours = 11) {
    makeItLocal();
    door.fillNeighbours(neighbours);
    rak().local_door = door.handler();
    rak().radio.caps |= link::cap::kLocalRequest;
    world().client.radioWasReset();
    ASSERT_TRUE(world().client.hello().ok());
    ASSERT_TRUE(link().answersRequestsLocally());
  }

  // ... and the radio is one at **link 1.3** (task p14-01, §6.5): the `FLOOD_SCOPE` capability in
  // the session, a body that says minor 3, and `default_scope` as its `region default` - empty
  // for a radio that holds none. `makeIt13()` alone leaves the job a mesh job; call it before
  // `makeItLocal()` or after, the two are about different things.
  void makeIt13(const std::string& default_scope) {
    rak().radio.speakLink13(default_scope);
    world().client.radioWasReset();
    ASSERT_TRUE(world().client.hello().ok());
    ASSERT_EQ(world().client.session().proto_minor, link::kFloodScopeMinor);
    ASSERT_TRUE(world().client.hasCapability(link::cap::kFloodScope));
  }

  // The radio of 1.2 this file has always used, said out loud: the bit is not in `caps`, the
  // body says what its band implies, and §6.5's request is `ERR_UNSUPPORTED`.
  void makeIt12Link() {
    rak().radio.speakLink12();
    world().client.radioWasReset();
    ASSERT_TRUE(world().client.hello().ok());
    ASSERT_LT(world().client.session().proto_minor, link::kFloodScopeMinor);
    ASSERT_FALSE(world().client.hasCapability(link::cap::kFloodScope));
  }

  // The frames the Walter sent since `from`, of the types §6.5 is about: every `SEND_*` and every
  // `FLOOD_SCOPE`, in order, each as `type` and - for a FLOOD_SCOPE - the `mode` it asked for.
  // Retries are included, because §3 says a retry is the same frame and the order is the point.
  struct Frame {
    uint8_t type = 0;
    uint8_t mode = 0;  // FLOOD_SCOPE's `mode`; 0 for a send
  };
  std::vector<Frame> scopeAndSendFrames(size_t from = 0) {
    std::vector<Frame> out;
    const std::vector<sim::ScriptedRadio::Seen>& seen = rak().radio.requests;
    for (size_t i = from; i < seen.size(); i++) {
      const uint8_t type = seen[i].type;
      const bool send = type >= link::type::kSendLogin && type <= link::type::kSendTrace;
      if (!send && type != link::type::kFloodScope) continue;
      Frame one;
      one.type = type;
      if (type == link::type::kFloodScope && !seen[i].payload.empty()) one.mode = seen[i].payload[0];
      out.push_back(one);
    }
    return out;
  }
  size_t framesSoFar() { return rak().radio.requests.size(); }

  sim::FixtureLocalDoor door;

  fake::FakePlatform platform;
  fake::FakeMesh mesh{platform};
  fake::FakeUplink server{platform};
  mm::JobRunner runner;
};

// ---------------------------------------------------------------------------------------------
// The road itself

TEST_F(LinkMeshTest, AStatusReadIsOneRequestOfSection6PerCall) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(100, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 2);
  // The wire: a job begun, a login, a request, the job ended, the counters read.
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 1u);
  EXPECT_EQ(world().requestsOf(link::type::kSendLogin), 1u);
  EXPECT_EQ(world().requestsOf(link::type::kSendRequest), 1u);
  EXPECT_EQ(world().requestsOf(link::type::kJobEnd), 1u);
  EXPECT_GE(world().requestsOf(link::type::kMeshCounters), 1u);
  // The reply came as the event, both times, and nothing had to be asked for.
  EXPECT_EQ(rak().events_sent, 2u);
  EXPECT_EQ(world().requestsOf(link::type::kReplyGet), 0u);
  EXPECT_EQ(link().linkCounters().events, 2u);
  // client_tx is the runner's two transactions, exactly, and the mesh reports it as its tx.
  EXPECT_EQ(rak().client_tx(), 2u);
  EXPECT_EQ(mesh.counters().tx, 2u);
  EXPECT_EQ(mesh.repeater().sendCount(), 2);
  // The data is the repeater's, through the framing: status-response.hex's battery.
  EXPECT_NE(body.find("\"batteryMv\":4012"), std::string::npos) << body;
}

TEST_F(LinkMeshTest, ALostEventCostsOneReplyGetAndNeverTheReply) {
  mesh.grant(self(), mm::kPermReadOnly);
  rak().lose_events = 1;  // the login's outcome reaches the RAK's record and no further
  const std::string body = run(fake::Job(101, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(rak().events_lost, 1u);
  EXPECT_EQ(rak().events_sent, 1u);
  EXPECT_EQ(world().requestsOf(link::type::kReplyGet), 1u) << "one REPLY_GET for the one lost event";
  EXPECT_EQ(link().linkCounters().reply_gets, 1u);
  EXPECT_EQ(link().linkCounters().lost_events, 1u);
  EXPECT_EQ(link().linkCounters().cancelled, 0u) << "the record was final: nothing to cancel";
  EXPECT_EQ(mesh.logins(), 1) << "the login was not sent again";
  EXPECT_EQ(mesh.counters().tx, 2u);
}

TEST_F(LinkMeshTest, ALostRequestIsTheLinksOwnRetryAndStillOnePacket) {
  mesh.grant(self(), mm::kPermReadOnly);
  rak().radio.ignoreType(link::type::kSendRequest, 1);  // the first SEND_REQUEST frame is lost
  const std::string body = run(fake::Job(102, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(world().requestsOf(link::type::kSendRequest), 2u) << "the same frame, sent again (§3)";
  EXPECT_EQ(world().client.counters().retries, 1u);
  EXPECT_EQ(rak().client_tx(), 2u) << "one packet for the request, whatever the link did";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetStatus), 1) << "the repeater was asked once";
  EXPECT_EQ(transactionsOf(body), 2);
}

TEST_F(LinkMeshTest, ARetryOfASendTransmitsNothing) {
  // §3's remembered response: the response to a SEND_* lost on the way back must not become a
  // second packet. The RAK's answer to the login is silenced once by making the radio's answer
  // arrive late, so the client resends and the radio answers the retry from memory.
  mesh.grant(self(), mm::kPermReadOnly);
  rak().radio.ignoreType(link::type::kSendLogin, 1);
  const std::string body = run(fake::Job(103, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(world().requestsOf(link::type::kSendLogin), 2u);
  EXPECT_EQ(mesh.logins(), 1) << "one login packet on the air";
  EXPECT_EQ(rak().client_tx(), 2u);
}

TEST_F(LinkMeshTest, TheRakEndsAJobAfterTwoMinutesOfLinkSilence) {
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  ASSERT_TRUE(rak().jobBegun());
  platform.advance(link::kJobIdleEndMs + 1000);  // the Walter crashed, or slept: no request at all
  uint32_t tag = 0;
  EXPECT_EQ(mesh.sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag), mm::SendResult::Failed);
  EXPECT_EQ(rak().jobs_timed_out, 1u);
  EXPECT_FALSE(rak().jobBegun());
  EXPECT_EQ(link().lastFault(), lc::Fault::kRefused);
  EXPECT_EQ(link().lastStatus(), link::status::kState) << "§6.2: a send without a job is ERR_STATE";
  EXPECT_EQ(rak().client_tx(), 0u);
  mesh.endJob();
}

// ---------------------------------------------------------------------------------------------
// The password's path

TEST_F(LinkMeshTest, APasswordIsZeroInEveryBufferItCrossedAndOnTheWireOnce) {
  const std::string secret = "hunter2-ish";
  mesh.setAdminPassword(secret);  // the repeater's, so that the login is answered
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 7;
  EXPECT_NE(mesh.sendLogin(secret.c_str(), tag), mm::SendResult::Failed);
  EXPECT_EQ(tag, 0u) << "a login's tag is 0: the reply carries the peer's timestamp";

  // Every buffer this end copied it to: the mesh client's own, the link client's, the RAK's.
  EXPECT_FALSE(holds(&link(), sizeof(agent::LinkMesh), secret)) << "LinkMesh keeps a copy";
  EXPECT_FALSE(holds(&world().client, sizeof(lc::Client), secret)) << "the link client keeps a copy";
  EXPECT_FALSE(holds(rak().passwordBuffer(), rak().passwordBufferSize(), secret)) << "the RAK keeps a copy";
  for (size_t i = 0; i < rak().passwordBufferSize(); i++) EXPECT_EQ(rak().passwordBuffer()[i], 0);
  // And it did cross: the wire carried it, once, as the SEND_LOGIN's rest.
  int carried = 0;
  for (const sim::ScriptedRadio::Seen& one : rak().radio.requests) {
    if (one.type != link::type::kSendLogin) continue;
    const std::string payload(one.payload.begin(), one.payload.end());
    if (payload.find(secret) != std::string::npos) carried++;
  }
  EXPECT_EQ(carried, 1);
  // Nothing logged it (the capture holds every line the library formatted).
  EXPECT_FALSE(fake::LogCapture::the().holds(secret));
  EXPECT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.endJob();
}

TEST_F(LinkMeshTest, ASensitiveCliLineTakesTheSamePathWithTheFlagBit) {
  const std::string secret = "n3w-adm1n-pass";
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  ASSERT_NE(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  const std::string line = "password " + secret;
  uint32_t timestamp = 0;
  EXPECT_NE(mesh.sendCli(line.c_str(), true, timestamp), mm::SendResult::Failed);
  EXPECT_NE(timestamp, 0u) << "the sender timestamp the RAK drew comes back (§6.2)";

  EXPECT_FALSE(holds(&link(), sizeof(agent::LinkMesh), secret));
  EXPECT_FALSE(holds(&world().client, sizeof(lc::Client), secret));
  EXPECT_FALSE(holds(rak().cliBuffer(), rak().cliBufferSize(), secret));
  const sim::ScriptedRadio::Seen& sent = rak().radio.requests.back();
  ASSERT_EQ(sent.type, link::type::kSendCli);
  ASSERT_GE(sent.payload.size(), 6u);
  EXPECT_EQ(sent.payload[4] & link::send_cli_flag::kSensitive, link::send_cli_flag::kSensitive)
      << "§6.2: bit 0 of flags is sensitive";
  EXPECT_FALSE(fake::LogCapture::the().holds(secret));

  // The repeater's echo ("password now: ...") is the reply the library redacts; it is held for
  // the await and gone with the job.
  EXPECT_TRUE(mesh.awaitText(16000, nullptr));
  mesh.endJob();
  EXPECT_FALSE(holds(&link(), sizeof(agent::LinkMesh), secret)) << "the reply is cleared with the job";
}

TEST_F(LinkMeshTest, APasswordSetRecordsNoValueAnywhereThroughTheLink) {
  const std::string secret = "n3w-adm1n-pass";
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(104, "password.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withNewPassword(secret)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"field\":\"adminPassword\"}"));
  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.adminPassword(), secret) << "the repeater on the far side stored it";
  EXPECT_EQ(body.find(secret), std::string::npos);
  EXPECT_FALSE(fake::LogCapture::the().holds(secret));
  EXPECT_FALSE(fake::LogCapture::the().holds("adm1n-pass"));
  EXPECT_FALSE(holds(&link(), sizeof(agent::LinkMesh), secret));
  EXPECT_FALSE(holds(&world().client, sizeof(lc::Client), secret));
}

// ---------------------------------------------------------------------------------------------
// The timeout's two ends

TEST_F(LinkMeshTest, TheSendCarriesACeilingAndTheWaltersOwnTimerDecides) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.beSilentAlways();
  const uint64_t before = platform.millis();
  const std::string body =
      run(fake::Job(105, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_EQ(mesh.logins(), 2) << "the login sequence's two tries";
  // Every send carried the ceiling, and the wait was the runner's: the claim's two-hop path at
  // perHopMs, 12 s (the longer of that and loginMs), plus the margin - twice.
  for (const sim::SimRak::Sent& one : rak().sent) EXPECT_EQ(one.timeout_ms, agent::kSendTimeoutCeilingMs);
  const uint64_t waited = platform.millis() - before;
  EXPECT_GE(waited, 2u * (12000u + agent::kReplyMarginMs));
  EXPECT_LT(waited, 2u * (12000u + agent::kReplyMarginMs) + 2000u);
  // Each silence: the event did not come, REPLY_GET said pending, JOB_BEGIN ended the wait - so
  // the RAK was never busy for the next send.
  EXPECT_EQ(link().linkCounters().reply_gets, 2u);
  EXPECT_EQ(link().linkCounters().cancelled, 2u);
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 3u);
  EXPECT_EQ(rak().refused_busy, 0u);
  EXPECT_EQ(rak().client_tx(), 2u);
}

TEST_F(LinkMeshTest, AReplyThatArrivesAfterThisEndGaveUpIsDroppedAndCounted) {
  mesh.grant(self(), mm::kPermReadOnly);
  rak().event_delay_ms = 30000;  // the outcome is in the record at once; the event is slow
  const std::string body = run(fake::Job(106, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok") << "REPLY_GET found the record final: " << body;
  EXPECT_EQ(link().linkCounters().reply_gets, 2u);
  EXPECT_EQ(link().linkCounters().late_replies, 0u);
  // The two events arrive long after; the pump takes them and drops them.
  platform.advance(60000);
  mesh.loop();
  EXPECT_EQ(link().linkCounters().late_replies, 2u);
  EXPECT_EQ(world().client.eventsQueued(), 0u);
}

// ---------------------------------------------------------------------------------------------
// A reply that came home late, and what it costs a Walter that a Heltec does not pay
//
// The library's own rule is *only a login reply answers a login* and *only a reply carrying this
// line's prefix is its answer* (its tasks p1.10-47 and p1.10-49): a reply that missed its own
// wait and turned up in the next one is dropped, and that wait goes on. Through this link the
// wait cannot go on. §6.2's radio awaits "until a matching reply arrives or the time is up" and
// §6.3 reports that outcome **once**, so an exchange that was handed a late reply is over and the
// reply it was waiting for is lost - and what recovers it is the job's own next try, one
// transaction more than the Heltec pays. The reading is the same; the airtime is not.
//
// These two are the Walter's half of the library's own
// `ALateRequestReplyDuringTheReLoginIsDroppedAndTheJobRunsOn` and
// `AConsoleReplyThatCameTooLateIsNotStoredAsTheNextLinesValue`, which are the two cases of its
// runner suite that host_test/CMakeLists.txt does not run through this shim, and for this reason.

TEST_F(LinkMeshTest, ALateRequestReplyEndsTheReLoginsExchangeAndCostsOneMoreTry) {
  mesh.grant(self(), mm::kPermReadOnly);
  // The first job opens the session, as the Heltec's own case has it.
  run(fake::Job(24141, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  ASSERT_EQ(mesh.logins(), 1);
  mesh.forgetSends();

  // The second finds the session live, and its request is answered past its own wait: the radio
  // holds the answer and hands it to the next exchange that §6.2 says may read it, which is the
  // re-login - a login awaits tag 0, "the next reply from the peer".
  mesh.deliverTheNextReplyLate(1);
  const std::string body =
      run(fake::Job(24142, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << "the job runs on and the reading is the repeater's: " << body;
  EXPECT_NE(body.find("\"batteryMv\":4012"), std::string::npos) << body;
  EXPECT_EQ(member(body, "loggedInAs"), "") << "nothing was granted by that reply";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the late reply to a request this job sent arrived instead"), std::string::npos) << log;
  EXPECT_NE(log.find("logged in as read-only"), std::string::npos) << log;
  EXPECT_EQ(log.find("refused"), std::string::npos) << "nothing refused anything: " << log;
  // The cost: the first login try is the exchange the late reply ended, so there is a second -
  // where the Heltec's one wait had found the login reply after dropping the late one.
  EXPECT_EQ(mesh.logins(), 2) << "the re-login twice: the late reply ended the first exchange";
  EXPECT_EQ(rak().late_read, 1u) << "the radio handed the late reply to an exchange that matched it";
  EXPECT_EQ(rak().late_dropped, 0u);
  EXPECT_EQ(link().linkCounters().mismatched, 0u) << "a login awaits tag 0, so this end read it and the library judged it";
}

TEST_F(LinkMeshTest, ALateConsoleReplyEndsTheNextLinesExchangeAndTheValueIsStillItsOwn) {
  // `get name` is answered too late for its own wait and for its retry, so the reply turns up
  // while `get tx` is awaited. The prefix is what tells it from `get tx`'s answer; without it the
  // node's name would be stored as the transmit power.
  mesh.deliverTheNextLineRepliesLate("get name", 1, /*waits=*/2);

  const std::string body = run(fake::Job(12390, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"tx\"]}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::string values = objectOf(body, "values");
  EXPECT_NE(values.find("\"tx\":\"22\""), std::string::npos) << "read from `get tx`'s own reply: " << values;
  EXPECT_EQ(values.find("\"tx\":\"Grenaa Chimney #1\""), std::string::npos) << "the swap this prefix exists for: " << values;
  EXPECT_NE(values.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos) << values;
  EXPECT_EQ(values.find("|"), std::string::npos) << "and no value keeps a prefix: " << values;
  const std::string log = logOf(body);
  EXPECT_NE(log.find("a console reply that does not carry this line's prefix arrived instead"), std::string::npos) << log;
  // The cost, again one try: `get tx`'s first exchange was ended by the reply it dropped.
  EXPECT_EQ(mesh.timesSent("get name"), 2) << "no reply once is retried once";
  EXPECT_EQ(mesh.timesSent("get tx"), 2) << "and the line whose wait the late reply ended is retried too";
  EXPECT_EQ(rak().late_read, 1u);
}

// ---------------------------------------------------------------------------------------------
// A line nobody waits for

TEST_F(LinkMeshTest, ALineNobodyWaitsForDoesNotLeaveTheNextOneErrBusy) {
  // The region loader is the one sequence whose lines go out bare and unawaited (the library's
  // `agent/ConfigApply.h`: `awaitReply = false` for a line the firmware answers with nothing at
  // all). §6.2's radio is awaiting from the moment that packet was queued, so the line after it
  // would be `ERR_BUSY` - and the apply would stop at the second line of the load with *the
  // packet could not be built*, which is what this end's `JOB_BEGIN` again prevents (§6.1).
  const std::string payload =
      "{\"commands\":["
      "{\"key\":\"regions\",\"command\":\"region load\",\"lines\":["
      "{\"line\":\"region load\",\"expect\":null,\"escape\":false},"
      "{\"line\":\" dk F\",\"expect\":null,\"escape\":false},"
      "{\"line\":\" \",\"expect\":\"OK - loaded\",\"escape\":true},"
      "{\"line\":\"region save\",\"expect\":\"OK\"}]}"
      "],\"planHash\":\"deadbeef\"}";
  const std::string body = run(fake::Job(126, "config.apply")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 3600)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(payload));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.repeater().regionsLoaded(), 1) << "the map was loaded, which takes every line: " << body;
  EXPECT_NE(body.find("OK - loaded 1 regions"), std::string::npos) << body;
  EXPECT_EQ(body.find("could not be built"), std::string::npos) << body;
  // Twice: the line after `region load` and the line after ` dk F`, both of which this end had
  // sent and never waited for.
  EXPECT_EQ(link().linkCounters().never_awaited, 2u);
  EXPECT_LE(link().linkCounters().never_awaited, link().linkCounters().cancelled);
  EXPECT_EQ(rak().refused_busy, 0u) << "and the radio never had to refuse one";
}

// ---------------------------------------------------------------------------------------------
// The radio restarts

TEST_F(LinkMeshTest, ABootIdChangeUnderAJobForgetsEverything) {
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  ASSERT_NE(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  EXPECT_TRUE(link().jobBegun());

  rak().restart(true);  // a new boot_id and an EV_BOOT (§5.1)
  mesh.loop();
  EXPECT_EQ(link().linkCounters().restarts, 1u);
  EXPECT_FALSE(link().jobBegun()) << "every piece of per-boot state is gone";
  EXPECT_FALSE(link().hasPath());
  // The next send is the RAK's own refusal: no job (§6.2), which the library ends the job on.
  EXPECT_EQ(mesh.sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag), mm::SendResult::Failed);
  EXPECT_EQ(link().lastStatus(), link::status::kState);
  mesh.endJob();
}

TEST_F(LinkMeshTest, ARestartUnderAWaitEndsTheWaitWithoutAskingTheRak) {
  mesh.beSilentAlways();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  ASSERT_NE(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  g_restartWorld = &world();
  EXPECT_FALSE(mesh.awaitReply(0, 16000, restartTheRakOnce));
  EXPECT_TRUE(g_restarted);
  EXPECT_EQ(link().linkCounters().restarts, 1u);
  EXPECT_EQ(link().linkCounters().reply_gets, 0u) << "a RAK that just booted holds nothing to ask for";
  EXPECT_FALSE(rak().awaiting());
  mesh.endJob();
}

// ---------------------------------------------------------------------------------------------
// The path

TEST_F(LinkMeshTest, AReturnedPathIsLearnedThroughPathGet) {
  mesh.setGuestPassword("gu3st-pass");
  mesh.setReturnPath({0x11, 0x22, 0x33, 0x44}, 2);
  const std::string body = run(fake::Job(107, "discover")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .withNoPath());
  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_NE(body.find("\"hops\":[\"1122\",\"3344\"]"), std::string::npos) << body;
  EXPECT_GE(rak().path_reads, 1u) << "PATH_GET after the flooded login's reply";
  EXPECT_GE(link().linkCounters().path_reads, 1u);
}

TEST_F(LinkMeshTest, ThePathTheClaimCarriedIsTheJobsAndADirectNeighbourIsZeroHops) {
  const uint8_t hashes[] = {0xa1, 0x7f};
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), hashes, 2, 1));
  EXPECT_TRUE(mesh.hasPath());
  EXPECT_EQ(mesh.pathHopCount(), 2);
  char hops[8][8];
  ASSERT_EQ(mesh.describePath(hops, 8), 2);
  EXPECT_STREQ(hops[0], "a1");
  EXPECT_STREQ(hops[1], "7f");
  EXPECT_EQ(mesh.repeater().pathHopCount(), 2) << "the RAK's repeater holds the same path";
  mesh.setDirectPath();
  EXPECT_TRUE(mesh.hasPath());
  EXPECT_EQ(mesh.pathHopCount(), 0);
  EXPECT_EQ(mesh.describePath(hops, 8), 0);
  EXPECT_EQ(world().requestsOf(link::type::kPathSet), 1u);
  mesh.forgetPath();
  EXPECT_FALSE(mesh.hasPath());
  EXPECT_EQ(world().requestsOf(link::type::kPathSet), 2u);
  mesh.endJob();
}

// ---------------------------------------------------------------------------------------------
// supports(), and the trace

TEST_F(LinkMeshTest, EveryCoreTypeIsCarriedWithASessionAndNoneWithout) {
  for (int i = 0; i < mm::kJobTypeCount; i++) {
    EXPECT_TRUE(mesh.supports(mm::kJobTypes[i])) << mm::kJobTypes[i];
  }
  // §6.2 has SEND_TRACE and §6.3 a trace record: a trace is carried, and the runner reads the
  // round trip and this unit's own hop out of it as it did in the same process.
  const std::string body = run(fake::Job(108, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 1);
  EXPECT_NE(body.find("\"prefix\":\"a1\""), std::string::npos) << body;
  EXPECT_EQ(world().requestsOf(link::type::kSendTrace), 1u);

  world().client.radioWasReset();  // no session: nothing can be carried, nothing is advertised
  for (int i = 0; i < mm::kJobTypeCount; i++) {
    EXPECT_FALSE(mesh.supports(mm::kJobTypes[i])) << mm::kJobTypes[i];
  }
  world().client.hello();
}

// Task p8-04: the second condition. A session that carries MESH_CLIENT is not enough - until this
// end knows its own key, `beginJob` cannot tell a job for the unit's own repeater from one for a
// stranger, and a job for the own repeater would be addressed over the air to the radio holding
// the conversation (the 6 November fault). So nothing is advertised, and the server routes
// nothing, rather than a job being claimed and then refused.
TEST_F(LinkMeshTest, NoCoreTypeIsCarriedUntilThisEndKnowsItsOwnKey) {
  ASSERT_TRUE(link().hasOwnKey()) << "the harness hands the key over, as a wake does";
  link().clearOwnKey();
  ASSERT_TRUE(world().client.hasCapability(link::cap::kMeshClient)) << "the session still carries it";
  for (int i = 0; i < mm::kJobTypeCount; i++) {
    EXPECT_FALSE(mesh.supports(mm::kJobTypes[i])) << mm::kJobTypes[i];
  }
  // And with the key back, all thirteen again - a stranger's job is carried as it always was.
  uint8_t key[32];
  ASSERT_TRUE(keyBytes(self(), key));
  link().setOwnKey(key);
  for (int i = 0; i < mm::kJobTypeCount; i++) {
    EXPECT_TRUE(mesh.supports(mm::kJobTypes[i])) << mm::kJobTypes[i];
  }
}

TEST_F(LinkMeshTest, OverheardIsZeroBecauseTheLinkCarriesNoSuchCount) {
  EXPECT_EQ(mesh.overheard(), 0u);  // §6.4: a specification gap, in the report
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(109, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(mesh.counters().adverts, 0u);
  EXPECT_EQ(mesh.counters().advertsDropped, 0u);
  EXPECT_NE(link().radioTx(), mesh.counters().tx) << "the radio's tx counts forwards and is not the mesh's";
}

// ---------------------------------------------------------------------------------------------
// client_tx, exact

TEST_F(LinkMeshTest, ClientTxIsTheRunnersTransactionsAcrossAMixOfJobs) {
  mesh.grant(self(), mm::kPermReadOnly);
  int transactions = 0;
  const std::string status =
      run(fake::Job(110, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  transactions += transactionsOf(status);
  EXPECT_EQ(mesh.counters().tx, static_cast<uint32_t>(transactions));
  const std::string cli = run(fake::Job(111, "cli")
                                  .atClass("admin")
                                  .leasedUntil(kLease)
                                  .withAdminPassword("adm1n-pass")
                                  .withPayload("{\"command\":\"set tx 20\"}"));
  transactions += transactionsOf(cli);
  EXPECT_EQ(mesh.counters().tx, static_cast<uint32_t>(transactions));
  const std::string trace = run(fake::Job(112, "trace").atClass("none").leasedUntil(kLease));
  transactions += transactionsOf(trace);
  EXPECT_EQ(mesh.counters().tx, static_cast<uint32_t>(transactions));
  mesh.setGuestPassword("gu3st-pass");
  const std::string neighbours =
      run(fake::Job(113, "neighbours").atClass("guest").leasedUntil(kLease).withGuestPassword("gu3st-pass"));
  transactions += transactionsOf(neighbours);
  EXPECT_EQ(mesh.counters().tx, static_cast<uint32_t>(transactions));

  EXPECT_EQ(transactions, mesh.repeater().sendCount()) << "every transaction was one send on the far side";
  EXPECT_EQ(mesh.logins(), 2) << "status and cli logged in; the admin session the cli job opened "
                                 "served the neighbours read (sessions are reused, EXIT-NODES §5); a "
                                 "trace asks nobody";
  EXPECT_EQ(rak().client_tx(), static_cast<uint32_t>(transactions));
  EXPECT_EQ(world().sendsExecuted(), static_cast<uint32_t>(transactions));
  EXPECT_EQ(mesh.repeater().counters().tx, static_cast<uint32_t>(transactions));
}

// ---------------------------------------------------------------------------------------------
// The local door

TEST_F(LinkMeshTest, TheLocalLoginReplyIsTheFixturesAdminLogin) {
  makeItLocal();
  const auto fixture = fake::body("login-success");  // is_admin(1), prefix(6), ts(4), acl(1), level(1)
  ASSERT_GE(fixture.size(), 13u);
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  EXPECT_TRUE(link().local());
  uint32_t tag = 9;
  EXPECT_EQ(mesh.sendLogin("whatever", tag), mm::SendResult::Direct);
  EXPECT_EQ(tag, 0u);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  const mm::MeshReply& reply = mesh.reply();
  ASSERT_GE(reply.len, 9);
  EXPECT_EQ(reply.data[0], 0x00) << "RESP_SERVER_LOGIN_OK";
  EXPECT_EQ(reply.data[2], fixture[0]) << "is_admin, byte 1 of login-success.hex";
  EXPECT_EQ(reply.data[3], fixture[11]) << "the permission byte, byte 12 of login-success.hex";
  EXPECT_EQ(reply.data[8], fixture[12]) << "FIRMWARE_VER_LEVEL, byte 13 of login-success.hex";
  mm::LoginReply login;
  ASSERT_TRUE(mm::parseLoginReply(reply.data, reply.len, login));
  EXPECT_TRUE(login.isAdmin());
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 0u) << "nothing of the mesh client was touched";
  EXPECT_EQ(world().requestsOf(link::type::kSendLogin), 0u);
  mesh.endJob();
  EXPECT_EQ(world().requestsOf(link::type::kJobEnd), 0u);
  EXPECT_EQ(rak().client_tx(), 0u);
}

TEST_F(LinkMeshTest, LocalModeServesWhatTheLinkCarriesAndFailsTheRest) {
  makeItLocal();
  rak().lpp = fake::afterPrefix("telemetry-response");  // the companion's four channels
  const std::string secret = "n3w-adm1n-pass";

  // -- served over §5.7 and §5.8
  const std::string telemetry =
      run(fake::Job(120, "telemetry").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(telemetry), "ok") << telemetry;
  EXPECT_NE(telemetry.find("\"type\":\"voltage\""), std::string::npos) << telemetry;
  ASSERT_EQ(rak().lpp_masks.size(), 1u);
  EXPECT_EQ(rak().lpp_masks[0], agent::kLocalLppPermissions);

  const std::string cli = run(fake::Job(121, "cli")
                                  .atClass("admin")
                                  .leasedUntil(kLease)
                                  .withAdminPassword("adm1n-pass")
                                  .withPayload("{\"command\":\"set tx 20\"}"));
  EXPECT_EQ(outcomeOf(cli), "ok") << cli;
  EXPECT_NE(objectOf(cli, "data").find("\"reply\":\"OK\""), std::string::npos) << cli;

  const std::string apply = run(fake::Job(122, "config.apply")
                                    .atClass("admin")
                                    .leasedUntil(kLease)
                                    .withAdminPassword("adm1n-pass")
                                    .withPayload("{\"commands\":[{\"key\":\"advert.interval\",\"command\":\"set "
                                                 "advert.interval 180\"}],\"planHash\":\"deadbeef\"}"));
  EXPECT_EQ(outcomeOf(apply), "ok") << apply;
  EXPECT_EQ(g_config["advert.interval"], "180");

  const std::string read = run(fake::Job(123, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(read), "ok") << read;
  EXPECT_NE(read.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos) << read;
  // The silence is read off the **result** and not the log: since the library's task p1.10-49
  // every job's console lines carry a prefix and the read says so in a line of its own, and this
  // job's log - thirty keys over the local door - is full before the access list is asked for.
  // `mm::kAclSilentLine` is where the snapshot records it, which is what meshmaster.dk reads.
  EXPECT_NE(read.find("acl: no reply"), std::string::npos) << "the access list is silence: " << read;
  ASSERT_FALSE(g_setperms.empty());
  EXPECT_EQ(g_setperms.back(), "setperm " + self() + " 1") << "the restore, over the console";

  const std::string advert = run(fake::Job(124, "advert.request")
                                     .atClass("admin")
                                     .leasedUntil(kLease)
                                     .withAdminPassword("adm1n-pass"));
  EXPECT_EQ(outcomeOf(advert), "ok") << advert;
  EXPECT_EQ(g_adverts, 1);

  const std::string clock =
      run(fake::Job(125, "clock.sync").atClass("admin").leasedUntil(kLease).withAdminPassword("adm1n-pass"));
  EXPECT_EQ(outcomeOf(clock), "ok") << clock;
  EXPECT_GE(g_clockSet, kNow);

  const std::string password = run(fake::Job(126, "password.set")
                                       .atClass("admin")
                                       .leasedUntil(kLease)
                                       .withAdminPassword("adm1n-pass")
                                       .withNewPassword(secret)
                                       .withAclLevel(mm::kPermReadOnly)
                                       .withPayload("{\"field\":\"adminPassword\"}"));
  EXPECT_EQ(outcomeOf(password), "ok") << password;
  EXPECT_EQ(password.find(secret), std::string::npos);
  EXPECT_FALSE(fake::LogCapture::the().holds(secret));
  EXPECT_FALSE(holds(&link(), sizeof(agent::LinkMesh), secret));
  EXPECT_FALSE(holds(&world().client, sizeof(lc::Client), secret));
  EXPECT_FALSE(holds(rak().cliBuffer(), rak().cliBufferSize(), secret));

  const std::string acl = run(fake::Job(127, "acl.set")
                                  .atClass("admin")
                                  .leasedUntil(kLease)
                                  .withAdminPassword("adm1n-pass")
                                  .withPayload("{\"publicKey\":\"" + fake::strangerKey() + "\",\"level\":1}"));
  EXPECT_EQ(outcomeOf(acl), "ok") << acl;

  const std::string reboot =
      run(fake::Job(128, "reboot").atClass("admin").leasedUntil(kLease).withAdminPassword("adm1n-pass"));
  EXPECT_EQ(outcomeOf(reboot), "ok") << "the console never answers a reboot; the silence is the answer: " << reboot;
  EXPECT_EQ(rak().cli_lines.back(), "reboot");

  // A discover of one's own repeater: the UART is the direct path, and the synthetic login
  // teaches it; nothing floods.
  const std::string discover =
      run(fake::Job(129, "discover").atClass("admin").leasedUntil(kLease).withAdminPassword("adm1n-pass").withNoPath());
  EXPECT_EQ(outcomeOf(discover), "ok") << discover;
  EXPECT_NE(discover.find("\"hops\":[]"), std::string::npos) << discover;

  // -- not served: no request of §5 yields these bytes (the report's local-mode table)
  const std::string status =
      run(fake::Job(130, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(status), "failed") << status;
  EXPECT_NE(logOf(status).find("the packet could not be built"), std::string::npos) << status;
  const std::string neighbours =
      run(fake::Job(131, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(neighbours), "failed") << neighbours;
  const std::string trace = run(fake::Job(132, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(trace), "failed") << trace;

  // -- and through all thirteen: nothing on the mesh client, client_tx unmoved, the repeater on
  // the far side never asked.
  EXPECT_EQ(link().linkCounters().local_jobs, 13u);
  EXPECT_EQ(rak().client_tx(), 0u);
  EXPECT_EQ(world().sendsExecuted(), 0u);
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 0u);
  EXPECT_EQ(mesh.repeater().sendCount(), 0);
  EXPECT_EQ(link().linkCounters().local_unserved, 3u);
  EXPECT_GE(link().linkCounters().local_requests, 10u);
}

// Link 1.2 (task p9-06): the four of §5.9 come out of the radio's own handler, and the three that
// were *Failed* before it are answers now. The test above is the same thirteen types against a
// **1.1** radio and must keep passing unchanged: that is the first unit's own radio until the
// repeater firmware's pile 3.9 reaches it.
TEST_F(LinkMeshTest, With12TheDoorAnswersStatusNeighboursOwnerInfoAndTheAccessList) {
  makeIt12();
  const std::uint32_t silent_before = link().linkCounters().local_silent;

  // -- the status, as the node packs it. The reading is the fixtures' own figures.
  const std::string status =
      run(fake::Job(160, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(status), "ok") << status;
  EXPECT_NE(status.find("\"batteryMv\":4012"), std::string::npos) << status;
  EXPECT_NE(status.find("\"uptimeS\":86400"), std::string::npos) << status;
  EXPECT_NE(status.find("\"noiseFloorDbm\":-112"), std::string::npos) << status;
  // A status at class `acl` reads the firmware version too, which is the owner info (0x07).
  EXPECT_NE(status.find("\"firmware\":\"v1.18.0\""), std::string::npos) << status;

  // -- the neighbours, paged: eleven rows and nine to a page, so a second request at offset 9.
  door.page_offsets.clear();
  const std::string neighbours =
      run(fake::Job(161, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(neighbours), "ok") << neighbours;
  ASSERT_EQ(door.page_offsets.size(), 2u) << "eleven neighbours, nine to a page: two pages";
  EXPECT_EQ(door.page_offsets[0], 0);
  EXPECT_EQ(door.page_offsets[1], 9);
  EXPECT_NE(neighbours.find("\"prefix\":\"0111223344556677\""), std::string::npos) << neighbours;
  EXPECT_NE(neighbours.find("\"prefix\":\"0b11223344556677\""), std::string::npos)
      << "the eleventh row, which only the second page carries: " << neighbours;

  // -- the access list, which before 1.2 was the one request the door answered with silence.
  const std::string read = run(fake::Job(162, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"acl\"]}"));
  EXPECT_EQ(outcomeOf(read), "ok") << read;
  EXPECT_EQ(logOf(read).find("no ACL in this snapshot"), std::string::npos)
      << "the door answers the access list now: " << read;
  EXPECT_NE(read.find("a1b2c3d4e5f6"), std::string::npos)
      << "the fixtures' one admin entry: " << read;
  EXPECT_EQ(link().linkCounters().local_silent, silent_before)
      << "nothing was answered with silence";

  // -- and nothing of it went on the air, at any point.
  EXPECT_EQ(rak().client_tx(), 0u);
  EXPECT_EQ(world().sendsExecuted(), 0u);
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 0u);
  EXPECT_EQ(mesh.repeater().sendCount(), 0);
  EXPECT_EQ(link().linkCounters().local_unserved, 0u);
  EXPECT_GT(world().requestsOf(link::type::kLocalRequest), 0u);
}

// §5.9: "A request the handler gives no answer to is ERR_RANGE... Over the mesh it would have been
// silence." So the library must read it as silence and not as a failure - for the access list that
// is the difference between *no ACL in this snapshot, no evidence either way* and a read that
// reports an empty list as a fact.
TEST_F(LinkMeshTest, With12AnErrRangeReachesTheRunnerAsSilence) {
  makeIt12();
  door.answers_access_list = false;  // a handler that answers none of them
  const std::string read = run(fake::Job(163, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"acl\"]}"));
  EXPECT_EQ(outcomeOf(read), "ok") << read;
  EXPECT_NE(logOf(read).find("no ACL in this snapshot"), std::string::npos)
      << "an ERR_RANGE is the silence the library's own rule is written for: " << read;
  // `readAccessList` asks with `retry`, so the silence is heard twice - the figure
  // tasks/phase-9/FOLLOWUPS.md's [05] item holds.
  EXPECT_EQ(link().linkCounters().local_silent, 2u);
  EXPECT_EQ(rak().client_tx(), 0u);
  EXPECT_EQ(link().linkCounters().local_unserved, 0u) << "a request was made; it answered nothing";
}

TEST_F(LinkMeshTest, With12DiscoveryAndTraceAreStillFailedAndTelemetryIsStillLppGet) {
  makeIt12();
  rak().lpp = fake::afterPrefix("telemetry-response");
  // §5.9 answers 0x03 ERR_RANGE on purpose: telemetry's local form is §5.8's LPP_GET, and it
  // stays there.
  const std::string telemetry =
      run(fake::Job(164, "telemetry").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(telemetry), "ok") << telemetry;
  EXPECT_NE(telemetry.find("\"type\":\"voltage\""), std::string::npos) << telemetry;
  ASSERT_EQ(rak().lpp_masks.size(), 1u);
  EXPECT_EQ(rak().lpp_masks[0], agent::kLocalLppPermissions);
  for (const sim::SimRak::LocalAsked& one : rak().local_asked) {
    EXPECT_NE(one.req_type, 0x03) << "telemetry never went as a LOCAL_REQUEST";
  }

  // An exchange of a radio with itself over zero hops means nothing, at any version.
  const std::string trace = run(fake::Job(165, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(trace), "failed") << trace;
  EXPECT_EQ(rak().client_tx(), 0u);
  EXPECT_EQ(link().linkCounters().local_unserved, 1u);
}

TEST_F(LinkMeshTest, TheLocalDoorIsSilentWithoutASessionThatHasCli) {
  makeItLocal();
  rak().radio.caps = link::cap::kMeshClient | link::cap::kPower;  // a radio without CLI
  world().client.radioWasReset();
  ASSERT_TRUE(world().client.hello().ok());
  const std::string body = run(fake::Job(140, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));
  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_TRUE(rak().cli_lines.empty()) << "no line went: there was no login to run it on";
  EXPECT_EQ(rak().client_tx(), 0u);
}

TEST_F(LinkMeshTest, AJobForANeighbourAfterALocalOneIsAMeshJobAgain) {
  makeItLocal();
  const std::string local = run(fake::Job(150, "cli")
                                    .atClass("admin")
                                    .leasedUntil(kLease)
                                    .withAdminPassword("adm1n-pass")
                                    .withPayload("{\"command\":\"ver\"}"));
  EXPECT_EQ(outcomeOf(local), "ok") << local;
  EXPECT_EQ(rak().client_tx(), 0u);

  // The same type for a stranger: the mesh client, the air, client_tx moving.
  mesh.setKey(fake::strangerKey());
  const std::string stranger = run(fake::Job(151, "cli")
                                       .atClass("admin")
                                       .leasedUntil(kLease)
                                       .withAdminPassword("adm1n-pass")
                                       .withPayload("{\"command\":\"ver\"}")
                                       .forRepeater(18, "Stranger", fake::strangerKey()));
  EXPECT_EQ(outcomeOf(stranger), "ok") << stranger;
  EXPECT_EQ(rak().client_tx(), 2u);
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 1u);
}

// The interface's own question (task p9-05): `IMesh::answersLocally()` is true from a `beginJob`
// for the unit's own repeater until the `endJob` that closes it, and false otherwise. The
// library reads it at every `spend` and at every login, so the whole of the contract is *when*
// it is true - a door that stayed open would stop pacing a stranger's job, and one that opened
// early would send no password to a repeater that wants one.
TEST_F(LinkMeshTest, TheDoorAnswersForTheOwnRepeaterOnlyAndOnlyInsideItsJob) {
  EXPECT_FALSE(link().answersLocally()) << "no job, no door - and no own key yet either";
  makeItLocal();
  EXPECT_FALSE(link().answersLocally()) << "the key alone is not a job";

  ASSERT_TRUE(link().beginJob(repeater().c_str(), nullptr, 0, 0));
  EXPECT_TRUE(link().answersLocally());
  EXPECT_TRUE(link().local()) << "the console's name for the same question";
  link().endJob();
  EXPECT_FALSE(link().answersLocally()) << "between jobs it is false";

  // A stranger's job, begun straight after a local one: the door is shut for the whole of it.
  ASSERT_TRUE(link().beginJob(fake::strangerKey().c_str(), nullptr, 0, 0));
  EXPECT_FALSE(link().answersLocally());
  link().endJob();
  EXPECT_FALSE(link().answersLocally());

  // And a key this end cannot parse begins no job, so it cannot open the door either.
  EXPECT_FALSE(link().beginJob("not-a-key", nullptr, 0, 0));
  EXPECT_FALSE(link().answersLocally());

  // The key going away shuts it under a job in progress, which is what a radio that was swapped
  // looks like (`clearOwnKey`, task p8-04): the next `beginJob` is a mesh job.
  ASSERT_TRUE(link().beginJob(repeater().c_str(), nullptr, 0, 0));
  EXPECT_TRUE(link().answersLocally());
  link().endJob();
  link().clearOwnKey();
  ASSERT_TRUE(link().beginJob(repeater().c_str(), nullptr, 0, 0));
  EXPECT_FALSE(link().answersLocally()) << "this end no longer knows which repeater is its own";
  link().endJob();
}

// The same door asked for a **key**, before any job has begun (task p10-01; the exit-node
// firmware's p1.10-43). The interface's contract is that the two answers agree - true for a key
// exactly when a job begun for that key would make `answersLocally()` true - so this walks the
// pair together and lets them disagree if they can.
TEST_F(LinkMeshTest, TheKeyedDoorAnswersForTheOwnRepeaterAndAgreesWithTheJobsOwnAnswer) {
  EXPECT_FALSE(link().answersLocallyFor(repeater().c_str())) << "no own key yet: the door is shut";
  makeItLocal();

  // True for this radio's key between jobs, which is exactly where `answersLocally()` is false:
  // that is the difference the second virtual exists for.
  EXPECT_TRUE(link().answersLocallyFor(repeater().c_str()));
  EXPECT_FALSE(link().answersLocally());
  // False for a stranger's.
  EXPECT_FALSE(link().answersLocallyFor(fake::strangerKey().c_str()));
  // **The same key shouted is the same key**, because `beginJob` reads it the same way - both go
  // through this end's one `parseKey`, whose hex digits are case-insensitive - and agreeing with
  // `beginJob` is the contract, not matching the claim's spelling. A claim carries lowercase, so
  // nothing the server sends takes this path; what matters is that the two cannot disagree.
  std::string shouted = repeater();
  for (char& c : shouted) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_TRUE(link().answersLocallyFor(shouted.c_str()));
  ASSERT_TRUE(link().beginJob(shouted.c_str(), nullptr, 0, 0));
  EXPECT_TRUE(link().answersLocally()) << "and the job for it really is a local one";
  link().endJob();
  // And a key it cannot parse is false and not an error, as the interface asks: the `beginJob` a
  // moment later refuses it anyway.
  EXPECT_FALSE(link().answersLocallyFor("not-a-key"));
  EXPECT_FALSE(link().answersLocallyFor(""));
  EXPECT_FALSE(link().answersLocallyFor(nullptr));
  EXPECT_FALSE(link().answersLocallyFor((repeater() + "00").c_str())) << "too long is not this key";

  // The pair, walked together over both keys: whatever the keyed door says of a key, a job begun
  // for it says the same.
  for (const std::string& key : {repeater(), fake::strangerKey()}) {
    const bool keyed = link().answersLocallyFor(key.c_str());
    ASSERT_TRUE(link().beginJob(key.c_str(), nullptr, 0, 0)) << key;
    EXPECT_EQ(link().answersLocally(), keyed) << key;
    EXPECT_EQ(link().local(), keyed) << key;
    link().endJob();
    EXPECT_EQ(link().answersLocallyFor(key.c_str()), keyed) << key;
  }

  // The radio swapped: the key goes away and the door shuts for the key it used to answer for.
  link().clearOwnKey();
  EXPECT_FALSE(link().answersLocallyFor(repeater().c_str()));
}

// ---------------------------------------------------------------------------------------------
// Where the last ten jobs went (task p9-04)

TEST_F(LinkMeshTest, TheLastTenJobsRememberTheKeyTheyNamedAndWhetherTheDoorAnsweredThem) {
  // Nothing of the library's own `last ten` carries the key a job was addressed to, and
  // components/agent-core is a hash-checked copy of upstream, so the end that decides where a
  // job goes is the end that remembers it.
  EXPECT_EQ(link().jobsBegun(), 0u);
  EXPECT_EQ(link().jobBegunAt(0), nullptr);
  // The fixture arms this end with a key (the shim's setSelfKey); a wake that has not read the
  // identity out of the radio yet has none, which is where this starts.
  link().clearOwnKey();
  EXPECT_EQ(link().ownKeyShown(), nullptr);

  // One for a stranger while this end has no key of its own: a mesh job, and the entry says the
  // door was shut at the time.
  EXPECT_TRUE(link().beginJob(fake::strangerKey().c_str(), nullptr, 0, 0));
  link().endJob();
  ASSERT_EQ(link().jobsBegun(), 1u);
  const agent::JobBegun* first = link().jobBegunAt(0);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(first->local);
  EXPECT_FALSE(first->own_key_known);

  // The key arrives, and a job naming it goes through the door.
  makeItLocal();
  ASSERT_NE(link().ownKeyShown(), nullptr);
  EXPECT_TRUE(link().beginJob(repeater().c_str(), nullptr, 0, 0));
  link().endJob();
  ASSERT_EQ(link().jobsBegun(), 2u);
  const agent::JobBegun* local = link().jobBegunAt(0);
  ASSERT_NE(local, nullptr);
  EXPECT_TRUE(local->local);
  EXPECT_TRUE(local->own_key_known);
  // Newest first: the stranger's is behind it now.
  EXPECT_FALSE(link().jobBegunAt(1)->local);
  // And the six bytes kept are the key's own first six.
  uint8_t key[32];
  ASSERT_TRUE(keyBytes(repeater(), key));
  EXPECT_EQ(std::memcmp(local->key, key, agent::kKeyShownBytes), 0);
  EXPECT_EQ(std::memcmp(link().ownKeyShown(), key, agent::kKeyShownBytes), 0);

  // A key that is not 64 hex characters is no job at all, and nothing is remembered for it.
  EXPECT_FALSE(link().beginJob("not-a-key", nullptr, 0, 0));
  EXPECT_EQ(link().jobsBegun(), 2u);

  // The list is ten deep and the oldest falls off the end.
  for (int i = 0; i < 12; i++) {
    EXPECT_TRUE(link().beginJob(fake::strangerKey().c_str(), nullptr, 0, 0));
    link().endJob();
  }
  EXPECT_EQ(link().jobsBegun(), agent::kJobsRemembered);
  for (size_t i = 0; i < agent::kJobsRemembered; i++) {
    EXPECT_FALSE(link().jobBegunAt(i)->local) << i;  // the local one is twelve jobs ago
  }
  EXPECT_EQ(link().jobBegunAt(agent::kJobsRemembered), nullptr);
}

// ---------------------------------------------------------------------------------------------
// §6.5, the flood scope (task p14-01)
//
// Every one of these is written twice over: once against a scripted radio of 1.2, where this end
// must say nothing about scope and send nothing, and once against one of 1.3, where it reads the
// name, scopes by default and drops the scope for exactly one send. The frames are what is
// asserted, in order, because the order *is* the rule: `FLOOD_SCOPE` with `mode` 2 goes
// immediately before the one send that is to go without the scope, and before no other.

// A flooded login with two tries, by hand: the library's own rule is *the scope first, then one
// try without it* (`agent/FloodScope.h`), and it tells the mesh which is which per send. Driving
// the two sends here rather than through a job is what makes the frame order readable.
TEST_F(LinkMeshTest, With12NothingIsEverSaidAboutScopeAndTheFramesAreTodaysButForHellosMinor) {
  makeIt12Link();
  EXPECT_EQ(link().floodScope(), nullptr) << "no capability: the mesh says nothing about scope";
  EXPECT_FALSE(link().floodScopeKnown());
  EXPECT_FALSE(link().askFloodScope()) << "and nothing is asked";
  EXPECT_EQ(world().requestsOf(link::type::kFloodScope), 0u);

  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  ASSERT_FALSE(mesh.hasPath()) << "no path: the sends flood";
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.endJob();

  // Two tries, two SEND_LOGIN frames, and nothing between them.
  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].type, link::type::kSendLogin);
  EXPECT_EQ(frames[1].type, link::type::kSendLogin);
  EXPECT_EQ(world().requestsOf(link::type::kFloodScope), 0u) << "no FLOOD_SCOPE is ever sent";
  EXPECT_EQ(rak().radio.flood_scope_asks, 0u);
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 0u);
  // §6.2: an older session does not carry `scoped` at all, which is not the same as a 0.
  EXPECT_FALSE(link().lastRecord().scoped_said);
  EXPECT_FALSE(link().lastRecord().scoped);
  ASSERT_EQ(rak().sent.size(), 2u);
  EXPECT_FALSE(rak().sent[0].scoped_said);
  EXPECT_FALSE(rak().sent[1].scoped_said);

  // And the HELLO: byte for byte what a 1.2 Walter sent, but for its `proto_minor`. Built here
  // from this firmware's own encoder at minor 2, so the comparison is of the whole payload and
  // not of a transcription of it.
  const sim::ScriptedRadio::Seen* hello = nullptr;
  for (const sim::ScriptedRadio::Seen& one : rak().radio.requests) {
    if (one.type == link::type::kHello) hello = &one;
  }
  ASSERT_NE(hello, nullptr);
  link::HelloRequest at12;
  at12.proto_minor = 2;
  at12.fw_version = link::text(core::version(), std::strlen(core::version()));
  uint8_t buffer[link::kMaxPayload] = {};
  const link::Encoded encoded = link::encode(at12, buffer, sizeof buffer);
  ASSERT_TRUE(encoded.ok);
  ASSERT_EQ(hello->payload.size(), encoded.size);
  EXPECT_EQ(hello->payload[1], link::kProtoMinor) << "this end asks for 3 (§5.1)";
  for (size_t i = 0; i < encoded.size; i++) {
    if (i == 1) continue;  // the one byte that moved
    EXPECT_EQ(hello->payload[i], buffer[i]) << "byte " << i << " of the HELLO";
  }
}

TEST_F(LinkMeshTest, With13TheNameIsReadOnceAndTheOneTryWithoutItIsFloodScope2ThenTheSend) {
  makeIt13("dk");
  // §6.5 read where the unit's own key is read, which is before the heartbeat is built: one
  // request, and the name is the radio's own.
  EXPECT_EQ(link().floodScope(), nullptr) << "nobody has asked yet";
  ASSERT_TRUE(link().askFloodScope());
  ASSERT_NE(link().floodScope(), nullptr);
  EXPECT_STREQ(link().floodScope(), "dk");
  EXPECT_TRUE(link().floodScopeKnown());
  EXPECT_EQ(rak().radio.flood_scope_asks, 1u);
  // Once a session: a second ask costs no frame.
  EXPECT_TRUE(link().askFloodScope());
  EXPECT_EQ(rak().radio.flood_scope_asks, 1u);

  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  // The first try: scoped, which is what the radio does unasked. One frame.
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  EXPECT_TRUE(link().lastRecord().scoped_said);
  EXPECT_TRUE(link().lastRecord().scoped) << "§6.2: it went out carrying the scope";
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  EXPECT_TRUE(link().lastRecord().scoped) << "and the reply record keeps it";
  // The second: FLOOD_SCOPE 2, then the send.
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  EXPECT_TRUE(link().lastRecord().scoped_said);
  EXPECT_FALSE(link().lastRecord().scoped) << "this one carried no transport code";
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  // The third: one SEND_* again - §6.5 puts the radio back at mode 1 after the send, and the
  // library asks for the scope again.
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_EQ(mesh.sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag), mm::SendResult::Flood);
  ASSERT_TRUE(mesh.awaitReply(tag, 16000, nullptr));
  mesh.endJob();

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 4u) << "one, then two, then one";
  EXPECT_EQ(frames[0].type, link::type::kSendLogin);
  EXPECT_EQ(frames[1].type, link::type::kFloodScope);
  EXPECT_EQ(frames[1].mode, link::flood_mode::kUnscopedNext);
  EXPECT_EQ(frames[2].type, link::type::kSendLogin);
  EXPECT_EQ(frames[3].type, link::type::kSendRequest);
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 1u) << "one mode 2, for one send";
  // And nothing of §6.5 moved client_tx: the request puts nothing on the air.
  ASSERT_EQ(rak().sent.size(), 3u);
  EXPECT_TRUE(rak().sent[0].scoped);
  EXPECT_FALSE(rak().sent[1].scoped);
  EXPECT_TRUE(rak().sent[2].scoped);
  EXPECT_EQ(rak().client_tx(), 3u);
  EXPECT_EQ(link().linkCounters().scope_asks, 1u);
  EXPECT_EQ(link().linkCounters().scope_unscoped, 1u);
  EXPECT_EQ(link().linkCounters().scope_surprises, 0u);
  EXPECT_EQ(link().linkCounters().scope_failed, 0u);
}

TEST_F(LinkMeshTest, With13AJobThatHoldsAPathSendsNoFloodScopeAtAll) {
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  const size_t mark = framesSoFar();
  const uint8_t hashes[] = {0xa1, 0x7f};
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), hashes, 2, 1));
  ASSERT_TRUE(mesh.hasPath());
  uint32_t tag = 0;
  // The library asks for the try without the scope all the same; the send goes direct, and a
  // path has no scope (§6.5), so there is nothing to drop and no request to make.
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Direct);
  EXPECT_TRUE(link().lastRecord().scoped_said);
  EXPECT_FALSE(link().lastRecord().scoped) << "§6.2: 0 for a direct send";
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag), mm::SendResult::Direct);
  ASSERT_TRUE(mesh.awaitReply(tag, 16000, nullptr));
  mesh.endJob();

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].type, link::type::kSendLogin);
  EXPECT_EQ(frames[1].type, link::type::kSendRequest);
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 0u);
  EXPECT_EQ(link().linkCounters().scope_unscoped, 0u);
}

TEST_F(LinkMeshTest, With13TheOwnRepeatersJobSendsNoFloodScopeAndNothingGoesOnTheAir) {
  makeIt13("dk");
  makeItLocal();
  ASSERT_TRUE(link().askFloodScope());
  const uint32_t asks = rak().radio.flood_scope_asks;
  const size_t mark = framesSoFar();

  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  ASSERT_TRUE(link().answersLocally()) << "the local door";
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Direct);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  uint32_t stamp = 0;
  ASSERT_EQ(mesh.sendCli("get tx", false, stamp), mm::SendResult::Direct);
  ASSERT_TRUE(mesh.awaitText(16000, nullptr));
  mesh.endJob();

  EXPECT_TRUE(scopeAndSendFrames(mark).empty()) << "nothing of §6 and nothing of §6.5";
  EXPECT_EQ(rak().radio.flood_scope_asks, asks) << "the door needs no scope";
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 0u);
  EXPECT_EQ(rak().client_tx(), 0u) << "the local door puts nothing on the air";
  EXPECT_EQ(link().linkCounters().scope_unscoped, 0u);
}

TEST_F(LinkMeshTest, With13AndNoDefaultScopeTheMeshSaysStarAndSendsNoFloodScope2) {
  makeIt13("");  // a radio that holds no `region default`
  ASSERT_TRUE(link().askFloodScope());
  ASSERT_NE(link().floodScope(), nullptr);
  EXPECT_STREQ(link().floodScope(), mm::kFloodScopeUnscoped) << "the library's `*`";
  EXPECT_TRUE(mm::floodScopeIsUnscoped(link().floodScope()));
  EXPECT_EQ(rak().radio.flood_scope_asks, 1u);

  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  // The library's own rule answers `Unscoped` for a `*` mesh at every flood - there is nothing
  // to drop - and this end sends nothing for it.
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  EXPECT_TRUE(link().lastRecord().scoped_said);
  EXPECT_FALSE(link().lastRecord().scoped) << "a radio with no default scope scopes nothing";
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.endJob();

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, link::type::kSendLogin);
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 0u) << "no mode 2: there is nothing to drop";
  EXPECT_EQ(link().linkCounters().scope_surprises, 0u) << "0 was what this end expected";
}

TEST_F(LinkMeshTest, With13AFloodScope2ThatIsNotAnsweredMakesTheSendFailAndSendsNothing) {
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));

  // §3: three silences and the link is down. The request that had to go first did not answer,
  // so the send is not made at all.
  rak().radio.ignoreType(link::type::kFloodScope, link::kRequestAttempts);
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  EXPECT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  EXPECT_EQ(link().lastFault(), lc::Fault::kLinkDown);
  EXPECT_EQ(link().linkCounters().scope_failed, 1u);
  EXPECT_FALSE(link().awaiting()) << "nothing is awaited for a send that was never made";
  EXPECT_FALSE(rak().awaiting());

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 3u) << "three attempts at the one request (§3)";
  for (const Frame& one : frames) {
    EXPECT_EQ(one.type, link::type::kFloodScope);
    EXPECT_EQ(one.mode, link::flood_mode::kUnscopedNext);
  }
  EXPECT_EQ(rak().client_tx(), 0u) << "nothing was transmitted";
  EXPECT_EQ(mesh.repeater().sendCount(), 0);
  mesh.endJob();
}

TEST_F(LinkMeshTest, With13AFloodScope2ThatIsRefusedMakesTheSendFailAndSendsNothing) {
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));

  rak().radio.refuseType(link::type::kFloodScope, link::status::kFailed, 1);
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Unscoped);
  EXPECT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  EXPECT_EQ(link().lastFault(), lc::Fault::kRefused);
  EXPECT_EQ(link().lastStatus(), link::status::kFailed);
  EXPECT_EQ(link().linkCounters().scope_failed, 1u);

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, link::type::kFloodScope);
  EXPECT_EQ(rak().client_tx(), 0u);
  mesh.endJob();
}

TEST_F(LinkMeshTest, With13AFloodAnsweredScopedZeroMakesTheNameBeAskedAgain) {
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  EXPECT_STREQ(link().floodScope(), "dk");
  ASSERT_EQ(rak().radio.flood_scope_asks, 1u);

  // The radio's `region default` is cleared under this end's feet - a `region default` over the
  // mesh, or a CLI line on this link (§6.5). The next flood this end asks to be scoped comes
  // back `scoped` 0.
  rak().radio.flood_scope.clear();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  EXPECT_TRUE(link().lastRecord().scoped_said);
  EXPECT_FALSE(link().lastRecord().scoped) << "it went out with no transport code";
  EXPECT_EQ(link().linkCounters().scope_surprises, 1u);
  // The name was asked again, and `floodScope()` says what the radio says now.
  EXPECT_EQ(rak().radio.flood_scope_asks, 2u);
  ASSERT_NE(link().floodScope(), nullptr);
  EXPECT_STREQ(link().floodScope(), mm::kFloodScopeUnscoped);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.endJob();

  // And with the scope back, the next flood is scoped again and nothing is surprising.
  rak().radio.flood_scope = "dk";
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_EQ(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Flood);
  EXPECT_TRUE(link().lastRecord().scoped);
  EXPECT_EQ(link().linkCounters().scope_surprises, 2u) << "`*` was asked for and `dk` came back";
  EXPECT_STREQ(link().floodScope(), "dk");
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));
  mesh.endJob();
}

TEST_F(LinkMeshTest, With13ARestartUnderAJobTellsTheLibraryNoScopeUntilTheNextHello) {
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  EXPECT_STREQ(link().floodScope(), "dk");

  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  mesh.setFloodMode(mm::FloodMode::Scoped);
  ASSERT_NE(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  ASSERT_TRUE(mesh.awaitReply(0, 16000, nullptr));

  rak().restart(true);  // a new boot_id and an EV_BOOT (§5.1)
  mesh.loop();
  ASSERT_EQ(link().linkCounters().restarts, 1u);
  // §6.5: the radio is back at 1.0 until the next HELLO and floods unscoped until then. So this
  // end says **nothing** - not `dk`, which the radio is not using, and not `*` either.
  EXPECT_EQ(link().floodScope(), nullptr);
  EXPECT_FALSE(link().floodScopeKnown());
  // And the radio proves it: §6.5's request is ERR_STATE in a session older than 1.3, which is
  // what the next job's ask gets - so the job fails as one whose JOB_BEGIN failed, at no send.
  const size_t mark = framesSoFar();
  const uint32_t client_tx = rak().client_tx();
  EXPECT_TRUE(link().beginJob(repeater().c_str(), nullptr, 0, 0)) << "the key is addressable";
  EXPECT_FALSE(link().jobBegun());
  EXPECT_EQ(link().lastStatus(), link::status::kState);
  EXPECT_EQ(world().requestsOf(link::type::kJobBegin), 1u) << "no JOB_BEGIN for a job that failed here";
  EXPECT_EQ(rak().client_tx(), client_tx) << "nothing on the air";
  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, link::type::kFloodScope);
  EXPECT_EQ(frames[0].mode, link::flood_mode::kAsk);
  link().endJob();

  // The next HELLO settles on 1.3 again, and the name comes back.
  ASSERT_TRUE(world().client.hello().ok());
  ASSERT_EQ(world().client.session().proto_minor, link::kFloodScopeMinor);
  ASSERT_TRUE(link().askFloodScope());
  EXPECT_STREQ(link().floodScope(), "dk");
}

TEST_F(LinkMeshTest, With13AnUnansweredAskSaysNullToTheHeartbeatAndIsAskedAgainBeforeAJob) {
  makeIt13("dk");
  // The one ask of the session goes unanswered: the heartbeat is told nothing about scope, which
  // is what leaves its `flood` object out altogether.
  rak().radio.ignoreType(link::type::kFloodScope, link::kRequestAttempts);
  EXPECT_FALSE(link().askFloodScope());
  EXPECT_EQ(link().floodScope(), nullptr);
  EXPECT_FALSE(link().floodScopeKnown());
  EXPECT_EQ(link().linkCounters().scope_asks, 1u);

  // And asked again before a mesh job begins - this time answered, so the job is begun.
  const size_t mark = framesSoFar();
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  EXPECT_TRUE(link().jobBegun());
  EXPECT_STREQ(link().floodScope(), "dk");
  EXPECT_EQ(link().linkCounters().scope_asks, 2u);
  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, link::type::kFloodScope);
  EXPECT_EQ(frames[0].mode, link::flood_mode::kAsk);
  mesh.endJob();
}

// The road as a job, not by hand: the library's own rule applied to a real `status` job through
// the framing, against each radio in turn. A `status` logs in and asks, both flooded, and the
// library's rule makes the second flood of the transaction the unscoped one. One job a test,
// because a session the server opened is reused across jobs (EXIT-NODES §5) and the second job of
// a rig does not log in again.
TEST_F(LinkMeshTest, AStatusJobOn12IsNeverAskedAboutScope) {
  mesh.grant(self(), mm::kPermReadOnly);
  makeIt12Link();
  const size_t mark = framesSoFar();
  const std::string body = run(fake::Job(300, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withNoPath()  // so both sends flood: the strongest case
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_EQ(frames.size(), 2u) << "a login and a request, and nothing between them";
  EXPECT_EQ(frames[0].type, link::type::kSendLogin);
  EXPECT_EQ(frames[1].type, link::type::kSendRequest);
  EXPECT_EQ(world().requestsOf(link::type::kFloodScope), 0u);
  EXPECT_EQ(link().floodScope(), nullptr);
  ASSERT_EQ(rak().sent.size(), 2u);
  EXPECT_FALSE(rak().sent[0].scoped_said) << "§6.2: an older session carries no `scoped` byte";
}

TEST_F(LinkMeshTest, AStatusJobOn13FloodsScopedThenOnceWithoutIt) {
  // The library's rule is per **transaction** - one retry loop of the runner's - and the first
  // flood of a transaction is the scoped one (`agent/FloodScope.h`). So the send that goes
  // without the scope is a *second try*: here the repeater is silent to the first login, the
  // runner tries again, and that second flood is the one §6.5's `mode` 2 goes before.
  mesh.grant(self(), mm::kPermReadOnly);
  makeIt13("dk");
  ASSERT_TRUE(link().askFloodScope());
  mesh.beSilent(1);  // the first flooded login gets no reply
  const size_t mark = framesSoFar();
  const std::string body = run(fake::Job(301, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withNoPath()  // a path has no scope (§6.5)
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok") << body;

  const std::vector<Frame> frames = scopeAndSendFrames(mark);
  ASSERT_GE(frames.size(), 3u) << "two login tries and the request";
  EXPECT_EQ(frames[0].type, link::type::kSendLogin) << "the first try: scoped, unasked";
  EXPECT_EQ(frames[1].type, link::type::kFloodScope) << "then the one request that drops it";
  EXPECT_EQ(frames[1].mode, link::flood_mode::kUnscopedNext);
  EXPECT_EQ(frames[2].type, link::type::kSendLogin) << "and the try that goes without the scope";
  // One mode 2, for one send: the request after it floods in the scope again.
  EXPECT_EQ(rak().radio.flood_scope_unscoped, 1u);
  EXPECT_EQ(link().linkCounters().scope_unscoped, 1u);
  EXPECT_EQ(link().linkCounters().scope_surprises, 0u);
  for (size_t i = 3; i < frames.size(); i++) {
    EXPECT_NE(frames[i].type, link::type::kFloodScope) << "frame " << i;
  }
  // And what went out: the first login carrying the scope, the second without it.
  ASSERT_GE(rak().sent.size(), 3u);
  EXPECT_TRUE(rak().sent[0].scoped);
  EXPECT_FALSE(rak().sent[1].scoped);
  EXPECT_TRUE(rak().sent[2].scoped) << "the request after the one send is scoped again";
  // §6.5 puts nothing on the air: client_tx is the runner's transactions and no more.
  EXPECT_EQ(rak().client_tx(), static_cast<uint32_t>(transactionsOf(body)));
}

// ---------------------------------------------------------------------------------------------
// The way back (the library's `v0.7.6`, upstream's task p1.21-71)

// **A mesh over the link tells no way back**, and that is the Walter's half of the eleven cases of
// upstream's suite this road does not run (host_test/CMakeLists.txt, LINK_SHIM_SKIPPED).
//
// A repeater answers a direct request along the way home it holds for that client, and it holds one
// only from a PATH packet the client sent it. §6 of the link protocol has no request that sends
// one: the Walter's radio is asked for a login, a request, a console line, a discovery and a trace,
// and for nothing else. So `LinkMesh` takes the library's **defaulted** member - it answers *not
// told*, nothing goes on the air for it, and every reply to this unit is flooded by the repeater
// exactly as it was before the bump. The library says that in one sentence in each job's log, and
// a reply's own route reads *not known*, because §6.3's reply record says only whether a response
// came inside a returned path - which is a path return and not a flood.
TEST_F(LinkMeshTest, AMeshOverTheLinkTellsNoWayBackAndSaysSoInEveryJobsLog) {
  mesh.grant(self(), mm::kPermReadOnly);
  const size_t mark = framesSoFar();
  const std::string body = run(fake::Job(302, "status")
                                   .atClass("acl")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .leasedUntil(kLease)
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok") << body;

  // The member itself: the library's default, which tells nothing and says so.
  const mm::IMesh::WayBack told = link().tellTheWayBack();
  EXPECT_EQ(told.how, mm::IMesh::WayBack::How::NotTold);
  EXPECT_EQ(told.hops, 0);
  EXPECT_EQ(told.clearMs, 0u);
  // Nothing went on the air for it: the repeater was told none, and the frames of this job are the
  // sends of §6.2 alone.
  EXPECT_EQ(mesh.repeater().waysBackCount(), 0);
  for (const std::string& frame : mesh.repeater().frames()) {
    EXPECT_NE(frame, "wayback") << "a way back reached the repeater without crossing the link";
  }
  EXPECT_GT(rak().radio.requests.size(), mark) << "the job's own sends did go out";
  // The job's log says it once, and every reply's route reads *not known* rather than *flooded*.
  const std::string log = logOf(body);
  EXPECT_NE(log.find("this mesh tells no way back"), std::string::npos) << log;
  EXPECT_NE(log.find("reply route unknown"), std::string::npos) << log;
  EXPECT_EQ(log.find("reply flooded"), std::string::npos) << log;
  EXPECT_EQ(link().reply().route, mm::ReplyRoute::NotKnown);
  // And the rail of this road is untouched: the sends the repeater was asked for are client_tx.
  EXPECT_EQ(rak().client_tx(), static_cast<uint32_t>(transactionsOf(body)));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
