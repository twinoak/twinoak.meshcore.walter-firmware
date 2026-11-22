// The job runner, end to end, on a host (task p1.5-17).
//
// `JobRunner` is driven here exactly as the board drives it - `claimAndRun()` against the three
// seams - with a scripted repeater behind the mesh (support/FakeMesh.h), a scripted server behind
// the uplink (support/FakeUplink.h) and a clock that only moves when the library asks it to
// (support/FakePlatform.h). Nothing sleeps: the airtime budget's twenty-second gap is twenty
// thousand turns of arithmetic, so eleven minutes of a config read is milliseconds here.
//
// What each test is for is EXIT-NODES §5's *Behaviour that matters*, §6's outcomes and §7's
// table: one transaction at a time, the gap and the per-minute cap, a bounded retry inside a
// transaction, sessions reused, credentials that never outlive their job, and the thirteen types
// with the transaction count §7 prices each of them at. The result of every job is held against
// the **shape** of its contract fixture - the members ContractsJson writes, in its order;
// test/test_job_results holds the same writers to those fixtures byte for byte for one sample
// each, and this says a *run* of the runner produces that shape.
//
// The suite's first test is the one that keeps the fake honest: every answer `FakeMesh` gives
// cites a companion fixture or a function of the pinned firmware, and the manifest at the top of
// that file is walked against its own body.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <agent/ConfigRead.h>
#include <agent/Iso8601.h>
#include <agent/JobEnvelopes.h>
#include <agent/JobResults.h>
#include <agent/JobRunner.h>
#include <agent/LoginSequence.h>

#include <support/FakeMesh.h>
#include <support/FakePlatform.h>
#include <support/FakeUplink.h>
#include <support/Fixtures.h>
#include <support/Jobs.h>

#ifndef PROJECT_DIR
#error "PROJECT_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

namespace fs = std::filesystem;

// A Thursday in September 2026, as every fixture's timestamps are.
constexpr uint32_t kNow = 1790000000;
constexpr uint32_t kLease = kNow + 1800;

const std::string& repeater() { return fake::repeaterKey(); }
const std::string& self() { return fake::selfKey(); }
const std::string& stranger() { return fake::strangerKey(); }

// One member of a JSON object, by name, with its braces: how a test reads the `data` out of a
// result envelope without a parser. Good enough for the envelopes this firmware writes, in which
// no nested object repeats a member name of the envelope itself.
std::string memberOf(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  size_t at = found + needle.size();
  while (at < json.size() && json[at] == ' ') at++;
  if (at >= json.size()) return "";
  if (json[at] == '{' || json[at] == '[') {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = at; i < json.size(); i++) {
      const char c = json[i];
      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        }
        continue;
      }
      if (c == '"') {
        inString = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
        if (depth == 0) return json.substr(at, i - at + 1);
      }
    }
    return "";
  }
  if (json[at] == '"') {
    // The value, unescaped. ContractsJson escapes more than JSON insists on - an apostrophe is
    // `'` and a `<` is `<`, as System.Text.Json's encoder writes them - so a test that
    // read the raw bytes could not look for a sentence with an apostrophe in it.
    std::string text;
    for (size_t i = at + 1; i < json.size(); i++) {
      if (json[i] == '"') return text;
      if (json[i] != '\\') {
        text.push_back(json[i]);
        continue;
      }
      if (++i >= json.size()) break;
      switch (json[i]) {
        case 'n': text.push_back('\n'); break;
        case 'r': text.push_back('\r'); break;
        case 't': text.push_back('\t'); break;
        case 'b': text.push_back('\b'); break;
        case 'f': text.push_back('\f'); break;
        case 'u': {
          if (i + 4 >= json.size()) return text;
          const int code = std::stoi(json.substr(i + 1, 4), nullptr, 16);
          i += 4;
          if (code < 0x80) text.push_back(static_cast<char>(code));
          break;
        }
        default: text.push_back(json[i]); break;
      }
    }
    return "";
  }
  size_t end = at;
  while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
  return json.substr(at, end - at);
}

// A repeater with `count` neighbours: different keys, newest first, which is the order the
// firmware answers in and all these tests read of them.
std::vector<fake::NeighbourEntry> neighbourList(int count) {
  std::vector<fake::NeighbourEntry> list;
  for (int i = 0; i < count; i++) {
    char prefix[17];
    std::snprintf(prefix, sizeof prefix, "%016d", i);
    list.push_back(fake::NeighbourEntry{std::string(prefix) + std::string(48, '1'),
                                        60u * static_cast<uint32_t>(i + 1), 24});
  }
  return list;
}

// Two invented job types compiled in beside the thirteen: what a consumer of the library does
// with `IJobExtension` (agent/IJobExtension.h).
class FakeExtension : public mm::IJobExtension {
 public:
  int typeCount() const override { return 2; }
  const char* type(int i) const override { return i == 0 ? "walter.sleep" : "walter.firmware"; }

  bool run(const mm::ClaimedJob& job, mm::JobContext& ctx, mm::ResultWriter& out) override {
    ran.emplace_back(job.type);
    if (std::strcmp(job.type, "walter.sleep") != 0 && std::strcmp(job.type, "walter.firmware") != 0) return false;
    ctx.log("the extension ran this type");
    waited = ctx.replyWaitMs();
    if (!ctx.spend()) {
      out.outcome(mm::Outcome::Timeout);
      return true;
    }
    uint32_t tag = 0;
    const mm::SendResult sent = ctx.mesh().sendRequest(mm::kReqTypeGetStatus, nullptr, 0, tag);
    answered = sent != mm::SendResult::Failed && ctx.mesh().awaitReply(tag, ctx.replyWaitMs(), nullptr);
    const std::string data = std::string("{\"slept\":true,\"heardTheRepeater\":") + (answered ? "true" : "false") + "}";
    out.data(data.c_str(), data.size());
    out.outcome(mm::Outcome::Ok);
    return true;
  }

  std::vector<std::string> ran;
  uint32_t waited = 0;
  bool answered = false;
};

// ---------------------------------------------------------------------------------------------
// The harness

class RunnerTest : public ::testing::Test {
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
    // A budget wide enough that most tests measure behaviour and not the gate; the gate's own
    // test sets the conservative default of §8 instead.
    setBudget(60, 1);
  }

  void setBudget(int perMinute, int gapSeconds) {
    mm::AgentBudget budget;
    budget.maxTransactionsPerMinute = perMinute;
    budget.minGapSeconds = gapSeconds;
    runner.setBudget(budget);
  }

  // One claim with one job, run to the end. Returns the result body the server was handed.
  std::string run(const fake::Job& job) {
    server.queue(job.json());
    const int ran = runner.claimAndRun();
    EXPECT_EQ(ran, 1) << "the claim did not hand over the job";
    EXPECT_FALSE(server.results().empty()) << "no result was posted";
    return server.results().empty() ? std::string() : server.results().back().body;
  }

  // The payload of the daily `config.read` of meshmaster.dk's key table: **thirty** keys, of
  // which `regions` is two commands and `acl` the binary access-list request, so the read is
  // 30 + the two extras + 1 = 33 transactions and `kThirtyKeyRead` **34** with its login -
  // EXIT-NODES §7's `config.read` row, and the shape of job 22032, the read that reset a Walter
  // on 7 November 2026. Every key the fake's repeater does not already hold is given a value
  // here, so that what these tests measure is the pacing and nothing else.
  std::string thirtyKeyPayload() {
    static const char* const keys[] = {
        "name",        "lat",              "lon",           "owner.info",       "advert.interval",
        "dutycycle",   "repeat",           "af",            "flood.max",        "flood.advert.interval",
        "flood.max.unscoped", "flood.max.advert", "path.hash.mode", "loop.detect", "txdelay",
        "direct.txdelay", "rxdelay",       "int.thresh",    "cad",              "multi.acks",
        "allow.read.only", "agc.reset.interval", "tx",      "radio",            "radio.rxgain",
        "radio.fem.rxgain", "radio.fem.txgain", "freq",     "regions",          "acl"};
    std::string json = "{\"keys\":[";
    for (const char* key : keys) {
      if (json.back() != '[') json += ",";
      json += std::string("\"") + key + "\"";
      const bool isGet = std::strcmp(key, "regions") != 0 && std::strcmp(key, "acl") != 0;
      if (isGet && mesh.config().count(key) == 0) mesh.setConfig(key, "0");
    }
    return json + "]}";
  }
  static constexpr int kThirtyKeyRead = 34;

  std::string outcomeOf(const std::string& body) const { return memberOf(body, "outcome"); }
  // Where a failed job failed (task p1.7-28), or "" when the result carries no such member -
  // which is what `memberOf` gives for an absent one, and an absent one is the point of half the
  // tests below.
  std::string failedAtOf(const std::string& body) const { return memberOf(body, "failedAt"); }
  // The tier a job that ran **below** the class it asked for ran at (meshmaster.dk's task
  // p4.30-180), or "" when the envelope carries no such member - which is every job that ran at
  // the class it asked for, and is half of what the tests below assert.
  //
  // **The envelope's own member, not the one in `data`.** `LoginBelowClassResult` is the one
  // result record that repeats a member name of the envelope (`{"loggedInAs","needed"}`), which
  // is exactly the case these tests have to tell apart: a refusal says the tier in its data and
  // nothing on the envelope. So the top-level keys decide whether it is there at all, and the
  // value is read with `data` cut out.
  std::string loggedInAsOf(const std::string& body) const {
    const std::vector<std::string> keys = fake::keysOf(body);
    if (std::find(keys.begin(), keys.end(), "loggedInAs") == keys.end()) return "";
    std::string envelope = body;
    const std::string data = dataOf(body);
    const size_t at = data.empty() ? std::string::npos : envelope.find(data);
    if (at != std::string::npos) envelope.erase(at, data.size());
    return memberOf(envelope, "loggedInAs");
  }
  // The clock the last answered login of the job showed, and this board's own at that moment
  // (meshmaster.dk's task p4.37-210), or "" when the envelope carries no such member - which is
  // every job that made no login of its own, every job the mesh answered without the air, and
  // every result of a board whose own clock is not set. Neither name appears in any result
  // record, so the top-level member is the only one there is to find.
  std::string repeaterClockOf(const std::string& body) const { return memberOf(body, "repeaterClockS"); }
  std::string agentClockOf(const std::string& body) const { return memberOf(body, "agentClockS"); }

  // **The envelope of a job that carries `loggedInAs` *and* the clock pair**, which no single
  // fixture has: `JobResultReportAtGuest.json` ends at `loggedInAs` and
  // `JobResultReportWithClock.json` never carries one. `JobResultReport`'s own parameter order
  // ends `… log, failedAt, loggedInAs, repeaterClockS, agentClockS`, and each fixture proves
  // half of it - so a body that follows **both**, with the other half cut out, follows the
  // record. An `acl`-class read a guest login served and which made that login itself is the one
  // result with both halves at once.
  ::testing::AssertionResult followsBothFixtures(const std::string& body) const {
    const std::size_t clockAt = body.find(",\"repeaterClockS\":");
    if (clockAt == std::string::npos) return ::testing::AssertionFailure() << "no clock members: " << body;
    const ::testing::AssertionResult tierLast =
        fake::followsShapeOf(body.substr(0, clockAt) + "}", fake::contract("JobResultReportAtGuest"));
    if (!tierLast) return tierLast;
    const std::size_t tierAt = body.find(",\"loggedInAs\":\"");
    if (tierAt == std::string::npos) return ::testing::AssertionFailure() << "no loggedInAs: " << body;
    std::string withoutTheTier = body;
    withoutTheTier.erase(tierAt, body.find(',', tierAt + 1) - tierAt);
    return fake::followsShapeOf(withoutTheTier, fake::contract("JobResultReportWithClock"));
  }
  std::string dataOf(const std::string& body) const { return memberOf(body, "data"); }
  std::string logOf(const std::string& body) const { return memberOf(body, "log"); }
  int transactionsOf(const std::string& body) const { return std::atoi(memberOf(body, "transactions").c_str()); }
  // Every `prefix` of a result's hops, in order: a trace's round trip, or a neighbours page.
  std::vector<std::string> prefixesOf(const std::string& data) const {
    std::vector<std::string> out;
    const std::string needle = "\"prefix\":\"";
    size_t at = 0;
    while ((at = data.find(needle, at)) != std::string::npos) {
      at += needle.size();
      const size_t end = data.find('"', at);
      if (end == std::string::npos) break;
      out.push_back(data.substr(at, end - at));
      at = end;
    }
    return out;
  }

  // The repeater's own neighbour count out of a neighbours result (task p1.15-56), or -1 when
  // the result does not carry one - which after this task is only a job that wrote no result.
  int totalOf(const std::string& data) const {
    const std::string said = memberOf(data, "total");
    return said.empty() ? -1 : std::atoi(said.c_str());
  }

  fake::FakePlatform platform;
  fake::FakeMesh mesh{platform};
  fake::FakeUplink server{platform};
  mm::JobRunner runner;
};

// ---------------------------------------------------------------------------------------------
// The fake's citations

// Every answer of FakeMesh names a companion fixture or a function of the firmware at the pin,
// and the manifest at the top of that file is the list - walked against the body, both ways, the
// way test_contract_fixtures walks its own manifest. A fake nobody can check is a fake that
// drifts; this is what makes a MeshCore bump re-read it (tasks/sync/TEMPLATE.md).
TEST(Citations, EveryAnswerOfTheFakeCitesAFixtureOrAFirmwareFunction) {
  const fs::path root(PROJECT_DIR);
  const std::string text = fake::readWholeFile(root / "test" / "support" / "FakeMesh.h");
  ASSERT_FALSE(text.empty());

  // The pin and the tag platformio.ini names, which every `fw` citation must agree with.
  const std::string ini = fake::readWholeFile(root / "platformio.ini");
  std::string pin;
  std::string tag;
  {
    std::istringstream lines(ini);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind("pin = ", 0) == 0 && pin.empty()) pin = line.substr(6);
      if (line.rfind("tag = ", 0) == 0 && tag.empty()) tag = line.substr(6);
      while (!pin.empty() && (pin.back() == '\r' || pin.back() == ' ')) pin.pop_back();
      while (!tag.empty() && (tag.back() == '\r' || tag.back() == ' ')) tag.pop_back();
    }
  }
  ASSERT_EQ(pin.size(), 40u) << "platformio.ini's [meshcore].pin";
  ASSERT_FALSE(tag.empty());
  // The fixtures cite the same commit under two names - the repeater build and the companion
  // build - and so does this file.
  std::set<std::string> tags = {tag};
  if (tag.rfind("companion-", 0) == 0) tags.insert("repeater-" + tag.substr(10));

  // The manifest: `// answer: <name>  cite: <citation>`, between its two markers.
  const size_t from = text.find("---- answers (the manifest) ---");
  const size_t to = text.find("---- end of the manifest ---");
  ASSERT_NE(from, std::string::npos) << "FakeMesh.h has no answer manifest";
  ASSERT_NE(to, std::string::npos);
  ASSERT_LT(from, to);

  std::set<std::string> declared;
  int citations = 0;
  const bool haveMeshCore = fs::is_directory(root / ".pio" / "libdeps" / "heltec_v3" / "MeshCore");
  std::istringstream manifest(text.substr(from, to - from));
  std::string line;
  while (std::getline(manifest, line)) {
    const size_t answer = line.find("answer: ");
    if (answer == std::string::npos) continue;
    const size_t cite = line.find("cite: ");
    ASSERT_NE(cite, std::string::npos) << "the manifest line names an answer and no citation: " << line;
    std::string name = line.substr(answer + 8, cite - answer - 8);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    ASSERT_FALSE(name.empty()) << line;
    declared.insert(name);
    citations++;

    std::istringstream words(line.substr(cite + 6));
    std::string kind;
    words >> kind;
    if (kind == "fixture") {
      std::string file;
      words >> file;
      EXPECT_TRUE(fs::is_regular_file(root / "tests" / "fixtures" / "companion" / file))
          << name << " cites a fixture that is not in the tree: " << file;
      continue;
    }
    ASSERT_EQ(kind, "fw") << "a citation is a `fixture` or a `fw` and nothing else: " << line;
    std::string citedTag;
    std::string citedPin;
    std::string where;
    std::string function;
    words >> citedTag >> citedPin >> where >> function;
    EXPECT_TRUE(tags.count(citedTag) > 0) << name << " cites the tag " << citedTag << ", and the pin is " << tag;
    EXPECT_EQ(pin.rfind(citedPin, 0), 0u) << name << " cites the commit " << citedPin << ", and the pin is " << pin;
    const size_t colon = where.rfind(':');
    ASSERT_NE(colon, std::string::npos) << "a `fw` citation names a file and a line range: " << line;
    const std::string file = where.substr(0, colon);
    const std::string range = where.substr(colon + 1);
    const size_t dash = range.find('-');
    ASSERT_NE(dash, std::string::npos) << "a `fw` citation names a line range: " << line;
    const int firstLine = std::atoi(range.substr(0, dash).c_str());
    const int lastLine = std::atoi(range.substr(dash + 1).c_str());
    EXPECT_GT(firstLine, 0) << line;
    EXPECT_GE(lastLine, firstLine) << line;
    EXPECT_FALSE(function.empty()) << line;

    if (!haveMeshCore) continue;
    // The source is on disk after a build, so the citation is checked against it: the file is
    // there, it is long enough for the lines claimed, and the function the citation names is
    // **defined at or above** the first of them - a citation names a range inside a function, so
    // the name itself is usually a few lines above it. That is what catches a function upstream
    // renamed or moved, which is the mistake a bump makes. CI's native job has no MeshCore (it
    // installs the host dependencies alone), which is why this half is conditional and the form
    // above is not.
    const fs::path source = root / ".pio" / "libdeps" / "heltec_v3" / "MeshCore" / file;
    ASSERT_TRUE(fs::is_regular_file(source)) << name << " cites " << file << ", which the pinned MeshCore has not got";
    const std::string signature = function + "(";
    std::ifstream in(source);
    std::string sourceLine;
    int n = 0;
    int definedAt = 0;
    while (std::getline(in, sourceLine)) {
      n++;
      if (n <= firstLine && sourceLine.find(signature) != std::string::npos) definedAt = n;
    }
    EXPECT_GE(n, lastLine) << name << " cites " << where << ", and that file has " << n << " lines";
    EXPECT_GT(definedAt, 0) << name << " cites " << where << " for " << function
                            << ", and nothing at or above line " << firstLine << " of the pinned source defines it";
  }
  EXPECT_GE(citations, 30) << "the manifest is shorter than the answers this fake gives";

  // And the body: every `// answer: <name>` in the code is declared, and every declared answer
  // is produced somewhere in the code. A citation for an answer nobody gives is as wrong as an
  // answer nobody cited.
  std::set<std::string> produced;
  size_t at = to;
  while ((at = text.find("// answer: ", at)) != std::string::npos) {
    at += 11;
    size_t end = at;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r') end++;
    produced.insert(text.substr(at, end - at));
  }
  for (const std::string& name : produced) {
    EXPECT_TRUE(declared.count(name) > 0) << "FakeMesh.h gives the answer '" << name << "' and the manifest has no citation for it";
  }
  for (const std::string& name : declared) {
    EXPECT_TRUE(produced.count(name) > 0) << "the manifest cites '" << name << "' and no answer of the fake is marked with it";
  }
}

// The login bytes the fake builds are the bytes `login-success.hex` carries, inverted through the
// companion's own composition (the table in test/test_reply_codec): if the two ever part, one of
// the two readings of handleLoginReq is wrong.
TEST(Citations, TheFakesLoginReplyIsTheFixturesBytes) {
  const auto fixture = fake::body("login-success");  // is_admin(1), prefix(6), ts(4), acl(1), level(1)
  ASSERT_GE(fixture.size(), 13u);

  fake::FakePlatform platform;
  fake::FakeMesh mesh(platform);
  platform.fakeClock().setFromNetwork(kNow);
  mesh.setKey(repeater());
  mesh.setSelfKey(self());
  mesh.setAdminPassword("adm1n-pass");
  ASSERT_TRUE(mesh.beginJob(repeater().c_str(), nullptr, 0, 0));
  uint32_t tag = 0;
  ASSERT_NE(mesh.sendLogin("adm1n-pass", tag), mm::SendResult::Failed);
  ASSERT_TRUE(mesh.awaitReply(tag, 16000, nullptr));

  const mm::MeshReply& reply = mesh.reply();
  ASSERT_GE(reply.len, 9);
  EXPECT_EQ(reply.data[0], 0x00) << "RESP_SERVER_LOGIN_OK";
  EXPECT_EQ(reply.data[1], 0x00) << "the legacy keep-alive field";
  EXPECT_EQ(reply.data[2], fixture[0]) << "is_admin, which the companion puts in byte 1 of its frame";
  EXPECT_EQ(reply.data[3], fixture[11]) << "the ACL permission byte, byte 12 of the companion's frame";
  EXPECT_EQ(reply.data[8], fixture[12]) << "FIRMWARE_VER_LEVEL, byte 13 of the companion's frame";

  mm::LoginReply login;
  ASSERT_TRUE(mm::parseLoginReply(reply.data, reply.len, login));
  EXPECT_STREQ(mm::permissionName(login.role()), "admin");
  EXPECT_TRUE(login.hasFirmwareLevel);
  EXPECT_EQ(login.firmwareLevel, 2);
}

