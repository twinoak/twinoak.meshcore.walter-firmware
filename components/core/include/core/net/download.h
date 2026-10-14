// A file by URL into a sink (task p3-02), in ranges, resumable, bounded - the way a radio image
// of several hundred KB reaches the image store over an LTE-M link that drops, from a Walter that
// is awake for minutes at a time:
//
//   * **in ranges** of kRangeBytes, each its own request with a `Range: bytes=a-b` header, its
//     own deadline and its own retries. A drop costs the range in flight, not the file;
//   * **resumable**: the sink says how many bytes it already holds (the image store's resume
//     point of task p2-02, 64 KB apart) and the first range starts there. A wake that ends
//     mid-file continues in the next;
//   * **the server must honour the range**: a 206 whose `Content-Range` is not exactly the bytes
//     asked for, out of exactly the size expected, is refused before a byte reaches the sink.
//     A 200 in answer to a range request means the server ignores ranges (or the file changed,
//     below): the sink is **restarted from zero** and the 200's body taken as the whole file -
//     never appended to what was there;
//   * **a changed file restarts**: the `ETag` (or, failing that, `Last-Modified`) of the first
//     range goes out as `If-Range` on every range after it, so a server whose file was replaced
//     under the same URL answers 200 with the new whole file, and the rule above restarts rather
//     than splices;
//   * **never more than expected**: the size is known beforehand (from a manifest that passed
//     checks 1-3) and a server that sends past it is cut off and refused. The hash and the size
//     are checked at the end by the store's own check 4, not here;
//   * **a budget** in bytes and in seconds per wake, handed in: a range that would pass either is
//     not begun, and the download stops where the sink can resume from.
//
// The verdicts a range's request can end in are the client's (core/net/client.h): *denied* and
// *later* stop the download at once, in this wake; a transport failure is retried kRangeTries
// times for that range, then the download has failed.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/net/client.h"
#include "interfaces/clock.h"
#include "interfaces/http.h"

namespace core {
namespace net {

// -- the numbers, each a sentence; TO BE SET ON THE BENCH (docs/bench.md §12) -------------------

// One range. 64 KB is the image store's resume step (core/image/store.h, kResumeStep): a lost
// range and a lost progress record cost the same. Every range is a TLS handshake, seconds on
// LTE-M, so smaller ranges cost time; larger ones cost more when a drop comes. The bench's
// `fetch` of a real release (its bytes and its seconds) decides.
constexpr std::uint32_t kRangeBytes = 64 * 1024;

// How long one range's request may take, connect to last byte: a handshake and 64 KB over a
// link of tens of kbit/s is tens of seconds; two minutes is a link that has gone.
constexpr std::uint32_t kRangeTotalMs = 120000;

// Between two pieces of a range: the modem hands bytes over as they come; fifteen seconds of
// silence mid-range is a stalled connection.
constexpr std::uint32_t kRangePieceMs = 15000;

// Attempts per range before the download gives up on this wake: the first, and two more after
// a transport failure. A third failure in a row is not a glitch.
constexpr std::uint32_t kRangeTries = 3;

// A pause between two attempts at the same range, so that a server or a link that was busy is
// not hit at once again.
constexpr std::uint32_t kRetryPauseMs = 2000;

// The least a range needs of the second budget to be worth starting; a range begun with less
// would be cut by the budget and fetched again next wake.
constexpr std::uint32_t kMinRangeSeconds = 10;

// The budget when the caller does not say (the console's `fetch`): a whole radio image and ten
// minutes. The wake cycle (task p3-05) hands in its own, from the battery.
constexpr std::uint64_t kDefaultBudgetBytes = 2u * 1024u * 1024u;
constexpr std::uint32_t kDefaultBudgetS = 600;

// An ETag or Last-Modified kept for If-Range. ETags are a few dozen characters; an HTTP-date is
// 29. One that does not fit is not used (a resume without If-Range still works; it only cannot
// notice a changed file by this means).
constexpr std::size_t kMaxValidatorLength = 128;

struct Budget {
  std::uint64_t bytes = kDefaultBudgetBytes;
  std::uint32_t seconds = kDefaultBudgetS;
};

// Where the file goes. `held()` is how many bytes of this file the sink already has (the resume
// point); `write()` takes the next bytes in order; `restart()` throws away what is held and
// begins at zero (false when it cannot, which ends the download).
class DownloadSink {
 public:
  virtual ~DownloadSink() = default;
  virtual std::uint32_t held() = 0;
  virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
  virtual bool restart() = 0;
};

// What a range did, for a console line or a test; optional.
struct RangeReport {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  std::uint16_t status = 0;
  std::uint32_t attempt = 0;
  bool ok = false;
  const char* why = "";
};

class RangeListener {
 public:
  virtual ~RangeListener() = default;
  virtual void range(const RangeReport& report) = 0;
};

enum class DownloadVerdict : std::uint8_t {
  kComplete = 0,    // every byte up to the expected size is in the sink
  kBudget = 1,      // stopped at a range boundary: the bytes or the seconds ran out; resumable
  kDenied = 2,      // 401 or 403: not retried in this wake
  kLater = 3,       // 429 or 5xx: retry after `retry_after_s`
  kFailed = 4,      // a range failed kRangeTries times; resumable next wake
  kRefusedUrl = 5,  // not an https URL
  kWrongFile = 6,   // the server's file is not the size expected (Content-Range's total, a
                    // Content-Length, or a 416): not resumable, not retried
  kSinkFailed = 7,  // the sink refused a write or a restart
};

const char* describe(DownloadVerdict verdict);

struct DownloadResult {
  DownloadVerdict verdict = DownloadVerdict::kFailed;
  const char* why = "";
  std::uint64_t bytes = 0;          // delivered to the sink in this call
  std::uint32_t held = 0;           // what the sink holds afterwards
  std::uint32_t ranges = 0;         // requests that delivered a range (or the whole, after a 200)
  std::uint32_t retries = 0;        // attempts after a failure
  std::uint32_t restarts = 0;       // times the sink was restarted from zero
  std::uint16_t last_status = 0;
  interfaces::HttpOutcome last_outcome = interfaces::HttpOutcome::kOk;
  std::uint32_t retry_after_s = 0;
  std::uint32_t elapsed_ms = 0;
  bool validator_used = false;      // If-Range went out on the ranges after the first
};

class Download {
 public:
  Download(Client& client, interfaces::Clock& clock);

  // `expected_size` is the file's size as the manifest promised it. `authorization` goes with
  // every request (and is dropped by the client when a redirect leaves the host).
  DownloadResult fetch(const char* url, std::uint32_t expected_size, const Budget& budget,
                       DownloadSink& sink, const char* authorization = nullptr,
                       RangeListener* listener = nullptr);

 private:
  Client& client_;
  interfaces::Clock& clock_;
  char range_[48] = {};
  char validator_[kMaxValidatorLength] = {};
  char content_range_[96] = {};
  char etag_[kMaxValidatorLength] = {};
  char last_modified_[kMaxValidatorLength] = {};
};

}  // namespace net
}  // namespace core
