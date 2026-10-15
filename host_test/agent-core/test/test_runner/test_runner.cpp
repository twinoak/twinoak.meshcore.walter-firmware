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

  std::string outcomeOf(const std::string& body) const { return memberOf(body, "outcome"); }
  std::string dataOf(const std::string& body) const { return memberOf(body, "data"); }
  std::string logOf(const std::string& body) const { return memberOf(body, "log"); }
  int transactionsOf(const std::string& body) const { return std::atoi(memberOf(body, "transactions").c_str()); }

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

}  // namespace

// PlatformIO's googletest integration links no gtest_main, so every test file brings its own.
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