// ---------------------------------------------------------------------------------------------
// The four reads

// A guest-class read asks for the owner-info reply too since meshmaster.dk's task p4.30-181:
// `handleRequest` checks no permission at all for `REQ_TYPE_GET_OWNER_INFO`, and the node name
// and the owner info are wanted for every repeater a person can see - somebody else's above all,
// which is exactly the repeater a guest-door pair reads.
TEST_F(RunnerTest, AStatusReadAtGuestAsksForTheOwnerInfoToo) {
  mesh.setGuestPassword("gu3st-pass");
  const std::string body = run(fake::Job(12345, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the status request and the owner-info request (EXIT-NODES §7)";
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetStatus), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 1) << "at every class since task p4.30-181";

  // The envelope and the data, against the shapes ContractsJson writes. The envelope's fixture is
  // `JobResultReportWithClock.json` since meshmaster.dk's task p4.37-210: this job made a login
  // of its own, so it carries the two clock members, and that file is `JobResultReport.json`'s
  // members plus those two, in the record's order.
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReportWithClock")));
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("StatusResult")));
  // The values are status-response.hex's, which test/test_reply_codec asserts for the same bytes.
  EXPECT_NE(data.find("\"batteryMv\":4012"), std::string::npos) << data;
  EXPECT_NE(data.find("\"uptimeS\":864000"), std::string::npos) << data;
  EXPECT_NE(data.find("\"lastSnr\":6.25"), std::string::npos) << data;
  // And the owner-info reply's three lines, from binary-response-owner-info.hex.
  EXPECT_NE(data.find("\"firmware\":\"v1.17.1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"nodeName\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"ownerInfo\":\"TwinOak\""), std::string::npos) << data;
  EXPECT_EQ(memberOf(body, "pathUsed"), "[\"a1\",\"7f\"]") << "the path the claim carried, reported back";
  EXPECT_EQ(memberOf(body, "hops"), "2");
}

TEST_F(RunnerTest, AStatusReadAtAclAlsoFetchesTheOwnerInfo) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body =
      run(fake::Job(12345, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the status request and the owner-info request (§7)";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 1);
  EXPECT_NE(dataOf(body).find("\"firmware\":\"v1.17.1\""), std::string::npos) << dataOf(body);
  // A blank login against an entry at read-only is answered read-only, and that is enough for
  // class acl: no restore, because the session is not admin.
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 0);
}

TEST_F(RunnerTest, AStatusReadTheClaimSaysNeedsNoOwnerInfoSpendsNothingOnIt) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12345, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 0);
  const std::string data = dataOf(body);
  EXPECT_EQ(data.find("\"firmware\""), std::string::npos) << data;
  EXPECT_EQ(data.find("\"nodeName\""), std::string::npos) << data;
  EXPECT_EQ(data.find("\"ownerInfo\""), std::string::npos) << data;
}

// And the same at guest: the payload is the whole of the claim's say, at every class.
TEST_F(RunnerTest, AGuestStatusReadTheClaimSaysNeedsNoOwnerInfoDoesNotAsk) {
  mesh.setGuestPassword("gu3st-pass");
  const std::string body = run(fake::Job(12345, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 0);
  EXPECT_EQ(dataOf(body).find("\"ownerInfo\""), std::string::npos) << dataOf(body);
}

// **Not below level 2.** `REQ_TYPE_GET_OWNER_INFO` is commented `FIRMWARE_VER_LEVEL >= 2`, and a
// repeater that does not know a request type answers it with silence - so asking an older one
// buys a timeout and nothing else. The level the login reply carried is what says so, and the
// log says why, which is the difference between a request that was skipped and one that failed.
TEST_F(RunnerTest, ARepeaterWhoseLoginSaidFirmwareLevelOneIsNotAskedForTheOwnerInfo) {
  mesh.setFirmwareVerLevel(1);
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body =
      run(fake::Job(12345, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2) << "the login and the status request; the owner info was never asked for";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 0);
  const std::string data = dataOf(body);
  EXPECT_EQ(data.find("\"firmware\""), std::string::npos) << data;
  EXPECT_EQ(data.find("\"nodeName\""), std::string::npos) << data;
  EXPECT_EQ(data.find("\"ownerInfo\""), std::string::npos) << data;
  EXPECT_NE(memberOf(body, "log").find("owner info not asked for: the login said firmware level 1"),
            std::string::npos)
      << memberOf(body, "log");
}

// An owner info of two lines - `set owner.info a|b`, which the firmware stores with a newline -
// reaches the server as one value with its newline kept and escaped. And an empty one is `""`
// and not left out: *the owner has written nothing* is not *nobody has asked*.
TEST_F(RunnerTest, TheOwnerInfosOwnNewlinesTravelAndAnEmptyOneIsStillAValue) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setOwnerInfoReply("v1.18.0\nGrenaa Chimney #1\nTwinOak\nthomas@twinoak.dk");
  const std::string twoLines =
      run(fake::Job(12345, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(twoLines), "ok");
  EXPECT_NE(dataOf(twoLines).find("\"ownerInfo\":\"TwinOak\\nthomas@twinoak.dk\""), std::string::npos)
      << dataOf(twoLines);

  mesh.setOwnerInfoReply("v1.18.0\nGrenaa Chimney #1\n");
  const std::string empty =
      run(fake::Job(12346, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(empty), "ok");
  EXPECT_NE(dataOf(empty).find("\"nodeName\":\"Grenaa Chimney #1\",\"ownerInfo\":\"\"}"), std::string::npos)
      << dataOf(empty);

  mesh.setOwnerInfoReply("v1.18.0");
  const std::string versionOnly =
      run(fake::Job(12347, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(versionOnly), "ok");
  EXPECT_NE(dataOf(versionOnly).find("\"firmware\":\"v1.18.0\"}"), std::string::npos) << dataOf(versionOnly);
  EXPECT_EQ(dataOf(versionOnly).find("\"nodeName\""), std::string::npos) << dataOf(versionOnly);
}

// A value longer than the firmware's own field is kept, cut, and the job's log says it was.
//
// **No stock repeater can send one.** `node_name[32]` and `owner_info[120]` bound the reply at
// 8 + 31 + 1 + 119 = 159 bytes and `Mesh::createDatagram` refuses a plaintext past 167, so the
// two cannot even be over-long at once - which is why the two cases are two reads here. A
// firmware with wider fields is what this guards against, and neither its name nor its note is
// dropped for being long: it arrives cut, and the log says so rather than the server guessing.
TEST_F(RunnerTest, ACutNameOrOwnerInfoSaysSoInTheLog) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setOwnerInfoReply("v1.18.0\n" + std::string(mm::kMaxNodeNameLen + 3, 'n') + "\nTwinOak");
  const std::string name =
      run(fake::Job(12345, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(name), "ok");
  EXPECT_NE(dataOf(name).find("\"nodeName\":\"" + std::string(mm::kMaxNodeNameLen, 'n') + "\",\"ownerInfo\":\"TwinOak\""),
            std::string::npos)
      << dataOf(name);
  EXPECT_NE(memberOf(name, "log").find("the node name was longer than the 31 characters"), std::string::npos)
      << memberOf(name, "log");

  mesh.setOwnerInfoReply("v1.18.0\nGrenaa\n" + std::string(mm::kMaxOwnerInfoLen + 5, 'o'));
  const std::string info =
      run(fake::Job(12346, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(info), "ok");
  EXPECT_NE(dataOf(info).find("\"ownerInfo\":\"" + std::string(mm::kMaxOwnerInfoLen, 'o') + "\""), std::string::npos)
      << dataOf(info);
  EXPECT_NE(memberOf(info, "log").find("the owner info was longer than the 119 characters"), std::string::npos)
      << memberOf(info, "log");
}

// A job the mesh answers **without the air** (tasks p1.10-40 and 41) follows the same rule: the
// claim's payload decides, the request is made through the door, and it costs the airtime budget
// nothing - which is the point of the door and not an exception to this rule.
TEST_F(RunnerTest, ALocalJobAsksForTheOwnerInfoOnTheSameRuleAndSpendsNoAirtime) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setLocalDoorKey(repeater());
  const std::string body =
      run(fake::Job(12345, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 0) << "none of it was on the air";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 1);
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"nodeName\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"ownerInfo\":\"TwinOak\""), std::string::npos) << data;

  // And the claim still has the last word on a local job as on one on the air.
  mesh.forgetSends();
  const std::string quiet = run(fake::Job(12346, "status")
                                    .atClass("acl")
                                    .leasedUntil(kLease)
                                    .withAclLevel(mm::kPermReadOnly)
                                    .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(quiet), "ok");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 0);
}

TEST_F(RunnerTest, ATelemetryReadDecodesTheChannelsTheRepeaterSent) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body =
      run(fake::Job(12346, "telemetry").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("TelemetryResult")));
  // telemetry-response.hex's four channels, in the firmware's own order.
  EXPECT_NE(data.find("\"type\":\"voltage\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"type\":\"humidity\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"type\":\"pressure\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"type\":\"temperature\""), std::string::npos) << data;
}

TEST_F(RunnerTest, NeighboursAtGuestPagesByNine) {
  mesh.setGuestPassword("gu3st-pass");
  mesh.setNeighbours(neighbourList(12));

  const std::string body = run(fake::Job(12347, "neighbours")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 2) << "nine a page, so twelve is two pages (§7)";
  EXPECT_EQ(transactionsOf(body), 3) << "the login and two pages";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("NeighboursResultTotal")));
  EXPECT_EQ(prefixesOf(data).size(), 12u);
  EXPECT_EQ(totalOf(data), 12) << "a whole list: as many entries as the repeater said it has";
}

// ------------------------------------------------------------- a listing says how many there were
//
// **The result carries the repeater's own count** (task p1.15-56). A list is read nine to a
// page, and a page that goes unanswered after an earlier one came back ends the reading rather
// than the job: the server gets what was heard rather than nothing at all. Until now it got
// nothing to tell that from a whole list, so it took the nine for all there were, counted the
// other eight as gone and found them again as new at the next listing. `total` is the figure
// every page of the reply carries, as the last page that was read gave it; meshmaster.dk's task
// 239 is what reads it. The pages asked for, the tries, the order, the log's lines and the
// outcome of every case are what they were.

TEST_F(RunnerTest, AWholeListIsAsManyEntriesAsTheRepeatersOwnCount) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(17));
  const std::string body =
      run(fake::Job(25601, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 2) << "nine a page: seventeen is two";
  const std::string data = dataOf(body);
  EXPECT_EQ(prefixesOf(data).size(), 17u);
  EXPECT_EQ(totalOf(data), 17);
  EXPECT_EQ(logOf(body).find("the list stops at"), std::string::npos) << "nothing stopped: " << body;
}

TEST_F(RunnerTest, AListThatStopsAtAPageSaysHowManyThereWereAndItsLogIsWhatItWas) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(17));
  mesh.silenceTheNeighboursPageFrom(2);
  const std::string body =
      run(fake::Job(25602, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << "a page that never came back after one did is still a reading";
  EXPECT_EQ(failedAtOf(body), "") << "the job did not fail";
  const std::string data = dataOf(body);
  EXPECT_EQ(prefixesOf(data).size(), 9u);
  EXPECT_EQ(totalOf(data), 17) << "nine of seventeen, and the result says so: " << data;
  // The log's two lines are the ones it wrote before this task, word for word.
  EXPECT_NE(logOf(body).find("the list stops at 9 of 17"), std::string::npos) << body;
  EXPECT_NE(logOf(body).find("9 of the repeater's 17 neighbour(s) reported"), std::string::npos) << body;
}

TEST_F(RunnerTest, APageThatDoesNotParseAfterOneThatDidStopsTheListAndKeepsTheCount) {
  // A reply with fewer than the four bytes of `total(i16)`, `returned(i16)`, which is the only
  // neighbours page `parseNeighboursPage` refuses. The repeater's padding is what makes a reply
  // that short impossible on the air, so it goes off for this one (FakeMesh's own note).
  mesh.setPadding(false);
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(17));
  mesh.truncateTheNeighboursPage(2);
  const std::string body =
      run(fake::Job(25603, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  const std::string data = dataOf(body);
  EXPECT_EQ(prefixesOf(data).size(), 9u);
  EXPECT_EQ(totalOf(data), 17) << data;
  EXPECT_NE(logOf(body).find("a neighbours page did not parse"), std::string::npos) << body;
}

TEST_F(RunnerTest, AListCutAtTheCapSaysTheRepeatersWholeCount) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(60));
  const std::string body =
      run(fake::Job(25604, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 6) << "six pages of nine is kMaxNeighbours";
  const std::string data = dataOf(body);
  EXPECT_EQ(prefixesOf(data).size(), static_cast<std::size_t>(mm::kMaxNeighbours));
  EXPECT_EQ(totalOf(data), 60) << "fifty-four of sixty, and the result says which: " << data;
  EXPECT_NE(logOf(body).find("54 of the repeater's 60 neighbour(s) reported"), std::string::npos) << body;
}

TEST_F(RunnerTest, ARepeaterWithNoNeighboursIsAnEmptyListAndACountOfNought) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours({});
  const std::string body =
      run(fake::Job(25605, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(dataOf(body), "{\"neighbours\":[],\"total\":0}") << body;
}

TEST_F(RunnerTest, TheCountIsTheOneTheLastPageReadGave) {
  // A neighbour heard between the two pages: the repeater says seventeen on the first page and
  // eighteen on every page after it, and the newer figure is the one the result carries.
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(17));
  mesh.sayTheNeighbourTotalFromPage(2, 18);
  const std::string body =
      run(fake::Job(25606, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  const std::string data = dataOf(body);
  EXPECT_EQ(prefixesOf(data).size(), 17u);
  EXPECT_EQ(totalOf(data), 18) << data;
}

TEST_F(RunnerTest, AFirstPageThatNeverComesBackIsTheJobItAlwaysWasAndCarriesNoResult) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours(neighbourList(17));
  mesh.silenceTheNeighboursPageFrom(1);
  const std::string body =
      run(fake::Job(25607, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_EQ(failedAtOf(body), "request") << body;
  EXPECT_EQ(dataOf(body), "") << "nothing was heard, so there is no reading to report: " << body;
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 2) << "the two tries every request has";
  EXPECT_EQ(logOf(body).find("the list stops at"), std::string::npos) << body;
}

TEST_F(RunnerTest, ATraceReportsTheRoundTripAndThisBoardsOwnHopLast) {
  const std::string body = run(fake::Job(12348, "trace").atClass("none").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 1) << "a trace asks nobody for a session (§7)";
  EXPECT_EQ(mesh.logins(), 0);
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("TraceResult")));
  // Out along a1, 7f, the repeater's own first byte, and back: five hops, and this board's own
  // hash appended with the SNR it measured coming home.
  int hops = 0;
  size_t at = 0;
  while ((at = data.find("\"prefix\":", at)) != std::string::npos) {
    hops++;
    at += 9;
  }
  EXPECT_EQ(hops, 6);
  EXPECT_NE(data.find("\"prefix\":\"a1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"prefix\":\"7f\""), std::string::npos) << data;
}

// The one width the library chooses for itself (task p1.10-39): a direct neighbour has no learned
// hop to take a width from, and until now `runTrace` filled in one byte - which is how job 20005
// of 5 November 2026 reported an `86` that any of three repeaters within earshot could have sent.
// The width is the board's own, which the mesh answers with (IMesh::floodPathHashSize).
TEST_F(RunnerTest, ATraceToADirectNeighbourGoesAtTheBoardsWidth) {
  const std::string body = run(fake::Job(12352, "trace").atClass("none").leasedUntil(kLease).asDirectNeighbour());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 1) << "a trace asks nobody for a session (§7)";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("TraceResult")));
  // The round trip of a direct neighbour is one hash out - the repeater's own, computed from its
  // key - and this board's own appended on the way home. Two bytes each: four hex digits, not two.
  const std::vector<std::string> hops = prefixesOf(data);
  ASSERT_EQ(hops.size(), 2u) << data;
  EXPECT_EQ(hops[0], repeater().substr(0, 4)) << data;
  EXPECT_EQ(hops[1], self().substr(0, 4)) << data;
}

// And that width is the **mesh's** answer, not a number in the library: a mesh that floods wider
// traces wider, which is what the method is for.
TEST_F(RunnerTest, TheWidthOfADirectNeighboursTraceIsTheMeshsOwnAnswer) {
  mesh.setFloodPathHashSize(4);
  const std::string body = run(fake::Job(12353, "trace").atClass("none").leasedUntil(kLease).asDirectNeighbour());

  EXPECT_EQ(outcomeOf(body), "ok");
  const std::vector<std::string> hops = prefixesOf(dataOf(body));
  ASSERT_EQ(hops.size(), 2u) << body;
  EXPECT_EQ(hops[0], repeater().substr(0, 8)) << body;
  EXPECT_EQ(hops[1], self().substr(0, 8)) << body;
}

// A path the claim carried keeps its own width, as it always did: the library never widens one,
// because the extra byte of a narrow hop belongs to no node.
TEST_F(RunnerTest, ATraceAlongAClaimedPathKeepsThatPathsWidth) {
  const std::vector<std::string> one =
      prefixesOf(dataOf(run(fake::Job(12354, "trace").atClass("none").leasedUntil(kLease).alongPath({"a1", "7f"}))));
  ASSERT_EQ(one.size(), 6u);
  for (const std::string& hop : one) EXPECT_EQ(hop.size(), 2u) << hop;
  EXPECT_EQ(one[0], "a1");
  EXPECT_EQ(one[2], repeater().substr(0, 2));
  EXPECT_EQ(one[5], self().substr(0, 2));

  const std::vector<std::string> two = prefixesOf(
      dataOf(run(fake::Job(12355, "trace").atClass("none").leasedUntil(kLease).alongPath({"a1b2", "7f80"}))));
  ASSERT_EQ(two.size(), 6u);
  for (const std::string& hop : two) EXPECT_EQ(hop.size(), 4u) << hop;
  EXPECT_EQ(two[0], "a1b2");
  EXPECT_EQ(two[2], repeater().substr(0, 4));
  EXPECT_EQ(two[5], self().substr(0, 4));
}

TEST_F(RunnerTest, ATraceAlongThreeByteHashesIsUnsupportedAtNoTransaction) {
  const std::string body = run(
      fake::Job(12349, "trace").atClass("none").leasedUntil(kLease).alongPath({"a1b2c3", "d4e5f6"}));

  EXPECT_EQ(outcomeOf(body), "unsupported") << "a trace packet states its hash size as an exponent: no three";
  EXPECT_EQ(transactionsOf(body), 0);
  EXPECT_EQ(mesh.sendCount(), 0);
  EXPECT_NE(logOf(body).find("1, 2, 4 or 8"), std::string::npos) << body;
}

// ---------------------------------------------------------------------------------------------
// discover, and the guest door of pile 1.4

TEST_F(RunnerTest, DiscoverAtGuestWithThePasswordLearnsThePathFromTheLoginsReply) {
  mesh.setGuestPassword("gu3st-pass");
  mesh.setReturnPath({0x11, 0x22, 0x33, 0x44}, 2);  // two hops of two-byte hashes
  const std::string body = run(fake::Job(12350, "discover")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .withNoPath());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 1) << "the flooded login and nothing else (§7)";
  EXPECT_EQ(mesh.logins(), 1);
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("DiscoverResult")));
  EXPECT_NE(data.find("\"hops\":[\"1122\",\"3344\"]"), std::string::npos) << data;
  EXPECT_NE(data.find("\"source\":\"login\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"loggedInAs\":\"guest\""), std::string::npos) << data;
}

