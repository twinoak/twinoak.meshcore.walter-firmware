// The download (task p3-02): a file whole; in ranges; a drop at every range boundary and in the
// middle of every range (a loop) - the file arrives intact and no byte is fetched twice beyond the
// range in flight; a resume from what the sink holds; a server that ignores ranges; one that
// answers 206 with a wrong Content-Range; one whose file changes between ranges (If-Range); a
// file longer than expected and one shorter; denied and later from the server; the budgets in
// bytes and seconds, each stopping cleanly at a resumable point. Task p10-05: the four headers a
// range carries, and the 416's new sentence.
#include <doctest.h>

#include <string>

#include "core/version.h"
#include "net_bench.h"

using bench::has;
using bench::MemoryDownloadSink;
using bench::NetBench;
using bench::patternBytes;
using core::net::Budget;
using core::net::DownloadResult;
using core::net::DownloadVerdict;
using core::net::kRangeBytes;

namespace {

// How many bytes of body the server sent over the whole download: the sum of the Content-Length
// of every 206 and 200 it answered, read back from its own requests' Range headers.
std::uint64_t bodyBytesServed(const sim::ScriptedServer& server, std::uint64_t file_size) {
  std::uint64_t total = 0;
  for (const sim::SeenRequest& r : server.requests) {
    const std::string range = r.header("Range");
    unsigned long a = 0, b = 0;
    if (std::sscanf(range.c_str(), "bytes=%lu-%lu", &a, &b) == 2) {
      total += (b >= file_size ? file_size - 1 : b) - a + 1;
    } else {
      total += file_size;
    }
  }
  return total;
}

}  // namespace

TEST_CASE("a file whole, in one range when it is small and in several when it is not") {
  NetBench b;
  const std::vector<std::uint8_t> small = patternBytes(1000);
  b.server().serve("/small.bin", small, "\"s1\"");
  MemoryDownloadSink sink;
  DownloadResult r = b.download.fetch("https://server.example/small.bin", 1000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink.bytes == small);
  CHECK(r.ranges == 1);
  CHECK(r.bytes == 1000);
  CHECK(b.server().requests.size() == 1);
  CHECK(b.server().requests[0].header("Range") == "bytes=0-999");
  CHECK_FALSE(b.server().requests[0].has("If-Range"));

  const std::vector<std::uint8_t> big = patternBytes(3 * kRangeBytes + 777, 7);
  b.server().serve("/big.bin", big, "\"b1\"");
  MemoryDownloadSink sink2;
  r = b.download.fetch("https://server.example/big.bin", static_cast<std::uint32_t>(big.size()), Budget{}, sink2);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink2.bytes == big);
  CHECK(r.ranges == 4);
  CHECK(r.retries == 0);
  CHECK(r.validator_used);
  // The ranges asked for, and If-Range on every one after the first.
  const std::vector<sim::SeenRequest>& reqs = b.server().requests;
  REQUIRE(reqs.size() == 5);
  CHECK(reqs[1].header("Range") == "bytes=0-65535");
  CHECK(reqs[2].header("Range") == "bytes=65536-131071");
  CHECK(reqs[2].header("If-Range") == "\"b1\"");
  CHECK(reqs[4].header("Range") == "bytes=196608-197384");
  CHECK(bodyBytesServed(b.server(), big.size()) - 1000 == big.size());  // nothing fetched twice
}

TEST_CASE("a drop at every range boundary and in the middle of every range: the file arrives intact, one retry, nothing fetched twice beyond the range in flight") {
  const std::vector<std::uint8_t> file = patternBytes(2 * kRangeBytes + 5000, 3);
  const std::uint32_t ranges = 3;
  int cases = 0;
  for (std::uint32_t range = 0; range < ranges; ++range) {
    const std::uint32_t length = range < 2 ? kRangeBytes : 5000;
    for (std::uint64_t at : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(length / 2), std::uint64_t(length - 1)}) {
      NetBench b;
      b.server().serve("/f.bin", file, "\"f\"");
      b.socket().drop_at_request = static_cast<int>(range);
      b.socket().drop_after_body_bytes = at;
      MemoryDownloadSink sink;
      INFO("range " << range << " dropped after " << at << " of its " << length << " bytes");
      const DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, sink);
      REQUIRE_MESSAGE(r.verdict == DownloadVerdict::kComplete, "range " << range << " dropped after " << at << ": " << r.why);
      REQUIRE_MESSAGE(sink.bytes == file, "range " << range << " dropped after " << at);
      CHECK(r.retries == 1);
      CHECK(sink.restarts == 0);
      // One extra request - the dropped attempt - and it asked for the whole range and got `at`
      // bytes of it; the retry continued from there with a full range of its own, so the ranges
      // after the drop are re-aligned and there may be one fewer of them. The bytes asked for
      // twice are exactly those the drop lost - the range in flight, never more.
      CHECK(r.ranges >= ranges - 1);
      CHECK(r.ranges <= ranges);
      CHECK(b.server().requests.size() == r.ranges + 1);
      CHECK(bodyBytesServed(b.server(), file.size()) == file.size() + length - at);
      ++cases;
    }
  }
  MESSAGE(cases << " drops, each recovered with one retry and no byte fetched twice");
}

