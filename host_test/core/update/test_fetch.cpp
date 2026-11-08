// The fetch of a release (task p3-02) against the specification's fixture release served by the
// scripted server: accepted in a test-key trust table; refused at check 1 with the build's own
// (empty) trust table; the tampered manifest refused; the unknown-key signature refused - all
// before a single byte of the image is requested. Then the file's half: a wrong hash refused at
// check 4 with the slot left empty; a resume across a simulated restart; the budget stopping a
// download and the next wake finishing it.
#include <doctest.h>

#include <memory>
#include <string>

#include "core/image/store_bench.h"
#include "core/image/this_image.h"
#include "core/image/trust.h"
#include "core/net/trust.h"
#include "core/update/fetch.h"
#include "core/version.h"
#include "fakes/capture.h"
#include "fixtures/manifest_files.h"
#include "core/net/net_bench.h"

using bench::Candidate;
using bench::has;
using bench::NetBench;
using bench::StoreBench;
using core::board::Slot;
using core::image::Check;
using core::image::Which;
using core::update::Fetch;
using core::update::FetchResult;
using core::update::FetchVerdict;

namespace {

// The fixture release on the server: manifest.json, manifest.json.sig and the image under the
// name the manifest gives it.
void serveRelease(sim::ScriptedServer& server, const std::string& base, const Candidate& image,
                  const std::vector<std::uint8_t>* manifest = nullptr, const std::string* sig = nullptr) {
  server.serve(base + "/manifest.json", manifest != nullptr ? *manifest : image.manifest);
  const std::string s = sig != nullptr ? *sig : image.signature;
  server.serve(base + "/manifest.json.sig", std::vector<std::uint8_t>(s.begin(), s.end()));
}

// The file's name in the manifest: the swd entry's name, read out of the manifest text.
std::string swdName(const Candidate& image) {
  const std::string text(image.manifest.begin(), image.manifest.end());
  const std::size_t use = text.find("\"use\": \"swd\"");
  REQUIRE(use != std::string::npos);
  const std::size_t name = text.find("\"name\": \"", use);
  REQUIRE(name != std::string::npos);
  const std::size_t start = name + 9;
  return text.substr(start, text.find('"', start) - start);
}

struct FetchRig {
  StoreBench store;   // the store over a simulated flash, the test key's trust table, one verdict
  NetBench net;       // the scripted server, the firmware's HTTP over the fake socket, the client, the download
  fakes::CaptureOut out;
  std::unique_ptr<Fetch> fetch;

  FetchRig() { build(); }
  void build() {
    fetch.reset(new Fetch(net.client, net.download, store.store, store.verdict, net.clock, &out));
  }
  // A restart of the Walter: the store reads its flash again, a new fetch over it.
  void reboot() {
    store.reboot();
    build();
  }
  int imageRequests(const std::string& name) const {
    int n = 0;
    for (const sim::SeenRequest& r : net.http.server.requests) {
      if (r.path.find(name) != std::string::npos) ++n;
    }
    return n;
  }
};

}  // namespace

TEST_CASE("the fixture release is fetched into the candidate slot through all six checks, each printed as it passes") {
  FetchRig rig;
  const Candidate image("store-a");
  serveRelease(rig.net.server(), "/release", image);
  rig.net.server().serve("/release/" + swdName(image), image.file, "\"a\"");
  const FetchResult r = rig.fetch->run("https://server.example/release", Which::kCandidate, Slot{0}, core::net::Budget{});
  INFO(rig.out.text);
  CHECK(r.verdict == FetchVerdict::kStored);
  CHECK(std::string(r.version) == image.version);
  CHECK(r.expected == image.file.size());
  CHECK(r.bytes == image.file.size());
  for (const char* line : {"check 1, the signature: passed", "check 2, the manifest: passed",
                           "check 3, the image for this slot: passed", "check 4, the file's size and hash: passed",
                           "check 5, the descriptor inside the file: passed", "check 6, the region: passed"}) {
    CHECK_MESSAGE(has(rig.out.text, line), line);
  }
  const core::image::Record record = rig.store.read(Which::kCandidate);
  CHECK(record.present);
  CHECK(record.size == image.file.size());
  CHECK(rig.store.store.verify(Which::kCandidate, Slot{0}) == core::image::StoreError::kNone);
  // Three requests: the manifest, the signature, the one range of the image. With a trailing
  // slash the URL is the same.
  CHECK(rig.net.server().requests.size() == 3);
  CHECK(rig.net.server().requests[0].path == "/release/manifest.json");
  CHECK(rig.net.server().requests[1].path == "/release/manifest.json.sig");
  CHECK(rig.net.server().requests[2].header("Range") == "bytes=0-" + std::to_string(image.file.size() - 1));
}

