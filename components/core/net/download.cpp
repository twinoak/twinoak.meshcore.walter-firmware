#include "core/net/download.h"

#include <cstdio>
#include <cstring>

namespace core {
namespace net {

namespace {

// `bytes a-b/total`, strictly (RFC 7233 §4.2): decimal `a`, `-`, decimal `b`, `/`, decimal
// `total`. A `*` anywhere is refused - the size is known and a server that will not say it
// cannot be held to it.
bool parseContentRange(const char* text, std::uint64_t* first, std::uint64_t* last,
                       std::uint64_t* total) {
  if (text == nullptr) return false;
  static const char kUnit[] = "bytes ";
  if (std::strncmp(text, kUnit, 6) != 0) return false;
  const char* p = text + 6;
  auto number = [&p](std::uint64_t* out) {
    std::size_t digits = 0;
    std::uint64_t value = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + static_cast<std::uint64_t>(*p - '0');
      if (++digits > 12) return false;
      ++p;
    }
    if (digits == 0) return false;
    *out = value;
    return true;
  };
  if (!number(first) || *p != '-') return false;
  ++p;
  if (!number(last) || *p != '/') return false;
  ++p;
  if (!number(total) || *p != 0) return false;
  return *first <= *last;
}

// The sink one range's response goes to. It sees the status and the headers before the first
// byte (interfaces/http.h), and decides from them: a 206 whose Content-Range is exactly the
// range asked for, out of exactly the size expected, is forwarded; a 200 restarts the sink and
// is forwarded from zero; anything else is refused without a byte reaching the sink. It also
// counts, so that the next attempt knows where to continue after a drop, and refuses more than
// the file is long.
class RangeSink final : public interfaces::HttpBodySink {
 public:
  RangeSink(const interfaces::HttpResponse& response, const interfaces::HttpWantedHeader& content_range,
            DownloadSink& sink, std::uint32_t offset, std::uint32_t length, std::uint32_t expected)
      : response_(response), content_range_(content_range), sink_(sink), offset_(offset),
        length_(length), expected_(expected) {}

  bool piece(const std::uint8_t* data, std::size_t size) override {
    if (!decided_) {
      decided_ = true;
      if (!decide()) return false;
    }
    if (!forward_) return false;
    if (delivered_ + size > limit_) {
      why_ = "the server sent more than the file is long";
      too_long_ = true;
      return false;
    }
    if (!sink_.write(data, size)) {
      why_ = "the sink refused a write";
      sink_failed_ = true;
      return false;
    }
    delivered_ += size;
    return true;
  }

  // Called by the download when the body was empty, so that a decision is reached anyway.
  bool decideIfNeeded() {
    if (decided_) return forward_;
    decided_ = true;
    return decide();
  }

  bool forward() const { return forward_; }
  bool restarted() const { return restarted_; }
  bool wrongFile() const { return wrong_file_; }
  bool wrongRange() const { return wrong_range_; }
  bool tooLong() const { return too_long_; }
  bool sinkFailed() const { return sink_failed_; }
  std::uint64_t delivered() const { return delivered_; }
  std::uint64_t limit() const { return limit_; }
  const char* why() const { return why_; }

 private:
  bool decide() {
    const std::uint16_t status = response_.status;
    if (status == 206) {
      std::uint64_t first = 0, last = 0, total = 0;
      // The header's *value* is read, not its `present` flag: the parser fills the value buffer
      // in place as the header streams past, before the first body piece, while `present` is a
      // field of the array the client hands the transport and is copied back to the caller's
      // only when the request returns. An empty value is "no Content-Range"; a cut one does
      // not parse.
      if (content_range_.value == nullptr || content_range_.value[0] == 0 ||
          !parseContentRange(content_range_.value, &first, &last, &total)) {
        why_ = "206 without a Content-Range this firmware can read";
        wrong_range_ = true;
        return false;
      }
      if (total != expected_) {
        why_ = "the server's file is not the size the manifest promised (Content-Range's total)";
        wrong_file_ = true;
        return false;
      }
      if (first != offset_ || last != static_cast<std::uint64_t>(offset_) + length_ - 1) {
        why_ = "206 with a Content-Range that is not the range asked for";
        wrong_range_ = true;
        return false;
      }
      if (response_.has_content_length && response_.content_length != length_) {
        why_ = "206 whose Content-Length is not the range's length";
        wrong_range_ = true;
        return false;
      }
      forward_ = true;
      limit_ = length_;
      return true;
    }
    if (status == 200) {
      // The whole file: the server ignores ranges, or If-Range said the file changed. Either
      // way nothing held is known to be this file's, so the sink starts over.
      if (response_.has_content_length && response_.content_length != expected_) {
        why_ = "the server's file is not the size the manifest promised (Content-Length)";
        wrong_file_ = true;
        return false;
      }
      if (offset_ != 0) {
        if (!sink_.restart()) {
          why_ = "the sink could not restart from zero";
          sink_failed_ = true;
          return false;
        }
        restarted_ = true;
      }
      forward_ = true;
      limit_ = expected_;
      return true;
    }
    // Any other status: the body is nobody's business; the download reads the verdict.
    why_ = "not a 206 or a 200";
    return false;
  }