TEST_CASE("a range that fails three times ends the download, resumable, with the bytes that did arrive kept") {
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(kRangeBytes + 100);
  b.server().serve("/f.bin", file);
  b.socket().stall = true;  // nothing ever arrives
  MemoryDownloadSink sink;
  Budget budget;
  budget.seconds = 3600;
  const DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), budget, sink);
  CHECK(r.verdict == DownloadVerdict::kFailed);
  CHECK(r.retries == core::net::kRangeTries - 1);
  CHECK(b.socket().opens == core::net::kRangeTries);
  CHECK(sink.bytes.empty());
  // Each attempt waited the piece deadline for a first byte that never came, with the retry
  // pause between; nothing shorter, nothing longer than the range's own deadline.
  CHECK(b.clock.nowMs() >= core::net::kRangeTries * core::net::kRangePieceMs);
  CHECK(b.clock.nowMs() <= core::net::kRangeTries * (core::net::kRangeTotalMs + core::net::kRetryPauseMs));
}

TEST_CASE("a resume continues from what the sink holds, and only that is asked for") {
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(2 * kRangeBytes + 10, 5);
  b.server().serve("/f.bin", file, "", "Wed, 30 Sep 2026 02:39:56 GMT");
  MemoryDownloadSink sink;
  sink.bytes.assign(file.begin(), file.begin() + kRangeBytes);  // what a previous wake left (the store's resume point)
  const DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink.bytes == file);
  CHECK(r.bytes == kRangeBytes + 10);
  REQUIRE(b.server().requests.size() == 2);
  CHECK(b.server().requests[0].header("Range") == "bytes=65536-131071");
  CHECK(b.server().requests[1].header("If-Range") == "Wed, 30 Sep 2026 02:39:56 GMT");  // Last-Modified when there is no ETag

  // A sink that already holds the whole file: nothing is asked for.
  MemoryDownloadSink full;
  full.bytes = file;
  const DownloadResult r2 = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, full);
  CHECK(r2.verdict == DownloadVerdict::kComplete);
  CHECK(b.server().requests.size() == 2);
}

TEST_CASE("a server that ignores ranges: the whole file is taken as the file, from zero, restarting a sink that held some of it") {
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(kRangeBytes + 500, 9);
  b.server().serve("/f.bin", file);
  b.server().ranges = false;
  MemoryDownloadSink sink;
  DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink.bytes == file);
  CHECK(r.ranges == 1);
  CHECK(r.restarts == 0);  // nothing was held, so nothing to restart

  MemoryDownloadSink held;
  held.bytes.assign(file.begin(), file.begin() + 1000);
  r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, held);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(held.bytes == file);
  CHECK(held.restarts == 1);  // the 200 restarted it: nothing was appended to the 1000
  CHECK(r.restarts == 1);

  // A sink that cannot restart ends the download without a byte appended.
  MemoryDownloadSink stuck;
  stuck.bytes.assign(file.begin(), file.begin() + 1000);
  stuck.refuse_restart = true;
  r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, stuck);
  CHECK(r.verdict == DownloadVerdict::kSinkFailed);
  CHECK(stuck.bytes.size() == 1000);
}

TEST_CASE("a 206 with a wrong Content-Range is refused before a byte reaches the sink") {
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(3000);
  b.server().serve("/f.bin", file);
  b.server().wrong_content_range = true;
  MemoryDownloadSink sink;
  const DownloadResult r = b.download.fetch("https://server.example/f.bin", 3000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kFailed);
  CHECK(sink.bytes.empty());
  CHECK(has(std::string(r.why), "Content-Range"));
  CHECK(r.retries == core::net::kRangeTries - 1);
}