TEST_CASE("refused at check 1 with the build's own trust table, by the tampered manifest, and by the unknown-key signature - before a byte of the image is asked for") {
  const Candidate image("store-a");
  SUBCASE("the normal build's table: not the test key, so the fixture release is not accepted") {
    FetchRig rig;
    serveRelease(rig.net.server(), "/r", image);
    rig.net.server().serve("/r/" + swdName(image), image.file);
    core::image::ImageVerdict normal(core::image::buildTrust(), StoreBench::makeUnit());
    // The build trusts the release key and never the specification's public test key, which is
    // what these fixtures are signed with (spec/FIRMWARE-MANIFEST.md section 3).
    REQUIRE_FALSE(core::image::trustsTestKey());
    Fetch fetch(rig.net.client, rig.net.download, rig.store.store, normal, rig.net.clock, &rig.out);
    const FetchResult r = fetch.run("https://server.example/r/", Which::kCandidate, Slot{0}, core::net::Budget{});
    CHECK(r.verdict == FetchVerdict::kRefused);
    CHECK(r.check == Check::kSignature);
    CHECK(rig.imageRequests(swdName(image)) == 0);
    CHECK(rig.net.server().requests.size() == 2);
    CHECK_FALSE(rig.store.read(Which::kCandidate).present);
  }
  SUBCASE("the specification's tampered manifest with the good signature") {
    FetchRig rig;
    const std::vector<std::uint8_t> tampered = fixtures::readSpec("manifest-repeater-tampered.json");
    const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig"));
    serveRelease(rig.net.server(), "/r", image, &tampered, &sig);
    rig.net.server().serve("/r/twinoak-repeater-rak4630-single-868-v0.3.0.bin", fixtures::readSpec("image-repeater.bin"));
    const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
    CHECK(r.verdict == FetchVerdict::kRefused);
    CHECK(r.check == Check::kSignature);
    CHECK(rig.imageRequests("twinoak-repeater") == 0);
  }
  SUBCASE("the specification's unknown-key signature with the good manifest") {
    FetchRig rig;
    const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater.json");
    const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater-unknown-key.json.sig"));
    serveRelease(rig.net.server(), "/r", image, &manifest, &sig);
    rig.net.server().serve("/r/twinoak-repeater-rak4630-single-868-v0.3.0.bin", fixtures::readSpec("image-repeater.bin"));
    const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
    CHECK(r.verdict == FetchVerdict::kRefused);
    CHECK(r.check == Check::kSignature);
    CHECK(has(rig.out.text, "refused at check 1"));
    CHECK(rig.imageRequests("twinoak-repeater") == 0);
  }
  SUBCASE("the specification's own repeater release, accepted with the test key") {
    FetchRig rig;
    const std::vector<std::uint8_t> manifest = fixtures::readSpec("manifest-repeater.json");
    const std::string sig = fixtures::asText(fixtures::readSpec("manifest-repeater.json.sig"));
    serveRelease(rig.net.server(), "/r", image, &manifest, &sig);
    rig.net.server().serve("/r/twinoak-repeater-rak4630-single-868-v0.3.0.bin", fixtures::readSpec("image-repeater.bin"));
    const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
    INFO(rig.out.text);
    CHECK(r.verdict == FetchVerdict::kStored);
    CHECK(std::string(r.file_name) == "twinoak-repeater-rak4630-single-868-v0.3.0.bin");
  }
}