  const interfaces::HttpResponse& response_;
  const interfaces::HttpWantedHeader& content_range_;
  DownloadSink& sink_;
  std::uint32_t offset_;
  std::uint32_t length_;
  std::uint32_t expected_;
  bool decided_ = false;
  bool forward_ = false;
  bool restarted_ = false;
  bool wrong_file_ = false;
  bool wrong_range_ = false;
  bool too_long_ = false;
  bool sink_failed_ = false;
  std::uint64_t delivered_ = 0;
  std::uint64_t limit_ = 0;
  const char* why_ = "";
};

}  // namespace

const char* describe(DownloadVerdict verdict) {
  switch (verdict) {
    case DownloadVerdict::kComplete: return "complete";
    case DownloadVerdict::kBudget: return "the wake's budget ran out at a range boundary; it resumes next wake";
    case DownloadVerdict::kDenied: return "denied (401 or 403): not retried in this wake";
    case DownloadVerdict::kLater: return "later (429 or 5xx): retry after Retry-After";
    case DownloadVerdict::kFailed: return "a range failed every attempt; it resumes next wake";
    case DownloadVerdict::kRefusedUrl: return "the URL was refused";
    case DownloadVerdict::kWrongFile: return "the server's file is not the size the manifest promised";
    case DownloadVerdict::kSinkFailed: return "the sink refused a write or a restart";
  }
  return "?";
}

Download::Download(Client& client, interfaces::Clock& clock) : client_(client), clock_(clock) {}

DownloadResult Download::fetch(const char* url, std::uint32_t expected_size, const Budget& budget,
                               DownloadSink& sink, const char* authorization,
                               RangeListener* listener) {
  DownloadResult result;
  const std::uint64_t started = clock_.nowMs();
  const std::uint64_t budget_deadline = started + static_cast<std::uint64_t>(budget.seconds) * 1000u;
  validator_[0] = 0;

  auto finish = [&](DownloadVerdict verdict, const char* why) {
    result.verdict = verdict;
    result.why = why;
    result.held = sink.held();
    result.elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - started);
    return result;
  };

  std::uint32_t offset = sink.held();
  if (offset > expected_size) {
    // The sink holds more than the file is long: whatever it is, it is not this file.
    if (!sink.restart()) return finish(DownloadVerdict::kSinkFailed, "the sink holds more than the file and could not restart");
    ++result.restarts;
    offset = 0;
  }
  if (offset == expected_size) return finish(DownloadVerdict::kComplete, "the sink already held the whole file");