TEST_CASE("a file that changes between ranges: If-Range makes the server send the new whole file, and the sink restarts rather than splices") {
  NetBench b;
  const std::vector<std::uint8_t> old_file = patternBytes(2 * kRangeBytes, 11);
  const std::vector<std::uint8_t> new_file = patternBytes(2 * kRangeBytes, 12);
  b.server().serve("/f.bin", old_file, "\"old\"");
  MemoryDownloadSink sink;
  // A budget of one range: the first wake gets the first 64 KB of the old file.
  Budget one;
  one.bytes = kRangeBytes;
  DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(old_file.size()), one, sink);
  CHECK(r.verdict == DownloadVerdict::kBudget);
  CHECK(sink.bytes.size() == kRangeBytes);
  // The file is replaced under the same URL. The next wake resumes with the ETag it saw... but a
  // new wake has no memory of the ETag: the validator is learned from the first range of *this*
  // download. So the resume's first request has no If-Range, and the server's 206 splices.
  // That is why the manifest's hash (check 4) is what finally judges a file, and why this test
  // is about the case the download *can* notice: a change between two ranges of one wake.
  b.server().serve("/f.bin", new_file, "\"new\"");
  MemoryDownloadSink sink2;
  Budget two_ranges;
  two_ranges.bytes = 2 * kRangeBytes;
  const std::vector<std::uint8_t> three = patternBytes(3 * kRangeBytes, 13);
  const std::vector<std::uint8_t> three_new = patternBytes(3 * kRangeBytes, 14);
  b.server().serve("/g.bin", three, "\"g-old\"");
  // The first range is served from the old file; then the file changes; the second range carries
  // If-Range: "g-old", the server answers 200 with the new whole file, the sink restarts.
  struct Swap final : public core::net::RangeListener {
    sim::ScriptedServer& server;
    const std::vector<std::uint8_t>& replacement;
    bool swapped = false;
    Swap(sim::ScriptedServer& s, const std::vector<std::uint8_t>& r) : server(s), replacement(r) {}
    void range(const core::net::RangeReport& report) override {
      if (!swapped && report.ok) {
        server.serve("/g.bin", replacement, "\"g-new\"");
        swapped = true;
      }
    }
  } swap(b.server(), three_new);
  MemoryDownloadSink sink3;
  r = b.download.fetch("https://server.example/g.bin", static_cast<std::uint32_t>(three.size()), Budget{}, sink3, nullptr, &swap);
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink3.bytes == three_new);  // the new file, whole; not the old first range and the new rest
  CHECK(sink3.restarts == 1);
  CHECK(r.restarts == 1);
  REQUIRE(b.server().requests.size() >= 2);
  CHECK(b.server().requests[b.server().requests.size() - 1].header("If-Range") == "\"g-old\"");
}

TEST_CASE("a file longer than expected and one shorter are refused as not this file, and never retried") {
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(5000);
  b.server().serve("/f.bin", file);
  MemoryDownloadSink sink;
  // Longer: the manifest said 4000, the server has 5000 - the 206's total says so.
  DownloadResult r = b.download.fetch("https://server.example/f.bin", 4000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kWrongFile);
  CHECK(sink.bytes.empty());
  CHECK(b.server().requests.size() == 1);  // not retried
  // Shorter: the manifest said 6000; the first range asks 0-5999 and the server's total says 5000.
  r = b.download.fetch("https://server.example/f.bin", 6000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kWrongFile);
  CHECK(sink.bytes.empty());
  // A server that ignores ranges and sends one byte too many: cut, refused.
  b.server().ranges = false;
  b.server().extra_bytes = 1;
  r = b.download.fetch("https://server.example/f.bin", 5000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kWrongFile);
  CHECK(sink.bytes.empty());
  // A resume past the end of a file that shrank: 416.
  b.server().ranges = true;
  b.server().extra_bytes = 0;
  MemoryDownloadSink too_far;
  too_far.bytes.assign(5500, 0);
  r = b.download.fetch("https://server.example/f.bin", 6000, Budget{}, too_far);
  CHECK(r.verdict == DownloadVerdict::kWrongFile);
  // And it says which status and which file it was, with what a 416 means to a download after it
  // (task p10-05): the client's sentence, and this firmware's clause.
  CHECK(std::string(r.why) == "http 416 for f.bin: the server's file is shorter than the manifest promised");
}

