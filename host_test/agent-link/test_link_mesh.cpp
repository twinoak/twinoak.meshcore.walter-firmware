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

#include "core/link/codec/wire.h"
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
  EXPECT_NE(logOf(read).find("no ACL in this snapshot"), std::string::npos) << "the access list is silence: " << read;
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

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