TEST_F(RunnerTest, DiscoverAtGuestWithNoPasswordIsTheBlankLoginOfTheEmptyDoor) {
  // A repeater whose owner never set a guest password answers the blank login *at guest*, and
  // the door's pairing step runs on it (task p1.4-14; EXIT-NODES §7's guest row).
  mesh.setGuestPassword("");
  mesh.setReturnPath({0x11, 0x22}, 2);
  const std::string body =
      run(fake::Job(12351, "discover").atClass("guest").leasedUntil(kLease).withNoPath());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_NE(dataOf(body).find("\"loggedInAs\":\"guest\""), std::string::npos) << dataOf(body);
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest) << "the blank login wrote a guest entry for our key";
}

TEST_F(RunnerTest, DiscoverAtGuestWithNoPasswordOnARepeaterThatHasOneIsTheSilenceItIsMeantToBe) {
  mesh.setGuestPassword("something-else");
  const std::string body =
      run(fake::Job(12352, "discover").atClass("guest").leasedUntil(kLease).withNoPath());

  EXPECT_EQ(outcomeOf(body), "unreachable") << "silence, by design on both sides (EXIT-NODES §4.1 step 2)";
  // A discover with no path held is one flooded login and then the path-discovery request, tried
  // once and last - §7's "1, at worst 3", of which the direct login is the one a known path buys.
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(transactionsOf(body), 2);
  ASSERT_EQ(mesh.sendCount(), 2);
  EXPECT_EQ(mesh.sends()[1].kindName(), std::string("discovery")) << "the last resort, once";
  EXPECT_NE(logOf(body).find("the last resort"), std::string::npos) << body;
}

// ---------------------------------------------------------------------------------------------
// The three that carry a password, and the restore

TEST_F(RunnerTest, AnAclSetWritesTheEntryAndSendsNoRestoreOfItsOwn) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12360, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + stranger() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2) << "the login and the setperm (§7)";
  EXPECT_EQ(mesh.timesSent("setperm " + stranger() + " 1"), 1);
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 0)
      << "an acl.set sends no second setperm: the command is the level (task p4.1-60)";
  EXPECT_EQ(mesh.levelOf(stranger()), mm::kPermReadOnly);
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("AclSetResult")));
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
}

TEST_F(RunnerTest, AnAclSetOnOurOwnKeyIsSentOnceAndKeepsNoSession) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12361, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 1) << "once, and it is the restore as well";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly);
  EXPECT_NE(logOf(body).find("the entry set is this exit node's own"), std::string::npos) << body;
  EXPECT_NE(logOf(body).find("the session is not kept, so the next job for this repeater logs in by key"),
            std::string::npos)
      << "a confirmed grant keeps no session (task p1.10-48)";
}

// ---------------------------------------------------------------------------------------------
// A line that cannot be asked twice (task p1.10-48; meshmaster.dk's p4.32-188 and p4.32-189)
//
// `setperm <own key> <level>` is the session's own level, so a repeater that ran it drops the
// retry without a word: the line is sent **once** and its silence read back with a blank login,
// whose reply carries the entry's permission byte. Four readings, and the second round.

TEST_F(RunnerTest, AGrantWhoseOkWasLostIsConfirmedByTheBlankLogin) {
  // The field case: job 4558 of meshmaster.dk's association 14 and nineteen others of 12
  // November 2026. The admin login was answered, the line ran, and its "OK" never came home.
  const std::string line = "setperm " + self() + " 1";
  mesh.loseTheNextLineReplies(line, 1);

  const std::string body = run(fake::Job(12365, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the line and the one blank login that checked it";
  EXPECT_EQ(mesh.timesSent(line), 1) << "sent once: the retry could not have been answered";
  EXPECT_EQ(mesh.logins(), 2);
  EXPECT_EQ(mesh.loginsWithMaterial(), 1) << "the check is a blank login: this board's own key and no secret";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly) << "the line did run";

  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("AclSetResult")));
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
  EXPECT_EQ(data.find("\"reply\""), std::string::npos) << "there was no reply to record: the login is the evidence";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("no answer, and this line cannot be asked twice"), std::string::npos) << log;
  EXPECT_NE(log.find("the entry check (blank login): answered at read-only"), std::string::npos) << log;
  EXPECT_NE(log.find("confirmed by the login and not by an \"OK\""), std::string::npos) << log;
}

TEST_F(RunnerTest, AGrantWhoseLineNeverArrivedIsSentOnceMoreAndThenConfirmed) {
  // The other reading of a silence: the line was lost on the way out, so the entry - and with it
  // the session - is still admin, and a second line can be answered.
  const std::string line = "setperm " + self() + " 1";
  mesh.loseTheNextLines(line, 1);

  const std::string body = run(fake::Job(12366, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 4) << "the login, the lost line, the check and the line again";
  EXPECT_EQ(mesh.timesSent(line), 2) << "the check found the key still at admin, so the line went once more";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly);
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
  EXPECT_NE(data.find("\"reply\":\"OK\""), std::string::npos) << "this one was confirmed by an \"OK\"";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the entry check (blank login): answered at admin"), std::string::npos) << log;
  EXPECT_NE(log.find("the line did not arrive, and the session is admin again - sending it once more"),
            std::string::npos)
      << log;
}

TEST_F(RunnerTest, AGrantLostTwiceIsNotConfirmedAndTheSessionIsDropped) {
  class Out : public mm::TextOut {
   public:
    void write(const char* text, size_t len) override { out.append(text, len); }
    std::string out;
  };
  const std::string line = "setperm " + self() + " 1";
  mesh.loseTheNextLines(line, 2);

  const std::string body = run(fake::Job(12367, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_EQ(transactionsOf(body), 5) << "the login, two lines and the two checks between and after them";
  EXPECT_EQ(mesh.timesSent(line), 2) << "two rounds at most";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "neither line arrived: the entry is where the login left it";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("AclSetResult")));
  EXPECT_NE(data.find("\"confirmed\":false"), std::string::npos) << data;
  EXPECT_NE(logOf(body).find("the line did not arrive twice, so it is not confirmed"), std::string::npos) << body;

  // The session is dropped whatever the line came to, so no later job spends an admin session
  // this board cannot know the repeater still honours.
  Out sessions;
  runner.reportSessions(sessions);
  EXPECT_NE(sessions.out.find("live      none"), std::string::npos) << sessions.out;
}

TEST_F(RunnerTest, ARestoreWhoseOkWasLostIsConfirmedByTheBlankLoginAndTheReadSaysSo) {
  // The same line at the other end of an admin session, which is where meshmaster.dk's
  // *elevated* came from: association 3 seven times in 39 days.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string line = "setperm " + self() + " 1";
  mesh.loseTheNextLineReplies(line, 1);

  const std::string body = run(fake::Job(12368, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 22) << "the twenty-one of task p4.1-60 and the blank login that checked the restore";
  EXPECT_EQ(mesh.timesSent(line), 1);
  EXPECT_EQ(mesh.logins(), 2);
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly) << "the restore did run";
  EXPECT_NE(dataOf(body).find("\"aclRestored\":true"), std::string::npos) << dataOf(body);
  EXPECT_NE(logOf(body).find("as granted, which the blank login confirmed and no \"OK\" did"), std::string::npos)
      << logOf(body);
}

TEST_F(RunnerTest, ARevokeIsConfirmedByABlankLoginAnsweredAtGuest) {
  // Level 0 cannot be answered at all unless our own entry was the last one in the table:
  // `applyPermissions` deletes it and moves the later entries down, and `onPeerDataRecv`
  // addresses the "OK" to whichever client sits in that slot afterwards. Here ours is the first
  // of two, so nothing comes back - and the blank login that follows is answered **at guest**,
  // because the key it carries is no longer in the access list and this repeater has no guest
  // password. That is the confirmation.
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.grant(stranger(), mm::kPermReadWrite);
  const std::string line = "setperm " + self() + " 0";

  const std::string body = run(fake::Job(12369, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":0}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the line and the blank login that checked it";
  EXPECT_EQ(mesh.timesSent(line), 1);
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest) << "the entry is off the repeater";
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
  EXPECT_NE(data.find("\"level\":0"), std::string::npos) << data;
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the entry check (blank login): answered at guest"), std::string::npos) << log;
  EXPECT_NE(log.find("this exit node's key is off it, so the line ran"), std::string::npos) << log;
  // The last sentence of this job's log - *the session is gone with the entry* - is cut by the
  // kilobyte since task p1.21-71 put the way back and each reply's route in it. What it says is
  // asserted of the store itself rather than of the log: the next `acl` job for this repeater
  // logs in again (`AfterAConfirmedGrantTheNextAclJobLogsIn`), and the entry really is off.
  EXPECT_NE(log.find("the entry set is this exit node's own"), std::string::npos) << log;
}

TEST_F(RunnerTest, ARevokeOnARepeaterWithAGuestPasswordCannotBeConfirmed) {
  // The same revoke on a repeater that **has** a guest password: a key it does not hold is
  // answered with silence, exactly as a repeater off the air answers, so nothing is known. The
  // result says the line went out and could not be confirmed, and the log has the line.
  mesh.setGuestPassword("gu3st-pass");
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.grant(stranger(), mm::kPermReadWrite);
  const std::string line = "setperm " + self() + " 0";

  const std::string body = run(fake::Job(12370, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":0}"));

  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_EQ(transactionsOf(body), 3);
  EXPECT_EQ(mesh.timesSent(line), 1) << "sent once, and not asked again";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("AclSetResult")));
  EXPECT_NE(data.find("\"confirmed\":false"), std::string::npos) << data;
  const std::string log = logOf(body);
  EXPECT_NE(log.find("cli 'setperm " + self().substr(0, 12) + "... 0'"), std::string::npos)
      << "the line that went out is in the log: " << log;
  EXPECT_NE(log.find("the blank login was not answered either"), std::string::npos) << log;
  EXPECT_NE(log.find("could not be confirmed"), std::string::npos) << log;
}

TEST_F(RunnerTest, AfterAConfirmedGrantTheNextAclJobLogsIn) {
  // The verify of EXIT-NODES §4.1 step 4 proves the repeater honours the ACL entry for this
  // board's own key, and it can prove nothing on the session the grant opened with a password.
  // meshmaster.dk's job 26077: *session reused (acl, read-only, 146 s old)*, and the pair went
  // active as *acl login ok* with no login made.
  run(fake::Job(12371, "acl.set")
          .atClass("admin")
          .leasedUntil(kLease)
          .withAdminPassword("adm1n-pass")
          .withAclLevel(mm::kPermReadOnly)
          .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));
  ASSERT_EQ(mesh.logins(), 1);
  ASSERT_EQ(mesh.levelOf(self()), mm::kPermReadOnly);

  const std::string verify = run(fake::Job(12372, "status")
                                     .atClass("acl")
                                     .leasedUntil(kLease)
                                     .withAclLevel(mm::kPermReadOnly)
                                     .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(verify), "ok") << verify;
  EXPECT_EQ(mesh.logins(), 2) << "the grant kept no session, so the verify logged in by key";
  EXPECT_EQ(mesh.loginsWithMaterial(), 1) << "and it carried no password doing it";
  EXPECT_NE(logOf(verify).find("logged in as read-only"), std::string::npos) << verify;
  EXPECT_EQ(logOf(verify).find("session reused"), std::string::npos) << verify;
}

TEST_F(RunnerTest, AConfigReadOfTheDefaultListEndsWithTheRestoreAndSaysSo) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.grant(stranger(), mm::kPermReadWrite);
  const std::string body = run(fake::Job(12362, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  // §7's `n + 2 + r` and a login, plus the restore of task p4.1-60: sixteen keys of which two
  // are not a `get`, the two extras, the two region commands, the access list, the login and
  // the restore.
  EXPECT_EQ(transactionsOf(body), 21);
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(mesh.timesSent("region"), 1);
  EXPECT_EQ(mesh.timesSent("region default"), 1);
  EXPECT_EQ(mesh.timesSent("get name"), 1);
  EXPECT_EQ(mesh.timesSent("ver"), 1);
  EXPECT_EQ(mesh.timesSent("board"), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetAccessList), 1);
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 1) << "the session's last command (task p4.1-60)";

  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ConfigReadResult")));
  EXPECT_NE(data.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"regions\":\"* F\\n dk F\\ndefault dk\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"firmware\":\"v1.17.1 (Build: 27 Sep 2025)\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"aclRestored\":true"), std::string::npos) << data;
  // The ACL as the server's AclEvidence reads it: `<prefix>:<role>` tokens. **Our own key is in
  // it at admin**, because this read's own login put it there and the access-list request comes
  // before the restore - which is exactly why `aclRestored` is in the same result, and why the
  // server must not read that entry as a session that died (task p4.1-60).
  EXPECT_NE(memberOf(data, "acl").find(self().substr(0, 12) + ":3"), std::string::npos) << data;
  EXPECT_NE(memberOf(data, "acl").find(stranger().substr(0, 12) + ":2"), std::string::npos) << data;
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly) << "and after the restore the repeater holds us at the grant";
}

TEST_F(RunnerTest, AnAdminJobThroughAPairWithAGrantEndsWithTheRestore) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12363, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the line and the restore";
  const std::vector<std::string> lines = mesh.lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "set tx 20");
  EXPECT_EQ(lines[1], "setperm " + self() + " 1") << "the restore is the session's last command";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly);
  EXPECT_TRUE(fake::followsShapeOf(dataOf(body), fake::contract("CliResult")));
}

TEST_F(RunnerTest, AnAdminJobWithNoGrantOnThePairSendsNoRestore) {
  const std::string body = run(fake::Job(12364, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_EQ(mesh.cliLines(), 1) << "nothing to restore: the claim carried no grant";
}

// ---------------------------------------------------------------------------------------------
// Nothing of ours left behind (task p1.10-50; meshmaster.dk's p4.33-191)
//
// An admin-password login writes this board's key into the repeater's access list **at admin**
// and saves it to flash, so where no grant of ours is there to be put back nothing has ever taken
// that entry off. The claim says so with `acl: 0` - *you should hold nothing here* - and the
// session's last command is then `setperm <own key> 0` rather than a restore, checked the same
// way (task p1.10-48), after which the session is gone with the entry.

TEST_F(RunnerTest, AnAdminJobThroughAPairThatHoldsNothingEndsByTakingTheKeyOff) {
  class Out : public mm::TextOut {
   public:
    void write(const char* text, size_t len) override { out.append(text, len); }
    std::string out;
  };
  // The one-off admin job of EXIT-NODES §4.3: no association at all, so nothing of ours should be
  // on this repeater once the job is over - and the entry the line takes off is the one this
  // job's own login made.
  const std::string body = run(fake::Job(12373, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(0)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the line and the removal - priced where the restore is (§7)";
  const std::vector<std::string> lines = mesh.lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "set tx 20");
  EXPECT_EQ(lines[1], "setperm " + self() + " 0") << "the removal is the session's last command";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest) << "this board's key is off the access list";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("removal: cli 'setperm <own key> 0'"), std::string::npos) << log;
  EXPECT_NE(log.find("this exit node's key is off the repeater's access list"), std::string::npos) << log;
  EXPECT_NE(log.find("nothing of ours is left behind"), std::string::npos) << log;
  EXPECT_NE(log.find("the session is gone with the entry"), std::string::npos) << log;

  // The entry *is* the session on stock firmware, so taking it off takes the session with it: no
  // later job may spend one on this repeater.
  Out sessions;
  runner.reportSessions(sessions);
  EXPECT_NE(sessions.out.find("live      none"), std::string::npos) << sessions.out;
}

TEST_F(RunnerTest, AConfigReadThatHoldsNoGrantTakesTheKeyOffAndTheReadSaysSo) {
  // The daily configuration read of a **guest** association's primary, which is the case that
  // leaves an entry behind every single day: the probe is planned at admin through a pair that
  // holds nothing. Our key is already in the table at admin - the leftover of an earlier admin
  // login, which is how meshmaster.dk's associations 14 and 6 stood on 12 November 2026 - and
  // another key sits after it, so the deletion's "OK" is addressed to that other key and never
  // comes home (task p1.10-48). The blank login is the confirmation, and no fault knob is needed
  // for any of it.
  mesh.grant(self(), mm::kPermAdmin);
  mesh.grant(stranger(), mm::kPermReadWrite);
  const std::string body = run(fake::Job(12374, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(0));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 22) << "the twenty-one of task p4.1-60 and the blank login that checked the removal";
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 0"), 1) << "sent once: this line cannot be asked twice";
  EXPECT_EQ(mesh.logins(), 2);
  EXPECT_EQ(mesh.loginsWithMaterial(), 1) << "the check is a blank login: this board's own key and no secret";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest) << "the key is off the access list";
  EXPECT_EQ(mesh.levelOf(stranger()), mm::kPermReadWrite) << "and nobody else's entry moved";

  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ConfigReadResult")));
  EXPECT_NE(data.find("\"aclRestored\":true"), std::string::npos)
      << "what became of this exit node's own entry after the read: it came off. " << data;
  // The access list was read **before** the removal, so it lists our key at the admin this read's
  // own login put it at - which is why the result has to say what the last command did.
  EXPECT_NE(memberOf(data, "acl").find(self().substr(0, 12) + ":3"), std::string::npos) << data;
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the entry check (blank login): answered at guest"), std::string::npos) << log;
  EXPECT_NE(log.find("which the blank login answered at guest confirmed and no \"OK\" did"), std::string::npos) << log;
}