  std::uint32_t attempt = 0;
  while (offset < expected_size) {
    // The budget, before a range is begun.
    const std::uint64_t now = clock_.nowMs();
    if (result.bytes >= budget.bytes) return finish(DownloadVerdict::kBudget, "the wake's byte budget is spent");
    if (now + static_cast<std::uint64_t>(kMinRangeSeconds) * 1000u > budget_deadline) {
      return finish(DownloadVerdict::kBudget, "too little of the wake's time budget is left for another range");
    }
    std::uint64_t length = kRangeBytes;
    if (length > expected_size - offset) length = expected_size - offset;
    if (length > budget.bytes - result.bytes) length = budget.bytes - result.bytes;
    const std::uint32_t range_length = static_cast<std::uint32_t>(length);
    const std::uint64_t range_deadline_ms = kRangeTotalMs < (budget_deadline - now) ? kRangeTotalMs : (budget_deadline - now);

    // The request: Range, and If-Range once a validator is known.
    std::snprintf(range_, sizeof range_, "bytes=%lu-%lu", static_cast<unsigned long>(offset),
                  static_cast<unsigned long>(offset + range_length - 1));
    interfaces::HttpHeader headers[2];
    std::size_t header_count = 0;
    headers[header_count++] = interfaces::HttpHeader{"Range", range_};
    if (validator_[0] != 0) {
      headers[header_count++] = interfaces::HttpHeader{"If-Range", validator_};
      result.validator_used = true;
    }
    interfaces::HttpWantedHeader wanted[3] = {};
    wanted[0].name = "Content-Range";
    wanted[0].value = content_range_;
    wanted[0].capacity = sizeof content_range_;
    wanted[1].name = "ETag";
    wanted[1].value = etag_;
    wanted[1].capacity = sizeof etag_;
    wanted[2].name = "Last-Modified";
    wanted[2].value = last_modified_;
    wanted[2].capacity = sizeof last_modified_;

    Options options;
    options.authorization = authorization;
    // A credential in the head is wiped by the transport the moment it is on the wire (task
    // p4-03's rule, core/net/client.h): the agent's offer downloads with the server's token.
    options.sensitive = authorization != nullptr;
    options.headers = headers;
    options.header_count = header_count;
    options.wanted = wanted;
    options.wanted_count = 3;
    options.bound = 0xFFFFFFFFull;  // the RangeSink bounds the body itself, to the range or the file
    options.total_ms = static_cast<std::uint32_t>(range_deadline_ms);
    options.piece_ms = kRangePieceMs;

    // The response the transport fills in is where the sink reads the status and the headers
    // from, before the first body piece (interfaces/http.h); the client is told to use it.
    interfaces::HttpResponse seen;
    options.response = &seen;
    RangeSink range_sink(seen, wanted[0], sink, offset, range_length, expected_size);
    ++attempt;
    const Result r = client_.get(url, options, range_sink);
    result.last_status = r.status;
    result.last_outcome = r.outcome;
    RangeReport report;
    report.offset = offset;
    report.length = range_length;
    report.status = r.status;
    report.attempt = attempt;

    // A 200 or a 206 whose body never came (an empty body) still needs a decision for the record.
    if (r.verdict == Verdict::kOk && r.bytes == 0) range_sink.decideIfNeeded();

    if (range_sink.restarted()) {
      ++result.restarts;
      offset = 0;
    }
    const std::uint64_t delivered = range_sink.delivered();
    result.bytes += delivered;
    offset += static_cast<std::uint32_t>(delivered);

    if (range_sink.sinkFailed()) {
      report.why = range_sink.why();
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kSinkFailed, range_sink.why());
    }
    if (range_sink.wrongFile()) {
      report.why = range_sink.why();
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kWrongFile, range_sink.why());
    }
    if (range_sink.tooLong()) {
      report.why = range_sink.why();
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kWrongFile, range_sink.why());
    }
    if (r.status == 416) {
      report.why = "416: the server's file is shorter than the manifest promised";
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kWrongFile, report.why);
    }
    if (r.verdict == Verdict::kDenied) {
      report.why = r.why;
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kDenied, r.why);
    }
    if (r.verdict == Verdict::kLater) {
      result.retry_after_s = r.retry_after_s;
      report.why = r.why;
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kLater, r.why);
    }
    if (r.verdict == Verdict::kRefusedUrl) {
      report.why = r.why;
      if (listener != nullptr) listener->range(report);
      return finish(DownloadVerdict::kRefusedUrl, r.why);
    }

    // Did this attempt deliver what it asked for? The whole range (or, after a 200, the whole
    // file), with the response complete.
    const bool complete_body = r.outcome == interfaces::HttpOutcome::kOk;
    const bool got_all = range_sink.forward() && complete_body && delivered == range_sink.limit();
    if (got_all) {
      ++result.ranges;
      report.ok = true;
      report.why = range_sink.restarted() ? "200: the whole file, from zero" : (r.status == 200 ? "200: the whole file" : "206");
      if (listener != nullptr) listener->range(report);
      attempt = 0;
      // The validator, from the first range that had one.
      if (validator_[0] == 0) {
        if (wanted[1].present && !wanted[1].truncated && etag_[0] != 0) {
          std::memcpy(validator_, etag_, sizeof validator_);
        } else if (wanted[2].present && !wanted[2].truncated && last_modified_[0] != 0) {
          std::memcpy(validator_, last_modified_, sizeof validator_);
        }
      }
      continue;
    }

    // Not whole: a drop, a timeout, a wrong range, a refused status. The bytes that did arrive
    // are in the sink and the next attempt continues after them.
    report.why = range_sink.wrongRange() ? range_sink.why() : r.why;
    if (listener != nullptr) listener->range(report);
    if (attempt >= kRangeTries) {
      return finish(DownloadVerdict::kFailed, report.why);
    }
    ++result.retries;
    if (clock_.nowMs() + kRetryPauseMs < budget_deadline) clock_.delayMs(kRetryPauseMs);
  }
  return finish(DownloadVerdict::kComplete, "complete");
}

}  // namespace net
}  // namespace core
