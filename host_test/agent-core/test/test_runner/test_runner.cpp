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

TEST_F(RunnerTest, AStatusReadAtGuestIsOneTransactionAndOneLogin) {
  mesh.setGuestPassword("gu3st-pass");
  const std::string body = run(fake::Job(12345, "status")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2) << "one login and the status request (EXIT-NODES §7)";
  EXPECT_EQ(mesh.logins(), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetStatus), 1);
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetOwnerInfo), 0) << "the version is read at class acl alone";

  // The envelope and the data, against the shapes ContractsJson writes.
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReport")));
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("StatusResult")));
  // The values are status-response.hex's, which test/test_reply_codec asserts for the same bytes.
  EXPECT_NE(data.find("\"batteryMv\":4012"), std::string::npos) << data;
  EXPECT_NE(data.find("\"uptimeS\":864000"), std::string::npos) << data;
  EXPECT_NE(data.find("\"lastSnr\":6.25"), std::string::npos) << data;
  EXPECT_EQ(data.find("\"firmware\""), std::string::npos) << "no owner-info request was made";
  EXPECT_EQ(memberOf(body, "pathUsed"), "[\"a1\",\"7f\"]") << "the path the claim carried, reported back";
  EXPECT_EQ(memberOf(body, "hops"), "2");
}

TEST_F(RunnerTest, AStatusReadAtAclAlsoFetchesTheFirmwareVersion) {
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

TEST_F(RunnerTest, AStatusReadTheClaimSaysNeedsNoVersionSpendsNothingOnIt) {
  mesh.grant(self(), mm::kPermReadOnly);
  const std::string body = run(fake::Job(12345, "status")
                                   .atClass("acl")
                                   .leasedUntil(kLease)
                                   .withAclLevel(mm::kPermReadOnly)
                                   .withPayload("{\"version\":false}"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(transactionsOf(body), 2);
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
  std::vector<fake::NeighbourEntry> list;
  for (int i = 0; i < 12; i++) {
    char prefix[17];
    std::snprintf(prefix, sizeof prefix, "%016d", i);
    list.push_back(fake::NeighbourEntry{std::string(prefix) + std::string(48, '1'), 60u * (i + 1), 24});
  }
  mesh.setNeighbours(list);

  const std::string body = run(fake::Job(12347, "neighbours")
                                   .atClass("guest")
                                   .leasedUntil(kLease)
                                   .withGuestPassword("gu3st-pass"));

  EXPECT_EQ(outcomeOf(body), "ok");
  EXPECT_EQ(mesh.requestsOf(mm::kReqTypeGetNeighbours), 2) << "nine a page, so twelve is two pages (§7)";
  EXPECT_EQ(transactionsOf(body), 3) << "the login and two pages";
  const std::string data = dataOf(body);
  EXPECT_TRUE(fake::followsShapeOf(data, fake::contract("NeighboursResult")));
  int reported = 0;
  size_t at = 0;
  while ((at = data.find("\"prefix\":", at)) != std::string::npos) {
    reported++;
    at += 9;
  }
  EXPECT_EQ(reported, 12);
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

TEST_F(RunnerTest, AnAclSetOnOurOwnKeyIsTheSessionsLevelToo) {
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

TEST_F(RunnerTest, ARefusedLoginIsUnauthorized) {
  mesh.refuseTheNextLogin(1);
  const std::string body = run(fake::Job(12395, "cli")
                                   .atClass("admin")
                                   .leasedUntil(kLease)
                                   .withAdminPassword("adm1n-pass")
                                   .withPayload("{\"command\":\"set tx 20\"}"));

  EXPECT_EQ(outcomeOf(body), "unauthorized");
  EXPECT_EQ(mesh.logins(), 1) << "a refusal is an answer: the sequence stops at it";
  EXPECT_EQ(mesh.cliLines(), 0);
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

// `acl`, the class of this exit node's own key: a login the repeater *refused* is an answer, and
// it is still a failure at the login - the field says where, not why.
TEST_F(RunnerTest, AnAclLoginTheRepeaterRefusedIsFailedAtTheLogin) {
  mesh.grant(self(), mm::kPermReadOnly);
  mesh.refuseTheNextLogin(1);
  const std::string body =
      run(fake::Job(12423, "status").atClass("acl").leasedUntil(kLease).withAclLevel(mm::kPermReadOnly));

  EXPECT_EQ(outcomeOf(body), "unauthorized");
  EXPECT_EQ(mesh.logins(), 1) << "a refusal is an answer: the sequence stops at it";
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
  EXPECT_TRUE(fake::followsShapeOf(body, fake::contract("JobResultReport"))) << body;
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