TEST_F(RunnerTest, TheSameReadWithNoLevelInTheClaimSendsNothingAfterIt) {
  // The difference between *hold nothing here* and *nothing was said*: a claim with no `acl`
  // member at all is an admin **fallback** pair, whose probes carry the password every time, or a
  // job over a unit's box link - and such a session ends with neither a restore nor a removal.
  mesh.grant(self(), mm::kPermAdmin);
  mesh.grant(stranger(), mm::kPermReadWrite);
  const std::string body = run(fake::Job(12375, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 20) << "the nineteen of the read and its login, and nothing after them";
  EXPECT_EQ(mesh.cliLines(), 18) << "the read's own lines and no setperm";
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 0"), 0);
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "the entry is where this read's own login left it";
  EXPECT_EQ(dataOf(body).find("aclRestored"), std::string::npos)
      << "there was nothing to say about it: " << dataOf(body);
}

TEST_F(RunnerTest, ALevelOfZeroOnAJobOfAnotherClassChangesNothing) {
  // A blank login answered **above** the grant is put back by the job that made it (§7, the third
  // rule), and that is the one job whose session is admin with no admin password behind it. A
  // claim of another class that says 0 is no evidence that this key should hold nothing, so
  // nothing is sent: meshmaster.dk sends no such claim - `acl: 0` rides beside an admin password,
  // and an `acl`-class claim carries the level its own login expects - and this is the firmware
  // refusing to act on one if it ever arrived.
  mesh.grant(self(), mm::kPermAdmin);
  const std::string body = run(fake::Job(12376, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(0)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(transactionsOf(body), 2) << "the blank login and the status request";
  EXPECT_EQ(mesh.cliLines(), 0) << "no console line at all";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "the entry is untouched";
  EXPECT_NE(logOf(body).find("but this job's class is acl and not admin"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, ARevokeCarryingALevelOfZeroBesideItSendsNoSecondLine) {
  // meshmaster.dk's revoke is an `acl.set` of `(own key, 0)`, and since its task p4.33-191 the
  // claim beside it carries `acl: 0` as every admin claim through a pair that holds nothing does
  // (its own decision 4: the claim says what the key should be at, and which types send the
  // command is the agent's rule). The command **is** the level: one line, and no removal after it.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12377, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(0)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":0}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.cliLines(), 1) << "one line in all";
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 0"), 1) << "once, and the command is the level";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest);
  EXPECT_EQ(logOf(body).find("removal: cli"), std::string::npos) << "no second line followed it: " << logOf(body);
}

TEST_F(RunnerTest, ARebootWithALevelOfZeroSendsNoRemovalAndSaysWhy) {
  // For the reason the restore gives there: there is no session after a reboot to send a line on,
  // and a line before it would leave a session the repeater answers no console line on. The key
  // waits at admin - no login takes an entry off - and the log says so.
  mesh.grant(self(), mm::kPermAdmin);
  const std::string body = run(fake::Job(12378, "reboot")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(0));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 0"), 0);
  EXPECT_EQ(mesh.cliLines(), 1) << "the reboot and nothing else";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "the key waits at admin for the next admin job";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("no restore and no removal on this session"), std::string::npos) << log;
  EXPECT_NE(log.find("until the next admin job here takes it off"), std::string::npos) << log;
}

TEST_F(RunnerTest, ALocalJobNeverTakesItsOwnRepeatersKeyOff) {
  // A unit logs in to its own repeater as the repeater it **is** (EXIT-NODES §4.1 step 3), so
  // `setperm <own key> 0` over the box link is the repeater asked to forget itself - the job
  // meshmaster.dk's p4.25-170 refuses to queue - and there is nothing on the other side of that
  // line to put it back. No claim over that link carries a level at all; this is the firmware
  // refusing to act on one that did.
  mesh.setAnswersLocally(true);
  mesh.grant(self(), mm::kPermAdmin);
  const std::string body = run(fake::Job(12379, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAclLevel(0)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.cliLines(), 1) << "the job's own line and nothing after it";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "the ACL is as it was";
  EXPECT_NE(logOf(body).find("no removal over the local door"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, AJobThatEndedInSilenceSendsNoRemovalAndSaysTheKeyMayBeAtAdmin) {
  // As the restore is not sent there either: a line sent now would go into the same silence. What
  // differs is the remedy, because there is none of the restore's kind - no login takes an entry
  // off - so the log says what is true and names the job that will take it.
  mesh.silenceLine("set tx 20");
  const std::string body = run(fake::Job(12380, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(0)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unreachable") << body;
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 0"), 0) << "nothing is sent into a silence";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "where this job's own login put it";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the job ended unreachable, so the removal is not sent"), std::string::npos) << log;
  EXPECT_NE(log.find("no login takes an entry off"), std::string::npos) << log;
  EXPECT_NE(log.find("the next admin job here is what takes it"), std::string::npos) << log;
}

// ---------------------------------------------------------------------------------------------
// A console reply belongs to its line (task p1.10-49)
//
// A CLI reply carries no tag, so until this task the first text of CLI data from the peer while
// one was awaited was the answer - whatever it answered. `handleCommand` reflects a two-character
// prefix and a `|`, so every console line of a job now carries one and only a reply that starts
// with it is its answer. The rule is `agent/CliReplies.h`'s and the wait is `awaitCliReply`.

TEST_F(RunnerTest, AConsoleReplyThatCameTooLateIsNotStoredAsTheNextLinesValue) {
  // The swap the task exists for, which nobody has seen because nothing would show it: a `get`
  // answers a value and not the key it belongs to. `get name` is answered too late for its own
  // wait **and** too late for its retry, so the reply the repeater sent to the first try turns up
  // while `get tx` is awaited. Without the prefix that text - the node's name - is stored as the
  // transmit power.
  mesh.deliverTheNextLineRepliesLate("get name", 1, /*waits=*/2);

  const std::string body = run(fake::Job(12390, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"tx\",\"repeat\"]}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"tx\":\"22\""), std::string::npos)
      << "the reply to `get tx` is the one `get tx` is read from: " << data;
  EXPECT_EQ(data.find("\"tx\":\"Grenaa Chimney #1\""), std::string::npos)
      << "the late reply to the line before was stored as this line's value: " << data;
  EXPECT_NE(data.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos)
      << "and the retry's own reply is the name: " << data;
  EXPECT_NE(data.find("\"repeat\":\"on\""), std::string::npos) << data;

  // One send per line still, and the retry of the line that was not answered in time.
  EXPECT_EQ(mesh.timesSent("get name"), 2) << "no reply once is retried once (EXIT-NODES §5)";
  EXPECT_EQ(mesh.timesSent("get tx"), 1);
  EXPECT_EQ(transactionsOf(body), 7)
      << "the login, two tries of `get name`, `get tx`, `get repeat`, `ver` and `board`";

  // The log of a read this long no longer holds its own first lines: `JobLog` is 1 024 bytes and
  // drops whole entries off the front when it fills, and since task p1.21-71 every answered
  // exchange says how its reply came home as well as how the request went. The entry this test
  // is about is the drop, which is near the end and survives; the retry's own *no reply* and the
  // one sentence about the prefixes are the earliest entries and are gone. The prefixes
  // themselves are read out of the fake below, and
  // `EveryConsoleLineOfAJobCarriesItsOwnPrefixAndNoTwoAreTheSame` holds that sentence on a
  // shorter job.
  const std::string log = logOf(body);
  EXPECT_NE(log.find("cli 'get tx' try 1: a console reply that does not carry this line's prefix arrived instead"),
            std::string::npos)
      << "the drop is in the job's log, with no byte of the dropped reply in it: " << log;
  EXPECT_NE(log.find("cli 'get name' try 2: ok"), std::string::npos) << log;
  // The prefix changes with every line and every try, so a late reply to try 1 is not the answer
  // to try 2 either - which is what made the drop above happen in `get tx`'s wait and not here.
  EXPECT_NE(mesh.prefixOf("get name", 0), mesh.prefixOf("get name", 1));
  EXPECT_NE(mesh.prefixOf("get name", 1), mesh.prefixOf("get tx"));
  // And no line's reply is read with a prefix still on it.
  EXPECT_EQ(data.find("|"), std::string::npos) << data;
}

TEST_F(RunnerTest, EveryConsoleLineOfAJobCarriesItsOwnPrefixAndNoTwoAreTheSame) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12391, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::vector<std::string> lines = mesh.lines();
  const std::vector<std::string> prefixes = mesh.linePrefixes();
  ASSERT_EQ(lines.size(), prefixes.size());
  ASSERT_GT(lines.size(), 15u) << "the default list is sixteen keys and the extras";

  std::set<std::string> seen;
  for (size_t i = 0; i < lines.size(); i++) {
    // `region` is the one line of this read that goes bare: its reply is
    // RegionMap::exportTo(reply, 160), the whole of the repeater's reply window, and the three
    // characters of a prefix are three the map has not got.
    if (lines[i] == "region") {
      EXPECT_EQ(prefixes[i], "") << "the region listing must not lose three characters of its window";
      continue;
    }
    EXPECT_EQ(prefixes[i].size(), mm::kCliPrefixLen) << lines[i];
    EXPECT_EQ(prefixes[i][2], '|') << lines[i];
    EXPECT_TRUE(seen.insert(prefixes[i]).second) << "two lines of one job share a prefix: " << lines[i];
  }
  // The read is exactly the read it was: the values, the regions and the restore.
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ConfigReadResult")));
  EXPECT_NE(data.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"regions\":\"* F\\n dk F\\ndefault dk\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"firmware\":\"v1.17.1 (Build: 27 Sep 2025)\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"aclRestored\":true"), std::string::npos) << data;
  EXPECT_EQ(transactionsOf(body), 21) << "the price did not move: a prefix is bytes of a line, not a transaction";
}

TEST_F(RunnerTest, ARepeaterThatDoesNotSayItReflectsThePrefixIsReadExactlyAsBefore) {
  // Nothing looser than the login's own FIRMWARE_VER_LEVEL tells the two apart. A repeater that
  // names a level below 1 - or none at all, which is the same branch - gets its lines as it got
  // them before this task, because a prefix a repeater does not strip is run as part of the
  // command: `rep-v6` of February 2025 answers `a7|get name` with
  // `Unknown: a7|get name (commands: ...)`.
  mesh.setFirmwareVerLevel(0);
  const std::string body = run(fake::Job(12392, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"tx\"]}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  for (const std::string& prefix : mesh.linePrefixes()) {
    EXPECT_EQ(prefix, "") << "a line went out with a prefix this repeater would not have stripped";
  }
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"name\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"tx\":\"22\""), std::string::npos) << data;
  EXPECT_EQ(transactionsOf(body), 5) << "the login, two gets, ver and board";
  EXPECT_NE(logOf(body).find("has not been told the repeater's firmware level, so its console lines carry no prefix"),
            std::string::npos)
      << logOf(body);
}

TEST_F(RunnerTest, AnApplysLinesCarryThePrefixAndTheRegionLoadersDoNot) {
  // The loader is the one sequence of this firmware whose lines must go bare, and for two
  // reasons: `handleCommand` returns from its region-load branch **before** it reads a prefix,
  // so a prefixed region line would be loaded as a region named `a7|...`; and a line nobody
  // awaits a reply for must not be given one, because the reflected prefix alone is a non-empty
  // reply and would put a packet on the air for every region in the map.
  const std::string payload =
      "{\"commands\":["
      "{\"key\":\"advert.interval\",\"command\":\"set advert.interval 180\"},"
      "{\"key\":\"regions\",\"command\":\"region load\",\"lines\":["
      "{\"line\":\"region load\",\"expect\":null,\"escape\":false},"
      "{\"line\":\" dk F\",\"expect\":null,\"escape\":false},"
      "{\"line\":\" \",\"expect\":\"OK - loaded\",\"escape\":true},"
      "{\"line\":\"region default dk\",\"expect\":\"default scope is now\"},"
      "{\"line\":\"region save\",\"expect\":\"OK\"}]}"
      "],\"planHash\":\"deadbeef\"}";
  const std::string body = run(fake::Job(12393, "config.apply")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 3600)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(payload));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::vector<std::string> lines = mesh.lines();
  const std::vector<std::string> prefixes = mesh.linePrefixes();
  ASSERT_EQ(lines.size(), prefixes.size());
  // The lines that must go bare: the loader's own three, and the `region` of the re-read.
  const std::set<std::string> bare = {"region load", " dk F", " ", "region"};
  int bareSeen = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    if (bare.count(lines[i]) > 0) {
      EXPECT_EQ(prefixes[i], "") << lines[i];
      bareSeen++;
    } else {
      EXPECT_EQ(prefixes[i].size(), mm::kCliPrefixLen) << lines[i];
    }
  }
  EXPECT_EQ(bareSeen, 4) << "the loader's three lines and the region listing of the re-read";
  // The load really loaded, which it could not have done through a prefix, and the apply is the
  // apply it was.
  EXPECT_EQ(mesh.regionsLoaded(), 1);
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ConfigApplyResult")));
  EXPECT_NE(data.find("OK - loaded 1 regions"), std::string::npos) << data;
  EXPECT_NE(data.find("default scope is now dk"), std::string::npos) << data;
  EXPECT_EQ(data.find("a7|"), std::string::npos) << data;
}

TEST_F(RunnerTest, ALateReplyDuringAnApplysReReadIsNotTheNextKeysValue) {
  // The apply's re-read is `get`s with the same bounded retry a config read's are (§5 step 5),
  // so it is the same swap: `get advert.interval` answered too late for both its tries, and the
  // reply turning up while `get repeat` is awaited. Without the prefix the re-read would carry
  // `repeat: 180`, and meshmaster.dk would fold that into the repeater's configuration.
  mesh.deliverTheNextLineRepliesLate("get advert.interval", 1, /*waits=*/2);
  const std::string payload =
      "{\"commands\":["
      "{\"key\":\"advert.interval\",\"command\":\"set advert.interval 180\"},"
      "{\"key\":\"repeat\",\"command\":\"set repeat off\",\"expect\":\"OK - repeat is now OFF\"}"
      "],\"planHash\":\"deadbeef\"}";
  const std::string body = run(fake::Job(12394, "config.apply")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 3600)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(payload));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  const std::string reRead = memberOf(dataOf(body), "reRead");
  EXPECT_NE(reRead.find("\"advert.interval\":\"180\""), std::string::npos) << reRead;
  EXPECT_NE(reRead.find("\"repeat\":\"off\""), std::string::npos)
      << "the key's own reply is what it is read from: " << reRead;
  EXPECT_EQ(reRead.find("\"repeat\":\"180\""), std::string::npos)
      << "the late reply to the key before was stored as this key's value: " << reRead;
  EXPECT_NE(logOf(body).find("a console reply that does not carry this line's prefix arrived instead"),
            std::string::npos)
      << logOf(body);
}

TEST_F(RunnerTest, TheTwoRoundsOfTheLineThatCannotBeAskedTwiceCarryDifferentPrefixes) {
  // Task p1.10-48's second round sends the **same text** again, and that is exactly where a
  // prefix per try earns its keep: a late "OK" from round one must not confirm round two of a
  // line whose first attempt may already have run.
  const std::string line = "setperm " + self() + " 1";
  mesh.loseTheNextLines(line, 1);

  const std::string body = run(fake::Job(12395, "acl.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"publicKey\":\"" + self() + "\",\"level\":1}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  ASSERT_EQ(mesh.timesSent(line), 2) << "two rounds (task p1.10-48)";
  EXPECT_EQ(mesh.prefixOf(line, 0).size(), mm::kCliPrefixLen);
  EXPECT_NE(mesh.prefixOf(line, 0), mesh.prefixOf(line, 1))
      << "the same text twice, and the second round answerable only by its own reply";
  EXPECT_NE(dataOf(body).find("\"reply\":\"OK\""), std::string::npos)
      << "the reply is recorded with its prefix taken off: " << dataOf(body);
}

TEST_F(RunnerTest, ThePasswordLineCarriesItsPrefixAndNeitherTheValueNorItsEchoIsLogged) {
  // A password never leaves differently: the line is sensitive in the frame log with a prefix on
  // it exactly as it was without one, the echo is compared and recorded redacted, and the buffer
  // the prefixed line was built in is wiped with the one the secret was built in.
  const std::string secret = "n3w-adm1n-pass";
  const std::string body = run(fake::Job(12396, "password.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withNewPassword(secret)
                                   .withPayload("{\"field\":\"adminPassword\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.timesSent("password " + secret), 1) << "sent once (§7), and recorded as the repeater ran it";
  EXPECT_EQ(mesh.prefixOf("password " + secret).size(), mm::kCliPrefixLen) << "the line carried a prefix";
  EXPECT_EQ(mesh.adminPassword(), secret) << "the repeater stored it";
  EXPECT_EQ(memberOf(dataOf(body), "reply"), "password now: <set>")
      << "the echo arrived behind the prefix, was compared and was recorded redacted: " << dataOf(body);

  // **The rail**, with the prefix in the way: neither the result nor one captured log line holds
  // the value.
  EXPECT_EQ(body.find(secret), std::string::npos) << "the result carries the new password";
  EXPECT_FALSE(fake::LogCapture::the().holds(secret)) << "a log line carries the new password";
  EXPECT_FALSE(fake::LogCapture::the().holds("adm1n-pass")) << "a log line carries the admin password";
}

// ---------------------------------------------------------------------------------------------
// The six that write

TEST_F(RunnerTest, AConfigApplySendsEachCommandOnceAndReadsBackWhatItTouched) {
  const std::string payload =
      "{\"commands\":["
      "{\"key\":\"advert.interval\",\"command\":\"set advert.interval 180\"},"
      "{\"key\":\"flood.max\",\"command\":\"set flood.max 32\"}"
      "],\"planHash\":\"deadbeef\"}";
  const std::string body = run(fake::Job(12370, "config.apply")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(payload));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 6) << "§7's n + m + 1 and a login: two sets, two gets, ver";
  EXPECT_EQ(mesh.timesSent("set advert.interval 180"), 1) << "a set sent twice is a change made twice";
  EXPECT_EQ(mesh.timesSent("set flood.max 32"), 1);
  EXPECT_EQ(mesh.timesSent("get advert.interval"), 1);
  EXPECT_EQ(mesh.timesSent("get flood.max"), 1);
  EXPECT_EQ(mesh.timesSent("ver"), 1);
  EXPECT_EQ(mesh.config().at("advert.interval"), "180") << "the repeater took the value";

  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ConfigApplyResult")));
  EXPECT_NE(data.find("\"allOk\":true"), std::string::npos) << data;
  EXPECT_NE(data.find("\"reRead\":{\"advert.interval\":\"180\",\"flood.max\":\"32\"}"), std::string::npos) << data;

  // The sender timestamps of an apply and its re-read are strictly increasing: equal is a retry
  // the firmware runs nothing for.
  const std::vector<uint32_t> stamps = mesh.cliTimestamps();
  ASSERT_GE(stamps.size(), 5u);
  for (size_t i = 1; i < stamps.size(); i++) EXPECT_GT(stamps[i], stamps[i - 1]) << "at " << i;
}

TEST_F(RunnerTest, AConfigApplyStopsAtTheFirstReplyThatIsNotWhatTheLineWanted) {
  mesh.answerLine("set flood.max 32", "ERROR: flood.max must be 1-64");
  const std::string payload =
      "{\"commands\":["
      "{\"key\":\"flood.max\",\"command\":\"set flood.max 32\"},"
      "{\"key\":\"tx\",\"command\":\"set tx 20\"}"
      "],\"planHash\":\"deadbeef\"}";
  const std::string body = run(fake::Job(12371, "config.apply")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(payload));

  EXPECT_EQ(outcomeOf(body), "failed");
  EXPECT_EQ(mesh.timesSent("set tx 20"), 0) << "nothing after the refusal is sent";
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"outcome\":\"error\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"outcome\":\"not-sent\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"allOk\":false"), std::string::npos) << data;
}

TEST_F(RunnerTest, ACliLineIsSentOnceAndNeverRetried) {
  mesh.silenceLine("set tx 20");  // the login is answered; the line's reply is eaten
  const std::string body = run(fake::Job(12372, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  // The line goes once and nothing comes back: `unreachable` with the command in the result, for
  // the server to settle against the next configuration read (§5a, §7's `cli` row).
  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.timesSent("set tx 20"), 1) << "sent once, never retried";
  EXPECT_NE(logOf(body).find("not sent again"), std::string::npos) << body;
  EXPECT_NE(dataOf(body).find("\"command\":\"set tx 20\""), std::string::npos) << dataOf(body);
}

TEST_F(RunnerTest, ACliLineTheRepeaterRefusesIsFailedAndRecorded) {
  const std::string body = run(fake::Job(12373, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"nonsense\"}"));

  EXPECT_EQ(outcomeOf(body), "failed");
  EXPECT_NE(dataOf(body).find("\"reply\":\"Unknown command\""), std::string::npos) << dataOf(body);
}

TEST_F(RunnerTest, AnAdvertRequestFloodsAndIsConfirmed) {
  const std::string body = run(fake::Job(12374, "advert.request")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_EQ(mesh.timesSent("advert"), 1);
  EXPECT_EQ(mesh.floodAdverts(), 1) << "`advert` is the flood that refreshes everyone's paths";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("AdvertRequestResult")));
  EXPECT_NE(data.find("\"reply\":\"OK - Advert sent\""), std::string::npos) << data;
}

TEST_F(RunnerTest, AZeroHopAdvertRequestAsksForTheOtherCommand) {
  const std::string body = run(fake::Job(12375, "advert.request")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"flood\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.timesSent("advert.zerohop"), 1);
  EXPECT_EQ(mesh.floodAdverts(), 0);
}

TEST_F(RunnerTest, AClockSyncSetsTheRepeatersClockFromThisBoards) {
  const std::string body = run(fake::Job(12376, "clock.sync")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_GE(mesh.repeaterClock(), kNow) << "the repeater's clock moved to this board's";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("ClockSyncResult")));
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
}

TEST_F(RunnerTest, AClockSyncTheRepeaterRefusesIsRecordedAsTheRefusalItIs) {
  mesh.setRepeaterClock(kNow + 3600);  // the repeater's clock is an hour ahead
  const std::string body = run(fake::Job(12377, "clock.sync")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass"));

  EXPECT_EQ(outcomeOf(body), "failed");
  EXPECT_NE(dataOf(body).find("clock cannot go backwards"), std::string::npos) << dataOf(body);
}

TEST_F(RunnerTest, APasswordSetSendsTheLineOnceAndRecordsNoValueAnywhere) {
  const std::string secret = "n3w-adm1n-pass";
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12378, "password.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withNewPassword(secret)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"field\":\"adminPassword\"}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.timesSent("password " + secret), 1) << "sent once (§7)";
  EXPECT_EQ(mesh.adminPassword(), secret) << "the repeater stored it";

  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("PasswordSetResult")));
  EXPECT_NE(data.find("\"confirmed\":true"), std::string::npos) << data;
  EXPECT_EQ(memberOf(data, "reply"), "password now: <set>")
      << "the echo is compared and recorded redacted: " << data;

  // **The rail.** Neither the result the server is handed nor one line of the captured log holds
  // the value, and the echo that carried it back is redacted before it is recorded.
  EXPECT_EQ(body.find(secret), std::string::npos) << "the result carries the new password";
  EXPECT_FALSE(fake::LogCapture::the().holds(secret)) << "a log line carries the new password";
  EXPECT_FALSE(fake::LogCapture::the().holds("adm1n-pass")) << "a log line carries the admin password";

  // The restore after the password line, and only then the session dropped (task p4.1-60).
  const std::vector<std::string> lines = mesh.lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "password " + secret);
  EXPECT_EQ(lines[1], "setperm " + self() + " 1");
  EXPECT_NE(logOf(body).find("the session is dropped"), std::string::npos) << body;
}

TEST_F(RunnerTest, APasswordSetLongerThanTheFirmwareKeepsIsNeverSent) {
  const std::string body = run(fake::Job(12379, "password.set")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withNewPassword("sixteen-chars-xx")
                                   .withPayload("{\"field\":\"adminPassword\"}"));

  EXPECT_EQ(outcomeOf(body), "failed");
  EXPECT_EQ(mesh.sendCount(), 0) << "a cut-off password is a lock-out: nothing goes on the air";
  EXPECT_NE(logOf(body).find("lock-out"), std::string::npos) << body;
}

TEST_F(RunnerTest, ARebootIsOkAsSentOnSilenceAndRestoresNothing) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12380, "reboot")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << "the firmware never answers a reboot: the silence is the answer";
  EXPECT_EQ(transactionsOf(body), 2) << "the login and the command (§7)";
  EXPECT_TRUE(mesh.repeaterRebooted());
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 0)
      << "a restore before the reboot would make the command unanswerable (task p4.1-60)";
  EXPECT_NE(logOf(body).find("no restore on this session"), std::string::npos) << body;
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("RebootResult")));
  EXPECT_NE(data.find("\"sent\":true"), std::string::npos) << data;
}