TEST_CASE("right length, wrong hash: refused at check 4, nothing stored, the candidate slot empty, no resume into the junk") {
  FetchRig rig;
  const Candidate a("store-a");
  const Candidate b("store-b");  // the same size, other bytes
  REQUIRE(a.file.size() == b.file.size());
  serveRelease(rig.net.server(), "/r", a);
  rig.net.server().serve("/r/" + swdName(a), b.file);
  FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kRefused);
  CHECK(r.check == Check::kFile);
  CHECK(has(rig.out.text, "check 4, the file's size and hash"));
  const core::image::Record record = rig.store.read(Which::kCandidate);
  CHECK_FALSE(record.present);
  CHECK_FALSE(record.transfer);  // the progress record went with the junk: nothing resumes into it
  // Served right the next time, the whole file is asked for again, from zero.
  rig.net.server().serve("/r/" + swdName(a), a.file);
  rig.out.clear();
  r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kStored);
  CHECK(rig.net.server().requests.back().header("Range") == "bytes=0-" + std::to_string(a.file.size() - 1));
}

TEST_CASE("a file longer or shorter than the manifest says is refused as the wrong file, the slot empty") {
  FetchRig rig;
  const Candidate a("store-a");
  serveRelease(rig.net.server(), "/r", a);
  std::vector<std::uint8_t> longer = a.file;
  longer.push_back(0x55);
  rig.net.server().serve("/r/" + swdName(a), longer);
  FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kWrongFile);
  CHECK_FALSE(rig.store.read(Which::kCandidate).present);
  std::vector<std::uint8_t> shorter(a.file.begin(), a.file.end() - 1);
  rig.net.server().serve("/r/" + swdName(a), shorter);
  r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kWrongFile);
  CHECK_FALSE(rig.store.read(Which::kCandidate).present);
}

TEST_CASE("the budget stops a download at a resumable point; a restart of the Walter and the next wake finish it from the store's progress record") {
  FetchRig rig;
  const Candidate big("store-resume");  // 150 000 bytes: two of the store's 64 KB resume points inside it
  REQUIRE(big.file.size() > 2 * core::net::kRangeBytes);
  serveRelease(rig.net.server(), "/r", big);
  rig.net.server().serve("/r/" + swdName(big), big.file, "\"big\"");
  core::net::Budget one_range;
  one_range.bytes = core::net::kRangeBytes;
  FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, one_range);
  CHECK(r.verdict == FetchVerdict::kBudget);
  CHECK(r.held == core::net::kRangeBytes);
  CHECK(r.bytes == core::net::kRangeBytes);
  CHECK_FALSE(rig.store.read(Which::kCandidate).present);
  CHECK(rig.store.read(Which::kCandidate).transfer);
  CHECK(rig.store.read(Which::kCandidate).transferred == core::net::kRangeBytes);

  // The Walter restarts. The next wake: the manifest again (small), then the rest from 65536.
  rig.reboot();
  const std::size_t before = rig.net.server().requests.size();
  r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  INFO(rig.out.text);
  CHECK(r.verdict == FetchVerdict::kStored);
  CHECK(has(rig.out.text, "resuming at 65536 of 150000 bytes"));
  REQUIRE(rig.net.server().requests.size() >= before + 3);
  CHECK(rig.net.server().requests[before + 2].header("Range") == "bytes=65536-131071");
  CHECK(rig.store.store.verify(Which::kCandidate, Slot{0}) == core::image::StoreError::kNone);
  CHECK(rig.store.read(Which::kCandidate).size == big.file.size());
}