TEST_CASE("a range carries four headers: the version, the range, its validator and the token") {
  // kMaxRequestHeaders is four and a range of a download uses all four since task p10-05, so a
  // fifth would be a compile error and a missing one a 400 from meshmaster.dk. The second range
  // is the one that carries them all: If-Range is learned from the first.
  NetBench b;
  const std::vector<std::uint8_t> file = patternBytes(2 * kRangeBytes, 31);
  b.server().serve("/agent/v1/firmware/9/files/f.bin", file, "\"v9\"");
  MemoryDownloadSink sink;
  const DownloadResult r =
      b.download.fetch("https://server.example/agent/v1/firmware/9/files/f.bin",
                       static_cast<std::uint32_t>(file.size()), Budget{}, sink, "Bearer mma_a_token");
  CHECK(r.verdict == DownloadVerdict::kComplete);
  CHECK(sink.bytes == file);
  REQUIRE(b.server().requests.size() == 2);
  CHECK(b.server().unversioned_refused == 0);
  for (const sim::SeenRequest& seen : b.server().requests) {
    CHECK(seen.header("X-Agent-Version") == core::version());
    CHECK(seen.header("Authorization") == "Bearer mma_a_token");
    CHECK(seen.has("Range"));
  }
  CHECK(b.server().requests[1].header("If-Range") == "\"v9\"");
  CHECK(b.server().requests[1].headers.count("content-type") == 0);
}

TEST_CASE("denied and later from the server stop the download at once, in this wake") {
  NetBench b;
  b.server().statuses["/denied.bin"] = sim::StatusRule{403, "", "no"};
  b.server().statuses["/later.bin"] = sim::StatusRule{503, "300", "maintenance"};
  MemoryDownloadSink sink;
  DownloadResult r = b.download.fetch("https://server.example/denied.bin", 5000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kDenied);
  CHECK(b.server().requests.size() == 1);
  r = b.download.fetch("https://server.example/later.bin", 5000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kLater);
  CHECK(r.retry_after_s == 300);
  CHECK(b.server().requests.size() == 2);
  CHECK(sink.bytes.empty());
  r = b.download.fetch("http://server.example/x.bin", 5000, Budget{}, sink);
  CHECK(r.verdict == DownloadVerdict::kRefusedUrl);
}

TEST_CASE("the budgets: bytes and seconds, each stopping cleanly at a range boundary the sink can resume from") {
  const std::vector<std::uint8_t> file = patternBytes(4 * kRangeBytes, 21);
  {
    NetBench b;
    b.server().serve("/f.bin", file, "\"x\"");
    MemoryDownloadSink sink;
    Budget bytes;
    bytes.bytes = 2 * kRangeBytes + 10;  // two ranges fit, a third does not whole; the third is cut to 10 bytes
    DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), bytes, sink);
    CHECK(r.verdict == DownloadVerdict::kBudget);
    CHECK(sink.bytes.size() == 2 * kRangeBytes + 10);
    CHECK(std::equal(sink.bytes.begin(), sink.bytes.end(), file.begin()));
    CHECK(r.bytes == 2 * kRangeBytes + 10);
    // The next wake: the rest, from where the sink is.
    r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, sink);
    CHECK(r.verdict == DownloadVerdict::kComplete);
    CHECK(sink.bytes == file);
  }
  {
    NetBench b;
    b.server().serve("/f.bin", file, "\"x\"");
    b.socket().piece_ms = 200;  // a slow link: a 64 KB range is 44 pieces, ~9 s
    MemoryDownloadSink sink;
    Budget seconds;
    seconds.seconds = 25;
    const std::uint64_t t0 = b.clock.nowMs();
    DownloadResult r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), seconds, sink);
    CHECK(r.verdict == DownloadVerdict::kBudget);
    CHECK(sink.bytes.size() % kRangeBytes == 0);  // stopped between ranges
    CHECK(sink.bytes.size() > 0);
    CHECK(sink.bytes.size() < file.size());
    CHECK(b.clock.nowMs() - t0 <= 25000 + core::net::kRangeTotalMs);
    r = b.download.fetch("https://server.example/f.bin", static_cast<std::uint32_t>(file.size()), Budget{}, sink);
    CHECK(r.verdict == DownloadVerdict::kComplete);
    CHECK(sink.bytes == file);
  }
}