// ---------------------------------------------------------------------------------------------
// The rules of §5 and §7

TEST_F(RunnerTest, AStatusAndTelemetryPairForOneRepeaterCostsOneLogin) {
  mesh.grant(self(), mm::kPermReadOnly);
  server.queue({fake::Job(12390, "status")
                    .atClass("acl")
                    .leasedUntil(kLease)
                    .withAclLevel(mm::kPermReadOnly)
                    .withPayload("{\"version\":false}")
                    .groupedWith(12391)
                    .json(),
                fake::Job(12391, "telemetry")
                    .atClass("acl")
                    .leasedUntil(kLease)
                    .withAclLevel(mm::kPermReadOnly)
                    .groupedWith(12390)
                    .json()});

  EXPECT_EQ(runner.claimAndRun(), 2);
  EXPECT_EQ(mesh.logins(), 1) << "the session is reused inside its lifetime (EXIT-NODES §5)";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetStatus), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetTelemetry), 1);
  ASSERT_EQ(server.results().size(), 2u);
  EXPECT_EQ(outcomeOf(server.results()[0].body), "ok");
  EXPECT_EQ(outcomeOf(server.results()[1].body), "ok");
  EXPECT_NE(logOf(server.results()[0].body).find("grouped with job 12391"), std::string::npos);
  EXPECT_NE(logOf(server.results()[1].body).find("session reused"), std::string::npos);
}

TEST_F(RunnerTest, ALostReplyOnAReusedSessionIsRetriedOnceWithAFreshLogin) {
  mesh.grant(self(), mm::kPermReadOnly);
  // The first job opens the session.
  run(fake::Job(12392, "status")
          .atClass("acl")
          .leasedUntil(kLease)
          .withAclLevel(mm::kPermReadOnly)
          .withPayload("{\"version\":false}"));
  ASSERT_EQ(mesh.logins(), 1);
  mesh.forgetSends();

  // The second finds the session live, and the repeater has forgotten us: the request goes
  // unanswered once, and the retry logs in again first (EXIT-NODES §5).
  mesh.forgetTheSession();
  const std::string body = run(fake::Job(12393, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  const std::vector<fake::Send>& sends = mesh.sends();
  ASSERT_EQ(sends.size(), 3u);
  EXPECT_EQ(sends[0].kindName(), std::string("request")) << "the first try, on the reused session";
  EXPECT_FALSE(sends[0].answered);
  EXPECT_EQ(sends[1].kindName(), std::string("login")) << "no reply on a reused session: log in again once";
  EXPECT_EQ(sends[2].kindName(), std::string("request"));
  EXPECT_TRUE(sends[2].answered);
  EXPECT_NE(logOf(body).find("no reply on a reused session: logging in again once"), std::string::npos) << body;
}

TEST_F(RunnerTest, SilenceTwiceIsUnreachable) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.beSilentAlways();
  const std::string body =
      run(fake::Job(12394, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.logins(), 2) << "the two tries of the login sequence, and nothing after them";
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_EQ(dataOf(body), "") << "an unreachable carries no data";
}

// A response that is not a login reply is not an answer to the login (task p1.10-47). It used to
// be `unauthorized` at one login, with the job's own command never sent; it is now dropped, the
// wait goes on, and the second try - which the repeater answers - runs the job.
TEST_F(RunnerTest, AResponseThatIsNotALoginReplyIsDroppedAndTheSecondTryRunsTheJob) {
  mesh.answerTheNextLoginWithSomethingElse(1);
  const std::string body = run(fake::Job(12395, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << "never `unauthorized`: nothing refused anything";
  EXPECT_EQ(mesh.logins(), 2) << "the dropped reply did not end the wait, so the second try follows as for silence";
  EXPECT_EQ(mesh.cliLines(), 1) << "the line the `unauthorized` used to lose";
  EXPECT_NE(logOf(body).find("a reply that is not a login reply arrived instead"), std::string::npos) << body;
  EXPECT_EQ(logOf(body).find("refused"), std::string::npos) << body;
}

// And the same response with nothing behind it: two tries, `unreachable`, and no verdict about
// any password - which is what stock firmware's silence has always come to.
TEST_F(RunnerTest, ANonLoginResponseAndThenSilenceIsUnreachableAndNeverUnauthorized) {
  mesh.answerTheNextLoginWithSomethingElse(1);
  mesh.beSilentToTheNextLogins(1);
  const std::string body = run(fake::Job(12398, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.logins(), 2);
  EXPECT_EQ(mesh.cliLines(), 0);
  EXPECT_EQ(dataOf(body), "") << "an unreachable carries no data, and no verdict about the password";
  EXPECT_EQ(failedAtOf(body), "login") << body;
}

TEST_F(RunnerTest, ALoginAnsweredBelowTheClassIsUnauthorizedWithTheTierItGranted) {
  // A wrong admin password on a repeater with a guest password: the guest password matches, the
  // login is answered *guest*, and an admin job cannot run on it (task p3.5-41).
  mesh.setGuestPassword("the-guest-one");
  const std::string body = run(fake::Job(12396, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("the-guest-one")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unauthorized");
  EXPECT_EQ(mesh.cliLines(), 0) << "nothing is sent on a session below the class";
  const std::string data = dataOf(body);
  EXPECT_NE(data.find("\"loggedInAs\":\"guest\""), std::string::npos) << data;
  EXPECT_NE(data.find("\"needed\":\"admin\""), std::string::npos) << data;
  EXPECT_EQ(loggedInAsOf(body), "") << "the tier is the refusal's data; the envelope says nothing";
}

// ---------------------------------------------------------------------------------------------
// **And guest is enough to read with** (meshmaster.dk's task p4.30-180, EXIT-NODES §7)
//
// AAR_LH, 10 November 2026: a guest-door pair with somebody else's repeater was still being
// proved, the read fell through to class `acl` - a login by our own key, which that repeater held
// no entry for - the repeater answered the blank login *guest*, and jobs 25169 and 25177, both
// *Read the neighbours*, ended `unauthorized` with `{"needed": "read-only", "loggedInAs":
// "guest"}` without the request the firmware would have answered ever going out.
//
// So for the two types a guest login serves **whole** - `status` and `neighbours`, which
// `handleRequest` checks no permission at all for - an `acl` login answered below `read-only`
// keeps its session at the tier granted, runs, and reports `ok` with the data and the tier on the
// envelope's `loggedInAs`. The login is still the evidence it was: the server reads that field
// exactly as it reads the `unauthorized` carrying the same tier, and moves the pair to
// `needs-regrant` either way.

// The scenario of the two lost jobs, run: the repeater holds no entry for our key and has no
// guest password, so the blank login of class `acl` is answered *guest*.
TEST_F(RunnerTest, TheNeighbourReadAnAclLoginAnsweredAtGuestRunsAndSaysWhatItRanAs) {
  mesh.setGuestPassword("");
  mesh.setNeighbours({fake::NeighbourEntry{repeater(), 342u, 29}});
  const std::string body = run(fake::Job(25169, "neighbours").atClass("acl").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "ok") << "the reading is not lost with the login";
  EXPECT_EQ(loggedInAsOf(body), "guest") << "and the login is still the evidence it was";
  EXPECT_EQ(failedAtOf(body), "") << "nothing failed";
  EXPECT_EQ(transactionsOf(body), 2) << "the login and one page of nine";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 1) << "the request the firmware answers was sent";
  // The envelope is `JobResultReportAtGuest.json`'s shape - `loggedInAs` after the log - with the
  // clock the login showed after it (`JobResultReportWithClock.json`, meshmaster.dk's task
  // p4.37-210), and the data is the reading a `neighbours` job always answers.
  EXPECT_TRUE(followsBothFixtures(body)) << body;
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("NeighboursResultTotal"))) << data;
  EXPECT_NE(data.find("\"prefix\":\"" + repeater().substr(0, 16) + "\""), std::string::npos) << data;
  EXPECT_EQ(totalOf(data), 1) << "one neighbour, and the repeater says it has one: " << data;
  EXPECT_NE(logOf(body).find("logged in as guest, below the read-only this job asked for"), std::string::npos) << body;
  EXPECT_NE(logOf(body).find("the ACL entry of this exit node's key is gone or demoted"), std::string::npos) << body;
}

// The other of the two types, with the owner-info request beside it: a `status` read at `acl`
// answered at guest reads everything such a read reads, because `handleRequest` checks no
// permission for either request.
TEST_F(RunnerTest, TheStatusReadAnAclLoginAnsweredAtGuestRunsWholeAndSaysWhatItRanAs) {
  mesh.setGuestPassword("");
  const std::string body = run(fake::Job(25177, "status").atClass("acl").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(loggedInAsOf(body), "guest");
  EXPECT_EQ(transactionsOf(body), 3) << "the login, the status request and the owner-info request";
  EXPECT_TRUE(followsBothFixtures(body)) << body;
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("StatusResult"))) << data;
  EXPECT_NE(data.find("\"batteryMv\":4012"), std::string::npos) << data;
  EXPECT_NE(data.find("\"nodeName\":\"Grenaa Chimney #1\""), std::string::npos) << data;
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermGuest) << "the blank login wrote a guest entry for our key";
}

// `telemetry` is **not** one of the two, although a guest login is answered it: `perm_mask` is
// forced to `0x00` for a guest sender, so the reply is the base channels and none of the sensors,
// and a read that quietly dropped them would look like the sensors had gone. It ends exactly
// where it ended before - the refusal at the login, with the tier and the need in the data and
// nothing on the envelope - and nothing goes on the air after the login.
TEST_F(RunnerTest, ATelemetryReadAfterTheSameLoginIsStillUnauthorized) {
  mesh.setGuestPassword("");
  const std::string body = run(fake::Job(25178, "telemetry").atClass("acl").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "unauthorized");
  EXPECT_EQ(failedAtOf(body), "login");
  EXPECT_EQ(loggedInAsOf(body), "") << "the envelope's field is for a job that *ran*";
  EXPECT_EQ(dataOf(body), "{\"loggedInAs\":\"guest\",\"needed\":\"read-only\"}");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetTelemetry), 0) << "nothing is sent on a session below the class";
  EXPECT_EQ(transactionsOf(body), 1) << "the login, and no request after it";
}

// A `get` list is admin's - `onPeerDataRecv` drops a text message from anybody below admin - and
// this board refuses an `acl`-class `config.read` at the claim, before any login, which is what
// `canRun` has done since task p1.2-10 and what the server's `JobTypes.CredentialClasses` agrees
// with (`ConfigRead => [Admin]`). At **admin**, where this type is asked for, a login answered at
// guest is the below-class refusal it always was: a wrong admin password, not a missing entry.
TEST_F(RunnerTest, AConfigReadAfterTheSameLoginIsStillRefusedAtEitherClass) {
  mesh.setGuestPassword("");
  const std::string atAcl = run(fake::Job(25179, "config.read")
                                    .atClass("acl")
                                    .leasedUntil(kLease)
                                    .withPayload("{\"keys\":[\"name\"]}"));
  EXPECT_EQ(outcomeOf(atAcl), "unsupported") << "refused at the claim, before any login";
  EXPECT_EQ(transactionsOf(atAcl), 0);
  EXPECT_EQ(loggedInAsOf(atAcl), "");

  mesh.setGuestPassword("the-guest-one");
  const std::string atAdmin = run(fake::Job(25180, "config.read")
                                      .atClass("admin")
                                      .leasedUntil(kLease)
                                      .withAdminPassword("the-guest-one")
                                      .withPayload("{\"keys\":[\"name\"]}"));
  EXPECT_EQ(outcomeOf(atAdmin), "unauthorized");
  EXPECT_EQ(loggedInAsOf(atAdmin), "");
  EXPECT_EQ(dataOf(atAdmin), "{\"loggedInAs\":\"guest\",\"needed\":\"admin\"}");
  EXPECT_EQ(mesh.cliLines(), 0);
}

// A grouped claim - two jobs, one login - where the second is one that must still be refused.
// The guest session the first job ran on is **invisible** to the second (`SessionStore::get` hands
// out a session only when its role satisfies the class asked for), so the `telemetry` logs in
// afresh and is refused on its own merits. Two logins where a pair at the class it asked for
// costs one: the price of the reading being kept.
TEST_F(RunnerTest, AGroupedPairWhoseLoginWasAnsweredAtGuestRunsTheReadAndRefusesTheRest) {
  mesh.setGuestPassword("");
  mesh.setNeighbours({fake::NeighbourEntry{repeater(), 60u, 24}});
  server.queue({fake::Job(25181, "neighbours").atClass("acl").leasedUntil(kLease).groupedWith(25182).json(),
                fake::Job(25182, "telemetry").atClass("acl").leasedUntil(kLease).groupedWith(25181).json()});

  EXPECT_EQ(runner.claimAndRun(), 2);
  ASSERT_EQ(server.results().size(), 2u);
  const std::string read = server.results()[0].body;
  const std::string refused = server.results()[1].body;

  EXPECT_EQ(outcomeOf(read), "ok");
  EXPECT_EQ(loggedInAsOf(read), "guest");
  EXPECT_EQ(outcomeOf(refused), "unauthorized");
  EXPECT_EQ(loggedInAsOf(refused), "");
  EXPECT_EQ(dataOf(refused), "{\"loggedInAs\":\"guest\",\"needed\":\"read-only\"}");
  EXPECT_EQ(mesh.logins(), 2) << "the session kept at guest is invisible to the class that asked for more";
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetTelemetry), 0);
  EXPECT_EQ(logOf(refused).find("session reused"), std::string::npos) << refused;
}

// And the session is not held above its level for a *later* read either: a second `neighbours`
// job for the same repeater finds no session it may use, logs in again, and is answered guest
// again - which is the rule of task p3.5-41 unchanged, and the one thing this task must not
// loosen. What the guest session *is* good for is a guest-class job.
TEST_F(RunnerTest, TheSessionKeptAtGuestIsNeverHandedToAJobOfTheClassThatAskedForMore) {
  mesh.setGuestPassword("");
  mesh.setNeighbours({fake::NeighbourEntry{repeater(), 60u, 24}});
  const std::string first = run(fake::Job(25183, "neighbours").atClass("acl").leasedUntil(kLease));
  ASSERT_EQ(outcomeOf(first), "ok");
  ASSERT_EQ(mesh.logins(), 1);

  const std::string second = run(fake::Job(25184, "neighbours").atClass("acl").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(second), "ok");
  EXPECT_EQ(loggedInAsOf(second), "guest");
  EXPECT_EQ(mesh.logins(), 2) << "a fresh login, judged on its own merits";
  EXPECT_EQ(logOf(second).find("session reused"), std::string::npos) << second;

  // The same session serves a guest-class read, which is the class it can carry.
  const std::string atGuest = run(fake::Job(25185, "neighbours").atClass("guest").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(atGuest), "ok");
  EXPECT_EQ(loggedInAsOf(atGuest), "") << "a guest answer is not below the class a guest job asked for";
  EXPECT_EQ(mesh.logins(), 2) << "the guest session was reused";
  EXPECT_NE(logOf(atGuest).find("session reused"), std::string::npos) << atGuest;
}

TEST_F(RunnerTest, ASilenceRightAfterAnAnsweredBlankLoginIsReportedAsTheRefusalItIs) {
  // The rotation probe (EXIT-NODES §7, "One silence is reported as a refusal all the same"): the
  // `new` candidate is silent, the blank login between the tries is answered because our key is
  // in the ACL, and the second try is silent again.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12397, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("not-the-admin-one")
                                   .withCandidate("new")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unauthorized");
  EXPECT_EQ(transactionsOf(body), 3) << "two tries and the blank login between them";
  EXPECT_EQ(dataOf(body), "{\"reason\":\"silentAfterReachable\",\"probeAnsweredAs\":\"read-only\"}");
  EXPECT_NE(logOf(body).find("answered a blank login at read-only"), std::string::npos) << body;
}

// ---------------------------------------------------------------------------------------------
// Only a login reply answers a login (task p1.10-47; meshmaster.dk's task p4.32-186)
//
// A login reply carries the repeater's own clock and no tag of ours, so a login is awaited with
// tag 0 and - until this task - whatever the peer sent next was its answer. On a job whose
// request had just timed out that is the request's **late** reply, and it is how ten working ACL
// associations were parked on meshmaster.dk on 12 November 2026 as *the ACL entry is gone*. These
// walk the sequence of those job logs with the reply arriving where it arrived: during the
// re-login's wait.