TEST_CASE("denied, later and a bad base URL; the golden image is never a target; the scanner slot is") {
  FetchRig rig;
  const Candidate a("store-a");
  rig.net.server().statuses["/denied/manifest.json"] = sim::StatusRule{403, "", "no"};
  rig.net.server().statuses["/later/manifest.json"] = sim::StatusRule{503, "60", "no"};
  FetchResult r = rig.fetch->run("https://server.example/denied", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kDenied);
  r = rig.fetch->run("https://server.example/later", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kLater);
  CHECK(r.retry_after_s == 60);
  r = rig.fetch->run("http://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kBadUrl);
  r = rig.fetch->run("https://server.example/r", Which::kGolden, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kNoSlot);
  CHECK(rig.net.server().requests.size() == 2);  // the golden refusal cost no request

  const Candidate scanner("store-scanner");
  serveRelease(rig.net.server(), "/s", scanner);
  rig.net.server().serve("/s/" + swdName(scanner), scanner.file);
  r = rig.fetch->run("https://server.example/s", Which::kScanner, Slot{0}, core::net::Budget{});
  INFO(rig.out.text);
  CHECK(r.verdict == FetchVerdict::kStored);
  CHECK(rig.store.read(Which::kScanner).present);
}

TEST_CASE("the manifest is bounded: one larger than 8 KB is refused before the signature is asked for") {
  FetchRig rig;
  rig.net.server().serve("/r/manifest.json", bench::patternBytes(9000));
  const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, core::net::Budget{});
  CHECK(r.verdict == FetchVerdict::kFailed);
  CHECK(has(r.why, "8 KB"));
  CHECK(rig.net.server().requests.size() == 1);
}

// -- the budget's sentence, per product (task p10-04) ---------------------------------------------
//
// A radio image's download resumes and the Walter's own does not, and until this task both
// sentences promised a resume (tasks/phase-9/FOLLOWUPS.md, *The Walter's own download does not
// resume, and the message says it does*). The difference is the target, so `Fetch` is where it is
// decided: `run()` writes into the image store, which keeps a progress record; `runInto()` writes
// into a target of the caller's - the Walter's other application slot - which does not.

namespace {

// The Walter's other application slot as a transfer target, in the one respect this is about: it
// keeps **no** progress record. `SelfUpdate::beginWrite` sets `written_ = 0` and the platform's
// erases the slot, and `abandonWrite()` zeroes it again, so a download the budget cut short has
// nothing to continue from (core/update/self_update.cpp). The three words are this image's own, as
// `SelfUpdate`'s are, so the fixture release of the Walter's product passes check 3.
class OwnSlotTarget final : public core::image::TransferTarget {
 public:
  OwnSlotTarget() {
    needs_.slot = Slot{0};
    needs_.role = core::image::role_name(self_.role);
    needs_.board = self_.board;  // the member's own bytes: a SlotNeeds only points at them
    needs_.band = self_.band;
  }

  const core::image::SlotNeeds& needs() const override { return needs_; }
  core::image::Use use() const override { return core::image::Use::kOta; }
  const core::flasher::BoardProfile* profile() const override { return nullptr; }
  const char* name() const override { return "the other application slot"; }
  bool writing() const override { return open_; }
  std::uint32_t written() const override { return written_; }
  std::uint32_t expectedSize() const override { return expected_; }
  const char* beginWrite(const core::image::ExpectedFile& expected) override {
    open_ = true;
    expected_ = expected.size();
    written_ = 0;  // the slot is erased when a write opens on it
    ++opens;
    return nullptr;
  }
  const char* write(const std::uint8_t*, std::uint32_t size) override {
    written_ += size;
    return nullptr;
  }
  const char* endWrite() override { return nullptr; }
  const core::flasher::ImageSource* staged(const char** why) override {
    *why = "this target stages nothing";
    return nullptr;
  }
  const char* commit(const core::image::AcceptedImage&) override { return nullptr; }
  void abandonWrite() override {
    open_ = false;
    written_ = 0;
  }

  int opens = 0;

 private:
  const core::image::Fields self_ = core::image::this_image();
  core::image::SlotNeeds needs_;
  bool open_ = false;
  std::uint32_t written_ = 0;
  std::uint32_t expected_ = 0;
};

// The `ota` entry's name, as swdName reads the `swd` one: the Walter's own file.
std::string otaName(const Candidate& image) {
  const std::string text(image.manifest.begin(), image.manifest.end());
  const std::size_t use = text.find("\"use\": \"ota\"");
  REQUIRE(use != std::string::npos);
  const std::size_t name = text.find("\"name\": \"", use);
  REQUIRE(name != std::string::npos);
  const std::size_t start = name + 9;
  return text.substr(start, text.find('"', start) - start);
}

}  // namespace

