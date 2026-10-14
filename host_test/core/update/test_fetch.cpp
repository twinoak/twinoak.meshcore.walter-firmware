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
#include "core/image/trust.h"
#include "core/net/trust.h"
#include "core/update/fetch.h"
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
  SUBCASE("the normal build's table: no key, so no release is accepted") {
    FetchRig rig;
    serveRelease(rig.net.server(), "/r", image);
    rig.net.server().serve("/r/" + swdName(image), image.file);
    core::image::ImageVerdict normal(core::image::buildTrust(), StoreBench::makeUnit());
    REQUIRE(core::image::buildTrust().empty());
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