TEST_F(RunnerTest, ALateRequestReplyDuringTheReLoginIsDroppedAndTheJobRunsOn) {
  mesh.grant(self(), mm::kPermReadOnly);
  // The first job opens the session, as the field's job 24142 had one 147 s old.
  run(fake::Job(24141, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  ASSERT_EQ(mesh.logins(), 1);
  mesh.forgetSends();

  // The second finds it live, and its request is answered **late**: past its own wait, during the
  // wait of the login that follows it.
  mesh.deliverTheNextReplyLate(1);
  const std::string body =
      run(fake::Job(24142, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(loggedInAsOf(body), "") << "the login was answered at the class the job asked for";
  EXPECT_EQ(mesh.logins(), 1) << "one re-login, answered";
  const std::vector<fake::Send>& sends = mesh.sends();
  ASSERT_EQ(sends.size(), 4u) << "the status request twice, the login between them, and the owner info after";
  EXPECT_EQ(sends[0].kindName(), std::string("request")) << "the first try, on the reused session";
  EXPECT_EQ(sends[1].kindName(), std::string("login"));
  EXPECT_EQ(sends[2].kindName(), std::string("request"));
  EXPECT_TRUE(sends[2].answered);
  EXPECT_EQ(sends[3].reqType, mm::kReqTypeGetOwnerInfo);
  const std::string log = logOf(body);
  EXPECT_NE(log.find("no reply on a reused session: logging in again once"), std::string::npos) << log;
  EXPECT_NE(log.find("the late reply to a request this job sent arrived instead"), std::string::npos) << log;
  EXPECT_NE(log.find("logged in as read-only"), std::string::npos) << log;
  EXPECT_EQ(log.find("refused"), std::string::npos) << "nothing refused anything: " << log;
}

TEST_F(RunnerTest, ALateRequestReplyAndNoLoginReplyAtAllIsWhatASilenceGives) {
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(21782, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  mesh.forgetSends();

  // The late reply arrives during the re-login's wait and is dropped; nothing answers either try.
  mesh.deliverTheNextReplyLate(1);
  mesh.beSilentToTheNextLogins(2);
  const std::string body =
      run(fake::Job(21783, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unreachable") << "what a silence gives, and never `unauthorized`: " << body;
  EXPECT_EQ(dataOf(body), "") << "no verdict about this exit node's ACL entry";
  EXPECT_EQ(failedAtOf(body), "login") << body;
  EXPECT_EQ(mesh.logins(), 2) << "the two tries of the login sequence, as for any silence";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("the late reply to a request this job sent arrived instead"), std::string::npos) << log;
  EXPECT_EQ(log.find("refused"), std::string::npos) << log;
}

// The reply a parse cannot catch: a neighbour page of a repeater with **no neighbours** is
// `total(i16)` and `returned(i16)`, four zero bytes, which `parseLoginReply` reads as *login ok,
// permissions 0* - and an `acl`-class `neighbours` whose login was answered at guest now runs at
// guest and says so (task p4.30-180). So without the tag this page would arrive as a true-looking
// *logged in as guest*, and the result would tell meshmaster.dk the ACL entry was gone.
TEST_F(RunnerTest, AnEmptyNeighbourPageArrivingLateIsDroppedByItsTagAndNotReadAsGuest) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setNeighbours({});
  run(fake::Job(19064, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  mesh.forgetSends();

  mesh.deliverTheNextReplyLate(1);
  const std::string body =
      run(fake::Job(19065, "neighbours").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(loggedInAsOf(body), "") << "the page is not a login reply, so no tier was ever granted by one";
  EXPECT_NE(logOf(body).find("the late reply to a request this job sent arrived instead"), std::string::npos) << body;
  EXPECT_NE(logOf(body).find("logged in as read-only"), std::string::npos) << body;
  EXPECT_EQ(logOf(body).find("logged in as guest"), std::string::npos) << body;
  EXPECT_EQ(dataOf(body), "{\"neighbours\":[],\"total\":0}") << "and the page itself is the reading: " << body;
}

// The rotation probe's blank login (task p1.3-13): a late reply reaching it must not make it
// *answered*, and least of all *refused* - the word is gone, and `probeAnsweredAs` could never be
// one again. Were it taken for an answer, the silence that follows would be reported to
// meshmaster.dk as the new candidate refused, and the row's fallback spent.
TEST_F(RunnerTest, TheReachabilityProbeIsNotAnsweredByALateReplyToSomethingElse) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.answerTheNextLoginWithSomethingElse(1);
  mesh.deliverTheNextReplyLate(1);   // and that response arrives during the probe's wait
  mesh.beSilentToTheNextLogins(2);   // the probe and the second try hear nothing
  const std::string body = run(fake::Job(12399, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("not-the-admin-one")
                                   .withCandidate("new")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unreachable") << "nothing was learned about the candidate: " << body;
  EXPECT_EQ(dataOf(body), "") << "no LoginSilentResult, because the probe was not answered";
  EXPECT_EQ(mesh.logins(), 3) << "the two tries and the blank login between them";
  EXPECT_EQ(transactionsOf(body), 3);
  const std::string log = logOf(body);
  EXPECT_NE(log.find("reachability probe (blank login): a reply that is not a login reply arrived instead"),
            std::string::npos)
      << log;
  EXPECT_EQ(log.find("answered a blank login"), std::string::npos) << log;
  EXPECT_EQ(log.find("refused"), std::string::npos) << log;
}

// The discover's logins (tasks p3.5-39 to 41): a response that is not a login reply used to be
// `Refused`, which the discover reports as a path learned and an admin job that would fail at the
// same login. Now it is dropped, the direct login is a silence, and the flooded one follows.
TEST_F(RunnerTest, TheDiscoversLoginIsNotRefusedByAResponseThatIsNotALoginReply) {
  mesh.setReturnPath({0x11, 0x22}, 2);
  mesh.answerTheNextLoginWithSomethingElse(1);
  const std::string body = run(fake::Job(12353, "discover")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .alongPath({"aabb"}));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.logins(), 2) << "the direct login was not answered, so the flooded one went out";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("DiscoverResult"))) << data;
  EXPECT_NE(data.find("\"loggedInAs\":\"admin\""), std::string::npos)
      << "the session the second login opened, not a refusal: " << data;
  EXPECT_NE(data.find("\"hops\":[\"1122\"]"), std::string::npos) << "the path the flooded reply taught: " << data;
  const std::string log = logOf(body);
  EXPECT_NE(log.find("a reply that is not a login reply arrived instead"), std::string::npos) << log;
  EXPECT_EQ(log.find("refused"), std::string::npos) << log;
}

// ---------------------------------------------------------------------------------------------
// `failedAt`: where a failed job failed (task p1.7-28; meshmaster.dk's task p4.17-136)
//
// `unreachable` is two different things - a login nobody answered, which on stock firmware is
// also a wrong password, and a request lost twice after a login that *was* answered - and the
// page that says *the stored guest password is probably wrong* must say it for the first alone.
// So every failed result names its step, and a result that asked the repeater for nothing names
// none: a trace logs in to nobody, and a job that never went on the air has no step to name.
// One test per credential class, which is what decides whether there is a login at all.

// `none`, the trace's class: it logs in to nobody, so there is no step to name however it ends.
TEST_F(RunnerTest, ATraceCarriesNoFailedAtHoweverItEnds) {
  const std::string ok = run(fake::Job(12420, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(ok), "ok");
  EXPECT_EQ(failedAtOf(ok), "") << ok;

  mesh.beSilentAlways();
  const std::string lost = run(fake::Job(12421, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(lost), "unreachable");
  EXPECT_EQ(mesh.logins(), 0) << "a trace asks nobody for a session (§7)";
  EXPECT_EQ(failedAtOf(lost), "") << "a job with no login has no step to name: " << lost;
}

// `guest`: the case the field exists for. Two silent logins, nothing after them - and the server
// may tell the owner that the stored guest password is the thing to look at.
TEST_F(RunnerTest, AGuestLoginNobodyAnsweredIsFailedAtTheLogin) {
  mesh.setGuestPassword("the-guest-one");
  mesh.beSilentAlways();
  const std::string body = run(fake::Job(12422, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("the-guest-one")
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.logins(), 2) << "the two tries of the login sequence";
  EXPECT_EQ(failedAtOf(body), "login") << body;
  // And it is the failed fixture's shape: the one field an `ok` result never carries, last.
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReportFailed"))) << body;
}

// `acl`, the class of this exit node's own key: a response to the login that is not a login reply
// is dropped (task p1.10-47), so the job fails at the login as a silence does - the field says
// where, not why, and the outcome is no longer the `unauthorized` this test asserted before.
TEST_F(RunnerTest, AnAclLoginAnsweredWithSomethingElseIsFailedAtTheLoginAsASilenceIs) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.answerTheNextLoginWithSomethingElse(1);
  mesh.beSilentToTheNextLogins(1);  // and nothing behind it, so the job ends at its login
  const std::string body =
      run(fake::Job(12423, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.logins(), 2) << "the drop did not end the wait, so there is a second try";
  EXPECT_EQ(failedAtOf(body), "login") << body;
}

// `admin`: the login was answered and the line after it was not. Nothing about any password.
TEST_F(RunnerTest, AnAdminRequestLostAfterAnAnsweredLoginIsFailedAtTheRequest) {
  mesh.silenceLine("set tx 20");  // the repeater is on the air and this line is never answered
  const std::string body = run(fake::Job(12424, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(mesh.logins(), 1) << "the login was answered";
  EXPECT_EQ(mesh.cliLines(), 1) << "a CLI line is sent once and never retried";
  EXPECT_EQ(failedAtOf(body), "request") << body;
}

// And the one that is both, in that order: a reused session, a request that got no answer, a
// login again that got none either. The job ended at a login, so that is what it names - which
// is why the stage is set at every attempt and not once at the start of the request.
TEST_F(RunnerTest, ARequestThatCouldNotLogInAgainIsFailedAtTheLoginAfterAll) {
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(12425, "status")
          .atClass("acl")
          .leasedUntil(kLease)
          .withAclLevel(mm::kPermReadOnly)
          .withPayload("{\"version\":false}"));
  ASSERT_EQ(mesh.logins(), 1);

  mesh.beSilentAlways();  // the repeater goes off the air under the second job
  const std::string body = run(fake::Job(12426, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_NE(logOf(body).find("no reply on a reused session: logging in again once"), std::string::npos) << body;
  EXPECT_EQ(failedAtOf(body), "login") << body;
}

// A job that never went on the air names no step, whatever its class: there was no login to go
// unanswered, and `failedAt: login` on it would point an owner at a password for a packet nobody
// sent. The lease that had already run out is the case with a class that is not `none`.
TEST_F(RunnerTest, AJobThatSentNothingCarriesNoFailedAt) {
  const std::string body =
      run(fake::Job(12427, "status").atClass("acl").leasedUntil(kNow - 1).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "timeout");
  EXPECT_EQ(mesh.sendCount(), 0);
  EXPECT_EQ(failedAtOf(body), "") << body;
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReport"))) << body;
}

// An `ok` result never carries it - the fixtures' own README says so, which is why
// JobResultReport.json is the ok result it always was and the failed one is a file of its own.
TEST_F(RunnerTest, AnOkResultCarriesNoFailedAt) {
  mesh.setGuestPassword("the-guest-one");
  const std::string body = run(fake::Job(12428, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("the-guest-one")
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(failedAtOf(body), "") << body;
  // The login this job made puts the clock pair on the envelope (meshmaster.dk's task p4.37-210),
  // which is `JobResultReportWithClock.json`'s shape.
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReportWithClock"))) << body;
}

// The two words are the server's `JobFailedAt`, read out of the fixture rather than typed here:
// a rename in meshmaster.dk moves that file and fails this.
TEST(FailedAt, TheTwoWordsAreTheOnesTheFixtureCarries) {
  const std::string failed = fake::contract("JobResultReportFailed");
  EXPECT_NE(failed.find(std::string("\"failedAt\":\"") + mm::kFailedAtLogin + "\""), std::string::npos) << failed;
  EXPECT_STREQ("login", mm::kFailedAtLogin);
  EXPECT_STREQ("request", mm::kFailedAtRequest);
  // And the ok fixture has neither word: the member is the one an ok result never carries.
  EXPECT_EQ(fake::contract("JobResultReport").find("failedAt"), std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The clock a login showed (meshmaster.dk's task p4.37-210; API §3, EXIT-NODES §7)
//
// A login reply begins with four bytes of the repeater's own clock - `MeshReply::tag`, which
// this firmware has always read and always thrown away. Since this task a job that made a login
// of its own and was answered reports that number and this board's own time at the same moment,
// as `repeaterClockS` and `agentClockS`, and meshmaster.dk stores the difference. **Two numbers
// and no arithmetic here.** The tests below are the one rule and its three absences, plus the
// one reply that must never be taken for a clock.

TEST_F(RunnerTest, AJobThatLoggedInReportsTheRepeatersClockAndThisBoardsOwn) {
  mesh.setGuestPassword("gu3st-pass");
  mesh.setRepeaterClock(kNow - 600);  // ten minutes behind this board
  const std::string body = run(fake::Job(25190, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.logins(), 1);
  // The reply's own four bytes, verbatim: `getCurrentTimeUnique` hands out the repeater's second
  // itself for the first reply of a second, and this is the first login of this job.
  EXPECT_EQ(repeaterClockOf(body), std::to_string(kNow - 600)) << body;
  // And this board's clock, read at the moment that reply came back: after the job began and not
  // after it ended.
  const uint32_t ours = static_cast<uint32_t>(std::atoll(agentClockOf(body).c_str()));
  EXPECT_GE(ours, kNow) << body;
  EXPECT_LE(ours, platform.clock().now()) << body;
  // The envelope's shape is the fixture's, with the pair last of all.
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReportWithClock"))) << body;
  // The difference is said on the **console**, for a bench, and nowhere else: nothing worked out
  // from the two numbers travels in the result, and the job's own log - 1 024 bytes, whole
  // entries falling off the front - is left for what the job did.
  EXPECT_TRUE(fake::LogCapture::the().holds("the repeater's clock says " + std::to_string(kNow - 600)));
  EXPECT_TRUE(fake::LogCapture::the().holds(" s behind; both go in the result"));
  EXPECT_EQ(logOf(body).find("the repeater's clock says"), std::string::npos)
      << "the job's log is not where this goes: " << logOf(body);
}

// Whatever the outcome, as `loggedInAs` is: a login that was answered said what time the
// repeater thought it was, and a request lost after it does not unsay it.
TEST_F(RunnerTest, ARequestLostAfterAnAnsweredLoginStillReportsTheClock) {
  mesh.setRepeaterClock(kNow + 3600);  // an hour ahead, which is the reading this exists to see
  mesh.silenceLine("set tx 20");
  const std::string body = run(fake::Job(25191, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(failedAtOf(body), "request") << body;
  EXPECT_EQ(repeaterClockOf(body), std::to_string(kNow + 3600)) << body;
  EXPECT_FALSE(agentClockOf(body).empty()) << "together or not at all: " << body;
}

// The discover's own login is a login of this job's, and its reply is the one the path is learned
// from - so a discover reports the clock like any other job (deliverable 3).
TEST_F(RunnerTest, TheDiscoversLoginReportsTheClockToo) {
  mesh.setGuestPassword("gu3st-pass");
  mesh.setRepeaterClock(kNow - 99);
  mesh.setReturnPath({0x11, 0x22, 0x33, 0x44}, 2);
  const std::string body = run(fake::Job(25192, "discover")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .withNoPath());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(repeaterClockOf(body), std::to_string(kNow - 99)) << body;
}

// **A job that made no login of its own reports none.** The second job finds the session the
// first opened, sends its request on it and never asks the repeater what time it is - so there
// is nothing to report, and the first job's reading is not repeated as though it were this
// job's.
TEST_F(RunnerTest, AJobThatReusedASessionReportsNoClock) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string first =
      run(fake::Job(25193, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  ASSERT_EQ(mesh.logins(), 1);
  ASSERT_FALSE(repeaterClockOf(first).empty()) << first;

  const std::string second =
      run(fake::Job(25194, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(second), "ok");
  EXPECT_EQ(mesh.logins(), 1) << "the session was reused, so no login went out";
  EXPECT_NE(logOf(second).find("session reused"), std::string::npos) << second;
  EXPECT_EQ(repeaterClockOf(second), "") << second;
  EXPECT_EQ(agentClockOf(second), "") << second;
  // And the shape of a result with neither member is still the fixture's.
  EXPECT_TRUE(fake::followsShapeOf(second, fake::contract("JobResultReportWithClock"))) << second;
}

// **A job through the local door reports none.** The door at this end answers the login itself
// (task p1.10-41), so the four bytes are no repeater's clock and the repeater behind the door was
// never asked what time it thinks it is.
TEST_F(RunnerTest, AJobThroughTheLocalDoorReportsNoClock) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setLocalDoorKey(repeater());
  const std::string body = run(fake::Job(25195, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 0) << "none of it was on the air";
  EXPECT_EQ(repeaterClockOf(body), "") << body;
  EXPECT_EQ(agentClockOf(body), "") << body;
  EXPECT_TRUE(fake::LogCapture::the().holds("the door at this end answered it"));
}

// **A board whose own clock is not set reports none.** One of the pair alone is a 400 on the
// result (API §3), and a board with no time of its own has no second number: the repeater's
// reading would be worth nothing without something to hold it against.
TEST_F(RunnerTest, ABoardWithNoClockOfItsOwnReportsNeitherNumber) {
  platform.fakeClock().unset();
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(25196, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.logins(), 1) << "the login went out and was answered";
  EXPECT_EQ(repeaterClockOf(body), "") << body;
  EXPECT_EQ(agentClockOf(body), "") << body;
  EXPECT_TRUE(fake::LogCapture::the().holds("this board's own clock is not set"));
}

// **The late reply task p1.10-47 drops is never taken for the clock.** The field's job 24142:
// a session 147 s old, a request answered past its own wait, and that answer arriving during the
// wait of the login that follows. A request's reply is stamped with the tag *this board* drew,
// so reading it as a clock would report this board's own number back as the repeater's - which
// would make a repeater that is ten minutes out look perfect.
TEST_F(RunnerTest, ALateRequestReplyIsNotTakenForTheRepeatersClock) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setRepeaterClock(kNow - 600);
  run(fake::Job(25197, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  ASSERT_EQ(mesh.logins(), 1);
  mesh.forgetSends();

  mesh.deliverTheNextReplyLate(1);
  const std::string body =
      run(fake::Job(25198, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_NE(logOf(body).find("the late reply to a request this job sent arrived instead"), std::string::npos) << body;
  // The re-login's own reply, which is the repeater's clock one second on from the first job's
  // reading (`getCurrentTimeUnique` never hands the same second out twice) - and never the tag of
  // the request that arrived late, which is a number of this board's own series above `kNow`.
  const uint32_t reported = static_cast<uint32_t>(std::atoll(repeaterClockOf(body).c_str()));
  EXPECT_GE(reported, kNow - 600);
  EXPECT_LT(reported, kNow - 500) << "a reply of this board's own series was read as the clock: " << body;
}

TEST_F(RunnerTest, AnUnknownTypeIsUnsupportedAtNoTransaction) {
  const std::string body = run(fake::Job(12398, "walter.sleep").atClass("none").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "unsupported");
  EXPECT_EQ(transactionsOf(body), 0);
  EXPECT_EQ(mesh.sendCount(), 0);
  EXPECT_NE(logOf(body).find("no handler for job type 'walter.sleep' in this firmware"), std::string::npos);
}

TEST_F(RunnerTest, TheCredentialBytesAreZeroAfterTheJob) {
  const std::string body = run(fake::Job(12399, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withNewPassword("n3w-adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));
  EXPECT_EQ(outcomeOf(body), "ok");

  // The claim's own buffer, as the runner handed it to the server: read back after the job.
  ASSERT_NE(server.claimed(), nullptr);
  ASSERT_GE(server.claimedCount(), 1);
  for (int i = 0; i < mm::kClaimMax; i++) {
    EXPECT_TRUE(server.claimed()[i].credential.isWiped())
        << "slot " << i << " still holds credential bytes after the job";
  }
}

TEST_F(RunnerTest, OneSendPerCommandTheMinimumGapAndThePerMinuteCapAreHeldByTheClock) {
  // The conservative default of EXIT-NODES §8: two transactions a minute, twenty seconds apart.
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  const std::string body = run(fake::Job(12400, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"name\",\"tx\",\"repeat\"]}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 6) << "the login, three gets, ver and board";
  // One send per command, whatever the pace.
  EXPECT_EQ(mesh.timesSent("get name"), 1);
  EXPECT_EQ(mesh.timesSent("get tx"), 1);
  EXPECT_EQ(mesh.timesSent("get repeat"), 1);
  // The gap and the cap, measured on the fake clock's record and not promised.
  for (uint32_t gap : mesh.gapsMs()) {
    EXPECT_GE(gap, mm::kDefaultMinGapSeconds * 1000u) << "two transactions closer than the minimum gap";
  }
  EXPECT_LE(mesh.mostInAnyMinute(), mm::kDefaultMaxTransactionsPerMinute)
      << "more transactions in a minute than the budget allows";
  EXPECT_GE(platform.millis(), 5u * mm::kDefaultMinGapSeconds * 1000u) << "the waits really happened";
  EXPECT_NE(logOf(body).find("waiting 20 s for the airtime budget"), std::string::npos) << body;
}

// ---- the local door (task p1.10-40) ----------------------------------------------------------
//
// A mesh that answers for the repeater **without the air** (IMesh::answersLocally) - the
// repeater-v2 unit's Walter, whose own radio is on a UART - neither waits for the airtime budget
// nor spends it. The three tests below are the pacing, the same job on the air for comparison,
// and the slot accounting between them.

TEST_F(RunnerTest, A34LineReadThroughTheLocalDoorWaitsForNoBudgetAndSpendsNone) {
  // The conservative default of EXIT-NODES §8, which is what the unit's own budget was on
  // 7 November 2026: two transactions a minute, twenty seconds apart.
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  mesh.setAnswersLocally(true);
  // A local job logs in with **no material at any class** (task p1.10-41), so the admin session
  // this read needs comes from the empty login: the real door answers that at admin by itself,
  // and this fake is a stock repeater, which answers it at the level its ACL holds this board's
  // key at. The claim's password is still here, and still goes nowhere.
  mesh.grant(self(), mm::kPermAdmin);
  const std::string body = run(fake::Job(12404, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(thirtyKeyPayload()));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.loginsWithMaterial(), 0) << "the door is answered by the board: nothing was sent to log in with";
  EXPECT_EQ(mesh.sendCount(), kThirtyKeyRead)
      << "the login, twenty-eight gets, the two region commands, ver, board and the access list";
  // A transaction is an exchange with a repeater **over the mesh** (EXIT-NODES §7), and none of
  // these was one, so the figure meshmaster.dk prices the air by counts none of them.
  EXPECT_EQ(transactionsOf(body), 0);
  EXPECT_EQ(runner.gate().transmissions(), 0u) << "and the gate was never told a slot was taken";
  EXPECT_EQ(runner.gate().totalWaitMs(), 0u) << "nor asked for one";
  EXPECT_EQ(logOf(body).find("waiting"), std::string::npos) << body;
  // (The job's own *the air is not used* line is the first entry of a log of thirty-four, which
  // a kilobyte does not hold - task p1.10-38 drops whole entries from the front when it fills -
  // so it is asserted on the short local job of the third test instead.)
  // No idle longer than a reply wait: the longest gap between two sends is the fake's flooded
  // reply latency, which is well under both the reply wait (perHopMs x 4, 16 s at the claim's
  // defaults) and the budget's twenty-second gap. On the Walter it was a 40 s idle here that
  // outlived the 30 s task watchdog.
  for (uint32_t gap : mesh.gapsMs()) {
    EXPECT_LT(gap, 3000u) << "a gap this long is a wait for something, and nothing here waits";
  }
  // The whole read, against the sixteen-odd minutes the next test measures for the same job.
  EXPECT_LT(platform.millis(), 60u * 1000u);
}

TEST_F(RunnerTest, TheSame34LineReadOnAMeshThatIsOnTheAirStillWaitsAsItDid) {
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  const std::string body = run(fake::Job(12405, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload(thirtyKeyPayload()));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.sendCount(), kThirtyKeyRead);
  EXPECT_EQ(transactionsOf(body), kThirtyKeyRead);
  EXPECT_EQ(runner.gate().transmissions(), static_cast<uint32_t>(kThirtyKeyRead));
  for (uint32_t gap : mesh.gapsMs()) {
    EXPECT_GE(gap, mm::kDefaultMinGapSeconds * 1000u) << "two transactions closer than the minimum gap";
  }
  EXPECT_LE(mesh.mostInAnyMinute(), mm::kDefaultMaxTransactionsPerMinute);
  // Two a minute for thirty-four of them: sixteen minutes of waiting, which is what the budget
  // is for when the air is really being used.
  EXPECT_GE(platform.millis(), 16u * 60u * 1000u);
  EXPECT_NE(logOf(body).find("waiting 20 s for the airtime budget"), std::string::npos) << body;
}

TEST_F(RunnerTest, ALocalJobLeavesTheNextJobOnTheAirNoFreeSlot) {
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  mesh.grant(self(), mm::kPermReadOnly);

  // One job on the air first: a login and a status request, which fills the minute.
  run(fake::Job(12406, "status")
          .atClass("acl")
          .leasedUntil(kNow + 1800)
          .withAclLevel(mm::kPermReadOnly)
          .withPayload("{\"version\":false}"));
  ASSERT_EQ(runner.gate().transmissions(), 2u);
  const uint32_t airSpentAt = platform.millis();
  // What that job waited for its own second slot: the gate's totals are cumulative, and the
  // local job's share of them must be nothing.
  const uint32_t waitedForTheAir = runner.gate().totalWaitMs();
  ASSERT_GT(waitedForTheAir, 0u);
  mesh.forgetSends();

  // Then a local one. It reuses the session, so on the air it would have been one transaction;
  // through the local door it is none, it waits for nothing, and the gate hears nothing of it.
  mesh.setAnswersLocally(true);
  const std::string local = run(fake::Job(12407, "status")
                                    .atClass("acl")
                                    .leasedUntil(kNow + 1800)
                                    .withAclLevel(mm::kPermReadOnly)
                                    .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(local), "ok");
  EXPECT_EQ(transactionsOf(local), 0);
  EXPECT_EQ(runner.gate().transmissions(), 2u) << "a local job takes no slot from the job after it";
  EXPECT_EQ(runner.gate().totalWaitMs(), waitedForTheAir) << "and asks the gate for none of its own";
  EXPECT_LT(platform.millis() - airSpentAt, 10u * 1000u) << "so nothing of it was a wait";
  EXPECT_NE(logOf(local).find("this mesh answers for this repeater without the air"), std::string::npos) << local;
  mesh.forgetSends();

  // And it gave the next job on the air nothing either: the window still holds the two starts of
  // the first job, so this one waits until the oldest of them is a minute old - which is where
  // it would have waited with no local job in between at all.
  mesh.setAnswersLocally(false);
  const std::string body = run(fake::Job(12408, "status")
                                   .atClass("acl")
                                   .leasedUntil(kNow + 1800)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));
  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 1) << "the session is still live; the request is the transaction";
  EXPECT_EQ(runner.gate().transmissions(), 3u);
  ASSERT_FALSE(mesh.sends().empty());
  EXPECT_GE(mesh.sends().front().atMs, 60u * 1000u)
      << "the third transaction of the minute went out before the first was a minute old";
  EXPECT_NE(logOf(body).find("s for the airtime budget"), std::string::npos) << body;
}

// The door is keyed to **one** repeater on the real mesh (`LinkMesh::setOwnKey`), and both of the
// library's questions about it come off that one key (task p1.10-43): `answersLocallyFor(key)`,
// which is what a wake prices a claimed job by, and `answersLocally()`, which is what `spend`
// paces the job by once it has begun. Here they are made to disagree if they can: the same mesh,
// the same job, and the door moved from another repeater's key to this one's. This is the host's
// half of bench item 6 of report p1.10-40 - a door left open for a key that is not its own.
TEST_F(RunnerTest, TheDoorIsKeyedToARepeaterSoAJobForAnotherIsStillOnTheAir) {
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setLocalDoorKey(fake::strangerKey());

  const std::string air = run(fake::Job(12420, "status")
                                  .atClass("acl")
                                  .leasedUntil(kNow + 1800)
                                  .withAclLevel(mm::kPermReadOnly)
                                  .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(air), "ok");
  EXPECT_EQ(transactionsOf(air), 2) << "a door for somebody else's repeater is no door for this job";
  EXPECT_EQ(runner.gate().transmissions(), 2u);
  EXPECT_EQ(logOf(air).find("without the air"), std::string::npos) << air;
  const uint32_t waitedForTheAir = runner.gate().totalWaitMs();
  mesh.forgetSends();

  // The door moved to this repeater's key. Nothing else about the mesh, the budget or the job
  // changes, and the same exchange is off the air: no transaction, no slot and no wait.
  mesh.setLocalDoorKey(repeater());
  const std::string local = run(fake::Job(12421, "status")
                                    .atClass("acl")
                                    .leasedUntil(kNow + 1800)
                                    .withAclLevel(mm::kPermReadOnly)
                                    .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(local), "ok");
  EXPECT_EQ(transactionsOf(local), 0);
  EXPECT_EQ(runner.gate().transmissions(), 2u) << "a local job takes no slot";
  EXPECT_EQ(runner.gate().totalWaitMs(), waitedForTheAir) << "and asks for none";
  EXPECT_NE(logOf(local).find("this mesh answers for this repeater without the air"), std::string::npos) << local;
}

// ---- no password for a local login (task p1.10-41) -------------------------------------------
//
// The same door, at its login: a job it answers needs **no material at any class**, because the
// board at the other end answers the login itself and at admin. What this is for is job 22016 of
// 7 November 2026 - a scheduled `config.read` of a unit's **own** repeater, refused by
// meshmaster.dk with *no admin password stored*, for a radio whose password is a random one
// nobody knows.

TEST_F(RunnerTest, AnAdminJobThroughTheLocalDoorRunsToItsEndWithNoPasswordAnywhere) {
  mesh.setAnswersLocally(true);
  // The real door answers any login at admin; this fake is a stock repeater and answers the
  // **empty** login at the level its ACL holds this board's key at, which is the same answer by
  // the stock route (FakeMesh::setAnswersLocally).
  mesh.grant(self(), mm::kPermAdmin);

  // The claim meshmaster.dk hands a unit for its own repeater: `admin`, and no credential at all.
  const std::string body = run(fake::Job(12409, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.logins(), 1) << "the login went, and it was the empty one";
  EXPECT_EQ(mesh.loginsWithMaterial(), 0);
  ASSERT_EQ(mesh.lines().size(), 1u);
  EXPECT_EQ(mesh.lines()[0], "set tx 20") << "the line the job is for, once";
  EXPECT_EQ(transactionsOf(body), 0) << "none of it was on the air (task p1.10-40)";
  EXPECT_NE(logOf(body).find("answered by the board, no password needed: the empty material at class admin"),
            std::string::npos)
      << body;
  EXPECT_EQ(logOf(body).find("nothing to log in with"), std::string::npos)
      << "that is the sentence this task ended for a local job: " << body;
  EXPECT_TRUE(fake::followsShapeOf(dataOf(body), fake::contract("CliResult")));
}

TEST_F(RunnerTest, TheSameAdminJobOnTheAirWithNoPasswordIsStillUnauthorizedWithNothingSent) {
  // Not local, and nothing changes: there is no admin login without an admin password, and the
  // job is refused before anything is sent.
  mesh.grant(self(), mm::kPermAdmin);
  const std::string body = run(fake::Job(12410, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unauthorized") << body;
  EXPECT_EQ(mesh.sendCount(), 0) << "nothing at all went out";
  EXPECT_EQ(transactionsOf(body), 0);
  EXPECT_NE(logOf(body).find("the claim carried no admin material; nothing to log in with"), std::string::npos)
      << body;
}

TEST_F(RunnerTest, ALocalJobSendsNoPasswordTheClaimDidCarryEither) {
  mesh.setAnswersLocally(true);
  mesh.grant(self(), mm::kPermAdmin);

  // A password this repeater does not know - an older server's, or one typed for another job. If
  // it went out, the fake would answer it with the silence stock firmware answers a wrong
  // password with, and the job would come back `unreachable` after two tries. It comes back `ok`,
  // which is the proof that nothing of it was sent.
  const std::string body = run(fake::Job(12411, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 1800)
                                   .withAdminPassword("not-the-one")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "ok") << body;
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(mesh.loginsWithMaterial(), 0) << "the claim's password stayed where it was";
  EXPECT_EQ(body.find("not-the-one"), std::string::npos) << "and is nowhere in the result: " << body;
}

TEST_F(RunnerTest, ALocalJobThatCarriedNoLevelRestoresNothing) {
  mesh.setAnswersLocally(true);
  mesh.grant(self(), mm::kPermAdmin);

  // The restore (task p4.1-60) is gated on the **level the claim carried**, and a unit's own
  // repeater is handed none (meshmaster.dk's task p4.27-173): nothing is put back on a repeater
  // the unit *is*, and no `setperm` goes over its console.
  const std::string first = run(fake::Job(12412, "cli")
                                    .atClass("admin")
                                    .leasedUntil(kNow + 1800)
                                    .withPayload("{\"command\":\"set tx 20\"}"));
  EXPECT_EQ(outcomeOf(first), "ok") << first;
  EXPECT_EQ(mesh.cliLines(), 1) << "the job's own line and no restore after it";
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 3"), 0);
  EXPECT_EQ(logOf(first).find("the ACL entry"), std::string::npos) << first;
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermAdmin) << "the ACL is as it was";
  mesh.forgetSends();

  // And the gate really is the claim's level and not the door: a local job that *does* carry one
  // is restored exactly as an over-the-air one is, because the level the server granted is the
  // level the server expects to find.
  const std::string second = run(fake::Job(12413, "cli")
                                     .atClass("admin")
                                     .leasedUntil(kNow + 1800)
                                     .withAclLevel(mm::kPermReadOnly)
                                     .withPayload("{\"command\":\"set tx 20\"}"));
  EXPECT_EQ(outcomeOf(second), "ok") << second;
  EXPECT_EQ(mesh.timesSent("setperm " + self() + " 1"), 1) << "the session's last command, as task 60 wrote it";
  EXPECT_EQ(mesh.levelOf(self()), mm::kPermReadOnly);
}

TEST_F(RunnerTest, TheLeaseRunningOutMidJobRefusesTheNextTransaction) {
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  const std::string body = run(fake::Job(12401, "config.read")
                                   .atClass("admin")
                                   .leasedUntil(kNow + 25)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"keys\":[\"regions\",\"name\"]}"));

  EXPECT_EQ(outcomeOf(body), "timeout");
  EXPECT_EQ(transactionsOf(body), 2) << "the login and one command; the next slot was past the lease";
  EXPECT_EQ(mesh.sendCount(), 2);
  EXPECT_EQ(mesh.timesSent("get name"), 0) << "nothing goes on the air for a lease somebody else may hold";
  EXPECT_NE(logOf(body).find("the budget's next slot is past the lease"), std::string::npos) << body;
}

TEST_F(RunnerTest, ALeaseThatHadAlreadyRunOutMeansNothingIsSentAtAll) {
  const std::string body =
      run(fake::Job(12402, "status").atClass("acl").leasedUntil(kNow - 1).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "timeout");
  EXPECT_EQ(transactionsOf(body), 0);
  EXPECT_EQ(mesh.sendCount(), 0);
  EXPECT_NE(logOf(body).find("the lease ran out before this job started"), std::string::npos) << body;
}

TEST_F(RunnerTest, AResultTheServerWouldNotTakeIsKeptAndPostedWithItsOwnTimestamps) {
  mesh.grant(self(), mm::kPermReadOnly);
  server.failResults(fake::FakeUplink::Fault::Unreachable, 1);  // the server goes away under the job
  server.queue(fake::Job(12403, "status")
                   .atClass("acl")
                   .leasedUntil(kLease)
                   .withAclLevel(mm::kPermReadOnly)
                   .withPayload("{\"version\":false}")
                   .json());
  ASSERT_EQ(runner.claimAndRun(), 1);
  EXPECT_TRUE(server.results().empty()) << "the server would not take it";
  EXPECT_EQ(runner.queueDepth(), 1) << "the result waited";
  ASSERT_NE(runner.newest(), nullptr);
  EXPECT_FALSE(runner.newest()->posted);
  const uint32_t ranAt = platform.clock().now();

  // An hour later, with the server back.
  platform.advanceSeconds(3600);
  runner.flushResults();
  EXPECT_EQ(runner.queueDepth(), 0);
  ASSERT_EQ(server.results().size(), 1u);
  const std::string body = server.results()[0].body;
  EXPECT_EQ(outcomeOf(body), "ok");
  uint32_t finishedAt = 0;
  ASSERT_TRUE(mm::parseUtc(memberOf(body, "finishedAt").c_str(), finishedAt)) << body;
  EXPECT_LE(finishedAt, ranAt) << "a late result carries the timestamps it was made with (EXIT-NODES §5)";
  EXPECT_GE(platform.clock().now() - finishedAt, 3600u) << "and it went an hour after it was made";
  EXPECT_NE(memberOf(body, "clientId").find("12403-"), std::string::npos)
      << "the client id it was made with, which is what the server de-duplicates on";
  EXPECT_TRUE(runner.newest()->posted);
}

// ---------------------------------------------------------------------------------------------
// The extension (agent/IJobExtension.h)

TEST_F(RunnerTest, AnExtensionsOwnTypeIsClaimedRunAndReported) {
  FakeExtension extension;
  runner.setExtension(&extension);
  mesh.grant(self(), mm::kPermReadOnly);

  const std::string body = run(fake::Job(12410, "walter.sleep").atClass("none").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "ok");
  ASSERT_EQ(extension.ran.size(), 1u);
  EXPECT_EQ(extension.ran[0], "walter.sleep");
  EXPECT_EQ(extension.waited, 4000u * 3u) << "the reply wait is the claim's timeouts and the hop count";
  EXPECT_TRUE(extension.answered) << "the extension reached the repeater through IMesh";
  EXPECT_EQ(dataOf(body), "{\"slept\":true,\"heardTheRepeater\":true}") << "its data, in the result envelope";
  EXPECT_EQ(transactionsOf(body), 1) << "the runner still counts the pacing";
  EXPECT_NE(logOf(body).find("the extension ran this type"), std::string::npos) << body;
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReport")));
}

TEST_F(RunnerTest, ATypeNobodyHandlesIsStillUnsupportedWithAnExtensionSet) {
  FakeExtension extension;
  runner.setExtension(&extension);

  const std::string body = run(fake::Job(12411, "walter.unknown").atClass("none").leasedUntil(kLease));

  EXPECT_EQ(outcomeOf(body), "unsupported");
  ASSERT_EQ(extension.ran.size(), 1u) << "it was offered the job and declined it";
  EXPECT_EQ(dataOf(body), "");
  EXPECT_NE(logOf(body).find("no handler for job type 'walter.unknown' in this firmware or its extension"),
            std::string::npos)
      << body;
}

// One of the thirteen the mesh cannot carry (IMesh::supports, task p1.6-23) is `unsupported` the
// same way a type nobody handles is: at no transaction, with nothing sent, the job never begun
// on the mesh. The trace is the one that would otherwise go (the test above sends it along a1,
// 7f); the status read is the one that would otherwise log in first - neither does.
TEST_F(RunnerTest, AJobOfATypeTheMeshCannotCarryIsUnsupportedAtNoTransactionWithNothingSent) {
  mesh.cannotServe("trace");
  mesh.cannotServe("status");
  mesh.grant(self(), mm::kPermReadOnly);

  const std::string trace = run(fake::Job(12423, "trace").atClass("none").leasedUntil(kLease));
  EXPECT_EQ(outcomeOf(trace), "unsupported") << trace;
  EXPECT_EQ(transactionsOf(trace), 0);
  EXPECT_EQ(dataOf(trace), "");
  EXPECT_NE(logOf(trace).find("this mesh cannot carry a 'trace' job, so it is not advertised and nothing was sent"),
            std::string::npos)
      << trace;
  EXPECT_EQ(mesh.sendCount(), 0) << "no trace packet";

  const std::string status = run(fake::Job(12424, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(status), "unsupported") << status;
  EXPECT_EQ(transactionsOf(status), 0);
  EXPECT_EQ(mesh.sendCount(), 0) << "no login either: the job was never begun on the mesh";
  EXPECT_EQ(mesh.logins(), 0);

  // And the one the mesh still carries runs as before, on the same runner.
  const std::string telemetry = run(fake::Job(12425, "telemetry").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));
  EXPECT_EQ(outcomeOf(telemetry), "ok") << telemetry;
  EXPECT_EQ(mesh.logins(), 1);
}

// ---------------------------------------------------------------------------------------------
// The two calls the protocol has and the reference agent never makes (API §3)

TEST_F(RunnerTest, ExtendLeaseAndPostProgressReachTheServerWithTheFixturesBytes) {
  uint32_t leaseUntil = 0;
  ASSERT_TRUE(server.extendLease(12345, 120, leaseUntil));
  ASSERT_EQ(server.extends().size(), 1u);
  EXPECT_EQ(server.extends()[0].first, 12345);
  EXPECT_EQ(server.extends()[0].second, fake::contract("ExtendRequest"));
  uint32_t expected = 0;
  ASSERT_TRUE(mm::parseUtc("2026-09-18T10:04:03Z", expected));
  EXPECT_EQ(leaseUntil, expected) << "the new end of the lease, as ExtendReply.json carries it";

  char body[512];
  const size_t len = mm::writeProgressReport("{\"neighbours\":[{\"prefix\":\"a1\",\"lastHeardS\":42,\"snr\":7.25}]}",
                                             "step 3 of 12", body, sizeof body);
  ASSERT_GT(len, 0u);
  ASSERT_TRUE(server.postProgress(12345, body, len));
  ASSERT_EQ(server.progress().size(), 1u);
  EXPECT_EQ(server.progress()[0].second, fake::contract("ProgressReport"));
}

TEST_F(RunnerTest, A409OnAnExtendIsNotTheLeaseHolderAndIsFalse) {
  server.failWith(fake::FakeUplink::Fault::Discard, 1);
  uint32_t leaseUntil = 7;
  EXPECT_FALSE(server.extendLease(12345, 120, leaseUntil));
  EXPECT_EQ(leaseUntil, 7u) << "nothing was learned about the lease";
  EXPECT_EQ(server.extends().size(), 1u) << "the body still went out";
}

// ---------------------------------------------------------------------------------------------
// The console's reports, which the runner also owns

TEST_F(RunnerTest, TheConsoleReportsReadBackWhatTheRunnerDid) {
  class Out : public mm::TextOut {
   public:
    void write(const char* text, size_t len) override { out.append(text, len); }
    std::string out;
  };
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(12420, "status")
          .atClass("acl")
          .leasedUntil(kLease)
          .withAclLevel(mm::kPermReadOnly)
          .withPayload("{\"version\":false}"));

  Out jobs;
  runner.report(jobs);
  EXPECT_NE(jobs.out.find("12420"), std::string::npos) << jobs.out;
  EXPECT_NE(jobs.out.find("status"), std::string::npos);
  EXPECT_NE(jobs.out.find("ok"), std::string::npos);

  Out sessions;
  runner.reportSessions(sessions);
  EXPECT_NE(sessions.out.find(repeater().substr(0, 12)), std::string::npos) << sessions.out;
  EXPECT_NE(sessions.out.find("read-only"), std::string::npos);
  EXPECT_EQ(sessions.out.find("adm1n-pass"), std::string::npos) << "no secret is in a session";

  char line[160];
  runner.describe(line, sizeof line);
  EXPECT_NE(std::string(line).find("1 run"), std::string::npos) << line;
}

// ---------------------------------------------------------------------------------------------
// The way back (task p1.21-71)
//
// On 22 November 2026 Thomas traced TwinOak Ebeltoft through the Risskov exit node: out by two
// hops and back, 7 to 12 dB at all six receptions (meshmaster.dk's job 40632). Two minutes later
// a status read along the same pinned path had two logins unanswered (40633). A trace goes out
// and comes back along the hops it names; a reply did not, because a repeater answers a direct
// request along the `out_path` it has stored for that client, stores one only from a PATH packet
// the client sends it, and this firmware sent none. So every reply to an exit node was a flood.
//
// `FakeMesh` tells one now, because the board does, and the repeater behind it does what the
// pinned firmware does with the packet: stores it under the key that sent it, drops it from a key
// its access list does not hold, forgets it when a login of that key arrives as a flood, and
// answers along it when it holds one.

// The frames of a job, in order, with the ways back among them - `mesh.frames()` as a sentence,
// so a failure prints what went on the air rather than an index.
std::string framesOf(const fake::FakeMesh& mesh) {
  std::string out;
  for (const std::string& frame : mesh.frames()) {
    if (!out.empty()) out += " ";
    out += frame;
  }
  return out;
}

TEST_F(RunnerTest, TwentyTwoNovemberTheRepliesComeBackTheWayTheRequestsWent) {
  // The read that failed: two hops from the server, and a repeater whose access list already
  // holds this exit node's key - the pinned pair that had been read hourly for weeks.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12600, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  // Way back, login, the status request, the owner-info request.
  EXPECT_EQ(framesOf(mesh), "wayback login request request") << logOf(body);
  EXPECT_EQ(mesh.waysBackCount(), 1) << "one telling is enough for a repeater that holds the key";
  EXPECT_EQ(transactionsOf(body), 3) << "the login and the two requests are what they always were (EXIT-NODES 7)";
  // Every reply came home along the hops the request went out by, and the log says so at each.
  const std::string log = logOf(body);
  EXPECT_EQ(log.find("reply flooded"), std::string::npos) << log;
  EXPECT_NE(log.find("way back: 2 hop(s)"), std::string::npos) << log;
  EXPECT_NE(log.find("logged in as read-only, firmware level 2 (direct, reply direct)"), std::string::npos) << log;
  int direct = 0;
  for (size_t at = 0; (at = log.find("reply direct", at)) != std::string::npos; at += 12) direct++;
  EXPECT_EQ(direct, 3) << "the login's reply and both requests' (" << log << ")";
}

TEST_F(RunnerTest, TheSameReadOnAMeshThatTellsNoWayBackHasEveryReplyFlooded) {
  // The same job before this task, and on a mesh that overrides nothing still: the repeater holds
  // no way home, so it floods every reply. Nothing else about the job moves.
  mesh.tellsNoWayBack();
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12601, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(framesOf(mesh), "login request request");
  EXPECT_EQ(mesh.waysBackCount(), 0);
  EXPECT_EQ(transactionsOf(body), 3);
  const std::string log = logOf(body);
  EXPECT_EQ(log.find("reply direct"), std::string::npos) << log;
  EXPECT_NE(log.find("this mesh tells no way back"), std::string::npos) << log;
  int flooded = 0;
  for (size_t at = 0; (at = log.find("reply flooded", at)) != std::string::npos; at += 13) flooded++;
  EXPECT_EQ(flooded, 3) << log;
}

TEST_F(RunnerTest, AMeshWithTheDefaultMemberSendsEveryByteAsItDid) {
  // Nothing of a job changes on a mesh that tells no way back: the same frames, the same
  // transactions, the same gaps, the same result - which is the promise the default member makes
  // to the Walter and to a host's own mesh.
  mesh.tellsNoWayBack();
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12602, "neighbours")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.waysBackCount(), 0);
  EXPECT_EQ(framesOf(mesh), "login request");
  EXPECT_EQ(transactionsOf(body), 2);
  EXPECT_NE(dataOf(body).find("\"prefix\":\"7f8e9dacbbcad9e8\""), std::string::npos) << dataOf(body);
  for (const fake::Send& sent : mesh.sends()) EXPECT_TRUE(sent.answered) << sent.kindName();
}

TEST_F(RunnerTest, AGuestsFirstLoginEndsDirectAndTellsExactlyTwoWaysBack) {
  // A guest's very first login: the repeater's access list does not hold this key yet, so the
  // first way back matches no peer and is dropped before it is decrypted, and the login - which
  // is what writes the entry - is answered with a flood. The way back goes once more, and the
  // request after it comes home direct.
  mesh.setGuestPassword("gu3st-pass");
  const std::string body = run(fake::Job(12603, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(framesOf(mesh), "wayback login wayback request request") << logOf(body);
  ASSERT_EQ(mesh.waysBackCount(), 2);
  EXPECT_FALSE(mesh.waysBack()[0].stored) << "a way back from a key the access list does not hold is dropped";
  EXPECT_TRUE(mesh.waysBack()[1].stored);
  EXPECT_EQ(transactionsOf(body), 3) << "the login and the two requests, as they always were";
  const std::string log = logOf(body);
  EXPECT_NE(log.find("(direct, reply flooded)"), std::string::npos) << "the login's own reply was flooded: " << log;
  EXPECT_NE(log.find("way back again (the reply came flooded)"), std::string::npos) << log;
  EXPECT_NE(log.find("reply direct"), std::string::npos) << log;

  // And the job after it is all direct: the repeater holds the way home now.
  mesh.forgetSends();
  const std::string next = run(fake::Job(12604, "telemetry")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass")
                                   .alongPath({"ef15", "f77b"}));
  EXPECT_EQ(outcomeOf(next), "ok");
  EXPECT_EQ(framesOf(mesh), "request") << "the session is still live and the repeater holds the way home: "
                                       << logOf(next);
  EXPECT_EQ(mesh.waysBackCount(), 0);
  EXPECT_EQ(logOf(next).find("reply flooded"), std::string::npos) << logOf(next);
  EXPECT_NE(logOf(next).find("reply direct"), std::string::npos) << logOf(next);
}

TEST_F(RunnerTest, APairOfNoHopsTellsTheEmptyPathAsItsWayBack) {
  // A direct neighbour: a known path of **zero** hops, whose way back is the empty path - which
  // `Packet::copyPath` stores as an `out_path_len` of 0 and not OUT_PATH_UNKNOWN, so the repeater
  // answers that client direct with no hops at all.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12605, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .asDirectNeighbour());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(framesOf(mesh), "wayback login request request");
  ASSERT_EQ(mesh.waysBackCount(), 1);
  EXPECT_TRUE(mesh.waysBack()[0].path.empty()) << "the empty path, which a repeater stores as direct";
  EXPECT_TRUE(mesh.waysBack()[0].stored);
  EXPECT_NE(logOf(body).find("way back: 0 hop(s), 0 ms to clear"), std::string::npos) << logOf(body);
  EXPECT_EQ(logOf(body).find("reply flooded"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, AFourHopPathInThreeByteHashesIsReversedAtThatWidth) {
  // The order is the thing: a direct packet is routed by the front of its path and each hop takes
  // itself off, while a flood appends at the end - so the way out read backwards is the way home
  // read forwards. Four hops of three-byte hashes, which is a width a trace cannot even express.
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12606, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"a1a2a3", "b1b2b3", "c1c2c3", "d1d2d3"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  ASSERT_EQ(mesh.waysBackCount(), 1);
  const fake::WayBackTold& told = mesh.waysBack()[0];
  EXPECT_EQ(told.hashSize, 3);
  const std::vector<uint8_t> reversed = {0xd1, 0xd2, 0xd3, 0xc1, 0xc2, 0xc3, 0xb1, 0xb2, 0xb3, 0xa1, 0xa2, 0xa3};
  EXPECT_EQ(told.path, reversed) << "the claim's hops, in the opposite order, at their own width";
  EXPECT_NE(logOf(body).find("way back: 4 hop(s)"), std::string::npos) << logOf(body);
  EXPECT_EQ(logOf(body).find("reply flooded"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, AReusedSessionWhoseClaimNamesAnotherPathTellsTheWayBackAgain) {
  // A session remembers the path it was opened along, because that is what the repeater was told
  // the way back from. The server has moved this pair onto another route; a reply along the old
  // one would come home by hops this job is not using.
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(12607, "status")
          .atClass("acl")
          .leasedUntil(kLease)
          .withAclLevel(mm::kPermReadOnly)
          .alongPath({"ef15", "f77b"}));
  mesh.forgetSends();

  const std::string body = run(fake::Job(12608, "telemetry")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "aaaa"}));
  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_NE(logOf(body).find("session reused"), std::string::npos) << logOf(body);
  EXPECT_EQ(framesOf(mesh), "wayback request") << logOf(body);
  EXPECT_EQ(mesh.waysBackCount(), 1);
  EXPECT_NE(logOf(body).find("way back again (the claim's path changed)"), std::string::npos) << logOf(body);
  EXPECT_EQ(transactionsOf(body), 1) << "a reused session is one transaction, as it always was";
}

TEST_F(RunnerTest, AReusedSessionAlongTheSamePathTellsNoWayBack) {
  mesh.grant(self(), mm::kPermReadOnly);
  run(fake::Job(12609, "status")
          .atClass("acl")
          .leasedUntil(kLease)
          .withAclLevel(mm::kPermReadOnly)
          .alongPath({"ef15", "f77b"}));
  mesh.forgetSends();

  const std::string body = run(fake::Job(12610, "telemetry")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));
  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_NE(logOf(body).find("session reused"), std::string::npos) << logOf(body);
  EXPECT_EQ(framesOf(mesh), "request") << "the repeater already holds this way back: " << logOf(body);
  EXPECT_EQ(mesh.waysBackCount(), 0);
  EXPECT_NE(logOf(body).find("reply direct"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, AFloodedLoginIsPrecededByNothingAndTheTaughtPathIsToldBeforeTheRequest) {
  // No path from the server: the login floods, and nothing precedes it - there are no hops to
  // name. Its reply comes back as a path return and teaches one, so the way back goes along that
  // path before the job's next send, and that reply comes home direct.
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.setReturnPath({0x11, 0x22, 0x33, 0x44}, 2);
  const std::string body = run(fake::Job(12611, "telemetry")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withNoPath());

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(framesOf(mesh), "login wayback request") << logOf(body);
  ASSERT_EQ(mesh.waysBackCount(), 1);
  const std::vector<uint8_t> reversed = {0x33, 0x44, 0x11, 0x22};
  EXPECT_EQ(mesh.waysBack()[0].path, reversed) << "the path the flood taught, reversed";
  EXPECT_NE(logOf(body).find("way back along the path a flood taught"), std::string::npos) << logOf(body);
  EXPECT_NE(logOf(body).find("reply direct"), std::string::npos) << logOf(body);
  EXPECT_EQ(transactionsOf(body), 2) << "the flooded login and the request, as they always were";
}

TEST_F(RunnerTest, AWayBackLostOnTheAirIsToldOnceMoreAndThenTheRepliesAreDirect) {
  // The packet itself lost: the repeater holds no way home, so the login's reply is flooded as it
  // was before this task, the way back goes once more, and the request after it is direct.
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.loseTheNextWaysBack(1);
  const std::string body = run(fake::Job(12612, "telemetry")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(framesOf(mesh), "wayback login wayback request") << logOf(body);
  ASSERT_EQ(mesh.waysBackCount(), 2);
  EXPECT_FALSE(mesh.waysBack()[0].reached);
  EXPECT_TRUE(mesh.waysBack()[1].stored);
  EXPECT_NE(logOf(body).find("(direct, reply flooded)"), std::string::npos) << logOf(body);
  EXPECT_NE(logOf(body).find("way back again (the reply came flooded)"), std::string::npos) << logOf(body);
  EXPECT_NE(logOf(body).find("reply direct"), std::string::npos) << logOf(body);
  EXPECT_EQ(transactionsOf(body), 2);
}

TEST_F(RunnerTest, ADeadWayBackEndsTheJobAsSilenceDoesAndNothingNewIsTried) {
  // **What gets less forgiving.** A reply has one way home now: a link that is good out and poor
  // back fails where a flood might have got through. The way back arrives and the repeater stores
  // it; every reply it sends along it is lost. Both tries of the login go unanswered, the job is
  // `unreachable` exactly as a silence makes it, and nothing new is sent - a way back is not a
  // command and is told once a login, not once a disappointment.
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.theWayBackIsDead();
  const std::string body = run(fake::Job(12613, "telemetry")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "unreachable");
  EXPECT_EQ(failedAtOf(body), "login");
  EXPECT_EQ(framesOf(mesh), "wayback login wayback login") << logOf(body);
  EXPECT_EQ(mesh.logins(), 2) << "the two tries of the login sequence and nothing else";
  EXPECT_EQ(mesh.requests(), 0);
  EXPECT_EQ(transactionsOf(body), 2) << "one a try, as a silence has always cost";
}

TEST_F(RunnerTest, TheWayBackAndTheLoginAreOneSlotOfTheGateAndOneTransaction) {
  // Two packets, one slot: the way back goes at the start of the login's own slot, the hops are
  // given the time this mesh says they need, and the login follows inside the same slot. The
  // budget's gap is measured from where the transaction started, which is the way back.
  setBudget(mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds);
  mesh.setWayBackClearMs(500);  // 500 ms a hop, so a two-hop path is a second
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12614, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 3);
  EXPECT_EQ(runner.gate().transmissions(), 3u) << "the way back took no slot of its own";
  ASSERT_EQ(mesh.waysBackCount(), 1);
  ASSERT_GE(mesh.sendCount(), 1);
  EXPECT_EQ(mesh.sends()[0].atMs - mesh.waysBack()[0].atMs, 1000u)
      << "the login waited for the hops to clear: two hops at 500 ms";
  for (uint32_t gap : mesh.gapsMs()) {
    EXPECT_GE(gap, mm::kDefaultMinGapSeconds * 1000u) << "two transactions closer than the minimum gap";
  }
  EXPECT_NE(logOf(body).find("1000 ms to clear"), std::string::npos) << logOf(body);
}

TEST_F(RunnerTest, ALocalJobTellsNoWayBack) {
  // A job the mesh answers without the air has no hops to clear and no reply to route
  // (IMesh::answersLocally, task p1.10-40).
  mesh.setLocalDoorKey(repeater());
  const std::string body = run(fake::Job(12615, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .alongPath({"ef15", "f77b"}));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.waysBackCount(), 0);
  EXPECT_EQ(transactionsOf(body), 0);
}

// ---------------------------------------------------------------------------------------------
// The result's log, one entry per line (task p1.10-38)
//
// `JobLog` joins its entries with a `'\n'`, as MeshMaster.Agent's `JobLog.Append` does, because
// *The agent's log* on a job's page keeps line breaks and several entries carry a `"; "` of
// their own - so a `"; "` between entries was a paragraph the page could not split back up.
// `logOf` unescapes, so a `'\n'` here is the `\n` the envelope carried.

// The entry the drop test fills the buffer with: 41 characters, so a kilobyte holds twenty-four
// of them and the twenty-fifth is what makes the buffer drop. The format string is written out
// at both of its two uses rather than held in a variable, so that it stays a literal to
// `JobLog::add`'s printf attribute.
std::string filler(int i) {
  char text[64];
  std::snprintf(text, sizeof text, "entry %03d of a job that said a great deal", i);
  return text;
}

TEST(JobLog, ThreeEntriesAreThreeLines) {
  mm::JobLog log;
  log.add("a");
  log.add("b");
  log.add("c");
  EXPECT_EQ(std::string(log.text()), "a\nb\nc");

  // And the envelope carries them as `a\nb\nc`: JsonWriter escapes the byte, so the server
  // parses three lines out of one JSON string (JsonWriter.h's `escape`).
  mm::JobReport r;
  r.clientId = "1-a";
  r.outcome = mm::Outcome::Ok;
  r.log = log.text();
  char out[256];
  const size_t len = mm::writeJobResult(r, out, sizeof out);
  ASSERT_GT(len, 0u);
  EXPECT_NE(std::string(out, len).find("\"log\":\"a\\nb\\nc\""), std::string::npos) << std::string(out, len);
}

TEST(JobLog, AnEmptyLogIsNoLineAndOneEntryIsNoLineBreak) {
  mm::JobLog log;
  EXPECT_STREQ(log.text(), "");
  log.add("only this");
  EXPECT_STREQ(log.text(), "only this") << "one entry carries no separator at all";
  log.clear();
  EXPECT_STREQ(log.text(), "");
}

TEST(JobLog, FillingTheBufferDropsWholeEntriesSoTheFirstLineIsAWholeEntry) {
  mm::JobLog log;
  for (int i = 0; i < 200; i++) log.add("entry %03d of a job that said a great deal", i);
  const std::string text = log.text();
  ASSERT_LT(text.size(), 1024u) << "the kilobyte buffer still bounds it";

  // The oldest line left is an entry from its first character to its last, not the tail of one
  // cut at byte 512.
  const std::string first = text.substr(0, text.find('\n'));
  int which = -1;
  ASSERT_EQ(std::sscanf(first.c_str(), "entry %d of a job that said a great deal", &which), 1)
      << "the log starts mid-entry: " << first;
  ASSERT_GT(which, 0) << "nothing was dropped, so this test checked nothing";
  EXPECT_EQ(first, filler(which));

  // The newest entry is whole too, and the one before the first line is gone.
  EXPECT_NE(text.find(filler(199)), std::string::npos) << text;
  EXPECT_EQ(text.find(filler(which - 1)), std::string::npos) << text;
}

TEST(JobLog, AnEntryLongerThanHalfTheBufferIsCutButStillBounded) {
  mm::JobLog log;
  const std::string huge(5000, 'x');
  for (int i = 0; i < 5; i++) log.add("%s", huge.c_str());
  EXPECT_LT(std::strlen(log.text()), 1024u) << "there is no line break to cut at, so the half goes as it always did";
  EXPECT_GT(std::strlen(log.text()), 0u);

  // A short entry before one that long puts the only line break well before the half, so there
  // is none at or after it: the half goes, and the short entry with it.
  mm::JobLog mixed;
  mixed.add("the first entry, short");
  mixed.add("%s", huge.c_str());
  mixed.add("%s", huge.c_str());
  EXPECT_EQ(std::string(mixed.text()).find("the first entry, short"), std::string::npos) << mixed.text();
}

TEST_F(RunnerTest, TheLogOfARealJobIsOneEntryPerLine) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12420, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));
  const std::string log = logOf(body);
  ASSERT_NE(log.find('\n'), std::string::npos) << "the entries are joined by a line break: " << body;
  EXPECT_EQ(log.substr(0, log.find('\n')).rfind("job 12420 status for repeater", 0), 0u)
      << "the first line is the whole of the first entry: " << log;
  EXPECT_NE(body.find("\\n"), std::string::npos) << "and the envelope escaped it: " << body;
}

}  // namespace

// PlatformIO's googletest integration links no gtest_main, so every test file brings its own.
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