TEST_CASE("describe(kBudget) says what the next wake does with the bytes, and the two products' next wakes differ") {
  CHECK(std::string(core::update::describe(FetchVerdict::kBudget, false)) ==
        "the wake's budget ran out; the download resumes next wake");
  CHECK(std::string(core::update::describe(FetchVerdict::kBudget, true)) ==
        "the wake's budget ran out; this slot keeps no progress, so the download starts again from zero next wake");
  // Nothing else reads differently: the flag is kBudget's alone.
  const FetchVerdict others[] = {FetchVerdict::kStored,  FetchVerdict::kRefused, FetchVerdict::kDenied,
                                 FetchVerdict::kLater,   FetchVerdict::kFailed,  FetchVerdict::kBadUrl,
                                 FetchVerdict::kNoSlot,  FetchVerdict::kStoreRefused,
                                 FetchVerdict::kWrongFile};
  for (const FetchVerdict v : others) {
    CHECK(std::string(core::update::describe(v, false)) == std::string(core::update::describe(v, true)));
  }
}

TEST_CASE("a radio image's budget stop is the resumable one: the result says so, and the next wake finishes it") {
  FetchRig rig;
  const Candidate big("store-resume");
  serveRelease(rig.net.server(), "/r", big);
  rig.net.server().serve("/r/" + swdName(big), big.file, "\"big\"");
  core::net::Budget one_range;
  one_range.bytes = core::net::kRangeBytes;
  const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0}, one_range);
  CHECK(r.verdict == FetchVerdict::kBudget);
  CHECK_FALSE(r.own_image);
  const std::string said = core::update::describe(r.verdict, r.own_image);
  CHECK(has(said, "resumes next wake"));
  CHECK_FALSE(has(said, "starts again from zero"));
  // And it is a promise the store can keep: the bytes and the progress record are in flash.
  CHECK(rig.store.read(Which::kCandidate).transfer);
  CHECK(rig.store.read(Which::kCandidate).transferred == core::net::kRangeBytes);
}

TEST_CASE("the Walter's own budget stop promises no resume: the slot keeps no progress, and the sentence says so") {
  FetchRig rig;
  const Candidate self("self-a");  // 12 288 bytes of the Walter's own product
  serveRelease(rig.net.server(), "/w", self);
  rig.net.server().serve("/w/" + otaName(self), self.file, "\"self\"");
  OwnSlotTarget slot;
  core::net::Budget part;
  part.bytes = 4096;  // a third of the file: the download stops with the budget spent
  const FetchResult r = rig.fetch->runInto("https://server.example/w", slot, part);
  INFO(rig.out.text);
  CHECK(r.verdict == FetchVerdict::kBudget);
  CHECK(r.own_image);
  CHECK(r.held == 4096);
  CHECK(r.expected == self.file.size());
  const std::string said = core::update::describe(r.verdict, r.own_image);
  CHECK(has(said, "starts again from zero"));
  CHECK_FALSE(has(said, "resumes next wake"));
  // Why it says so: the abandoned write left nothing behind, so the next wake opens the slot again
  // and the download begins at zero - the bytes above were paid for twice.
  CHECK(slot.written() == 0);
  CHECK_FALSE(slot.writing());
  const FetchResult again = rig.fetch->runInto("https://server.example/w", slot, part);
  CHECK(again.verdict == FetchVerdict::kBudget);
  CHECK(again.resumed_from == 0);
  CHECK(again.bytes == 4096);
  CHECK(slot.opens == 2);
}

// -- who is asking, and what a refusal says (task p10-05) ----------------------------------------
//
// Every request of a fetch - the manifest, its signature and every range - carries
// `X-Agent-Version`, because the client adds it (core/net/client.h). Until this task none of them
// did, and meshmaster.dk refuses an `/agent/v1` request without it with a 400: job 22841 of
// 8 November 2026, *a status this firmware does not act on*, which named neither the status nor
// the file. The scripted server holds that rule now (sim/scripted_server.h), so the two downloads
// below pass a server that refuses a request that does not say who it is.

TEST_CASE("both products' downloads pass a server that refuses a request without X-Agent-Version, and every request of them carries it") {
  SUBCASE("a radio image, from a release under the agent API's prefix") {
    FetchRig rig;
    const Candidate image("store-a");
    const std::string base = "/agent/v1/firmware/9";
    serveRelease(rig.net.server(), base, image);
    rig.net.server().serve(base + "/files/" + swdName(image), image.file, "\"a\"");
    const FetchResult r = rig.fetch->run(("https://server.example" + base).c_str(), Which::kCandidate,
                                         Slot{0}, core::net::Budget{}, "Bearer mma_a_token", "files");
    INFO(rig.out.text);
    CHECK(r.verdict == FetchVerdict::kStored);
    CHECK(r.bytes == image.file.size());
    CHECK(rig.net.server().unversioned_refused == 0);
    REQUIRE(rig.net.server().requests.size() == 3);
    for (const sim::SeenRequest& seen : rig.net.server().requests) {
      CHECK(seen.header("X-Agent-Version") == core::version());
      CHECK(seen.header("Authorization") == "Bearer mma_a_token");
    }
    CHECK(rig.net.server().requests[0].path == base + "/manifest.json");
    CHECK(rig.net.server().requests[1].path == base + "/manifest.json.sig");
    CHECK(rig.net.server().requests[2].path == base + "/files/" + swdName(image));
  }

  SUBCASE("the Walter's own image, into the other application slot") {
    FetchRig rig;
    const Candidate self("self-a");
    const std::string base = "/agent/v1/firmware/9";
    serveRelease(rig.net.server(), base, self);
    rig.net.server().serve(base + "/files/" + otaName(self), self.file, "\"self\"");
    OwnSlotTarget slot;
    const FetchResult r = rig.fetch->runInto(("https://server.example" + base).c_str(), slot,
                                             core::net::Budget{}, "Bearer mma_a_token", "files");
    INFO(rig.out.text);
    // The slot stages nothing, so check 4 cannot run here (OwnSlotTarget::staged); what this
    // subcase is about is that the three requests went out and were served.
    CHECK(r.verdict == FetchVerdict::kStoreRefused);
    CHECK(r.bytes == self.file.size());
    CHECK(rig.net.server().unversioned_refused == 0);
    REQUIRE(rig.net.server().requests.size() == 3);
    for (const sim::SeenRequest& seen : rig.net.server().requests) {
      CHECK(seen.header("X-Agent-Version") == core::version());
    }
  }
}

TEST_CASE("a refused request of a fetch names its status and its file, with the server's own detail") {
  SUBCASE("the manifest refused with problem details: the sentence job 22841 should have come back with") {
    FetchRig rig;
    rig.net.server().statuses["/r/manifest.json"] = sim::StatusRule{
        400, "",
        "{\"type\":\"bad-request\",\"title\":\"Request not accepted\",\"status\":400,"
        "\"detail\":\"Every request carries X-Agent-Version: the agent's build.\"}"};
    const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0},
                                         core::net::Budget{});
    CHECK(r.verdict == FetchVerdict::kFailed);
    CHECK(r.status == 400);
    CHECK(std::string(r.why) ==
          "http 400 for manifest.json: Every request carries X-Agent-Version: the agent's build.");
    // Nothing of the image was asked for: the first of the three steps ended it.
    CHECK(rig.net.server().requests.size() == 1);
  }

  SUBCASE("a signature that is not there: the 404 names the file it asked for") {
    FetchRig rig;
    const Candidate image("store-a");
    rig.net.server().serve("/r/manifest.json", image.manifest);
    const FetchResult r = rig.fetch->run("https://server.example/r", Which::kCandidate, Slot{0},
                                         core::net::Budget{});
    CHECK(r.verdict == FetchVerdict::kFailed);
    CHECK(r.status == 404);
    CHECK(std::string(r.why) == "http 404 for manifest.json.sig");
  }
}
