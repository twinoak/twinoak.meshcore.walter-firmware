#include "core/update/fetch.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace core {
namespace update {

namespace {

// A small response into a fixed buffer, bounded; what the manifest and its signature arrive in.
class BufferSink final : public interfaces::HttpBodySink {
 public:
  BufferSink(std::uint8_t* out, std::size_t capacity) : out_(out), capacity_(capacity) {}
  bool piece(const std::uint8_t* data, std::size_t size) override {
    if (size_ + size > capacity_) {
      overflow_ = true;
      return false;
    }
    std::memcpy(out_ + size_, data, size);
    size_ += size;
    return true;
  }
  std::size_t size() const { return size_; }
  bool overflow() const { return overflow_; }

 private:
  std::uint8_t* out_;
  std::size_t capacity_;
  std::size_t size_ = 0;
  bool overflow_ = false;
};

// The transfer target as the download's sink: the bytes go straight into the open write, `held()`
// is the target's resume point, and a restart from zero is an abandoned write, the slot erased
// (its progress record with it) and the write opened again. For the image store the erase is
// the store's own; for the Walter's other application slot (task p4-05) the target's beginWrite
// erases it, so `store` is null there.
class TargetSink final : public net::DownloadSink {
 public:
  TargetSink(image::TransferTarget& target, image::Store* store, image::Which which, board::Slot slot,
             const image::ExpectedFile& expected)
      : target_(target), store_(store), which_(which), slot_(slot), expected_(expected) {}

  std::uint32_t held() override { return target_.writing() ? target_.written() : 0; }

  bool write(const std::uint8_t* data, std::size_t size) override {
    const char* refused = target_.write(data, static_cast<std::uint32_t>(size));
    if (refused != nullptr) why_ = refused;
    return refused == nullptr;
  }

  bool restart() override {
    target_.abandonWrite();
    if (store_ != nullptr) {
      const image::StoreError erased = store_->erase(which_, slot_);
      if (erased != image::StoreError::kNone) {
        why_ = image::describe(erased);
        return false;
      }
    }
    const char* refused = target_.beginWrite(expected_);
    if (refused != nullptr) {
      why_ = refused;
      return false;
    }
    return target_.written() == 0;
  }

  const char* why() const { return why_; }

 private:
  image::TransferTarget& target_;
  image::Store* store_;
  image::Which which_;
  board::Slot slot_;
  const image::ExpectedFile& expected_;
  const char* why_ = "";
};

// The console's line per range, and the caller's own listener after it (task p4-05: the agent's
// progress report and lease extension go there, between two ranges).
class RangePrinter final : public net::RangeListener {
 public:
  RangePrinter(interfaces::TextOut* out, net::RangeListener* next) : out_(out), next_(next) {}
  void range(const net::RangeReport& r) override {
    if (next_ != nullptr) next_->range(r);
    if (out_ == nullptr) return;
    char line[160];
    const int n = std::snprintf(line, sizeof line, "  range %lu-%lu: %s%s%s (attempt %lu)\n",
                                static_cast<unsigned long>(r.offset),
                                static_cast<unsigned long>(r.offset + r.length - 1),
                                r.ok ? "ok, " : "failed, ", r.why,
                                r.status == 0 ? ", no response" : "",
                                static_cast<unsigned long>(r.attempt));
    if (n > 0) out_->write(line, static_cast<std::size_t>(n) < sizeof line ? static_cast<std::size_t>(n) : sizeof line - 1);
  }

 private:
  interfaces::TextOut* out_;
  net::RangeListener* next_;
};

}  // namespace

const char* describe(FetchVerdict verdict, bool own_image) {
  switch (verdict) {
    case FetchVerdict::kStored: return "stored: all six checks passed";
    case FetchVerdict::kRefused: return "refused by one of the six checks";
    case FetchVerdict::kDenied: return "denied by the server (401 or 403): not retried in this wake";
    case FetchVerdict::kLater: return "the server says later (429 or 5xx)";
    case FetchVerdict::kBudget:
      // Two sentences, because the two slots are not alike (core/update/fetch.h, task p10-04).
      return own_image ? "the wake's budget ran out; this slot keeps no progress, so the download "
                         "starts again from zero next wake"
                       : "the wake's budget ran out; the download resumes next wake";
    case FetchVerdict::kFailed: return "a request failed";
    case FetchVerdict::kBadUrl: return "the base URL was refused";
    case FetchVerdict::kNoSlot: return "no such slot, or the golden image (reached by a promotion, not a fetch)";
    case FetchVerdict::kStoreRefused: return "the image store refused the write";
    case FetchVerdict::kWrongFile: return "the server's file is not the size the manifest promised";
  }
  return "?";
}

Fetch::Fetch(net::Client& client, net::Download& download, image::Store& store,
             image::ImageVerdict& verdict, interfaces::Clock& clock, interfaces::TextOut* out)
    : client_(client), download_(download), store_(store), verdict_(verdict), clock_(clock),
      out_(out), target_(store) {}

void Fetch::print(const char* text) {
  if (out_ != nullptr) out_->write(text, std::strlen(text));
}

void Fetch::printf(const char* format, ...) {
  if (out_ == nullptr) return;
  char buf[200];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  if (n <= 0) return;
  out_->write(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1);
}

// `<base>` + `/` + `<name>` into url_, with one slash between whatever the base ends in.
bool Fetch::buildUrl(const char* base_url, const char* name) {
  if (base_url == nullptr || name == nullptr) return false;
  const std::size_t b = std::strlen(base_url);
  const std::size_t n = std::strlen(name);
  if (b == 0 || n == 0) return false;
  const bool slash = base_url[b - 1] == '/';
  if (b + (slash ? 0 : 1) + n > net::kMaxUrlLength) return false;
  std::memcpy(url_, base_url, b);
  std::size_t at = b;
  if (!slash) url_[at++] = '/';
  std::memcpy(url_ + at, name, n);
  url_[at + n] = 0;
  return true;
}

FetchResult Fetch::run(const char* base_url, image::Which which, board::Slot slot,
                       const net::Budget& budget, const char* authorization, const char* file_dir,
                       net::RangeListener* listener) {
  if (which == image::Which::kGolden) {
    FetchResult result;
    result.verdict = FetchVerdict::kNoSlot;
    result.why = "the golden image is reached by a promotion, not by a fetch";
    return result;
  }
  if (!target_.begin(which, slot)) {
    FetchResult result;
    result.verdict = FetchVerdict::kNoSlot;
    result.why = target_.reason();
    return result;
  }
  if (store_.writing()) {
    FetchResult result;
    result.verdict = FetchVerdict::kStoreRefused;
    result.why = image::describe(image::StoreError::kBusy);
    return result;
  }
  return fetchInto(base_url, target_, &store_, which, slot, budget, authorization, file_dir, listener);
}

FetchResult Fetch::runInto(const char* base_url, image::TransferTarget& target, const net::Budget& budget,
                           const char* authorization, const char* file_dir, net::RangeListener* listener) {
  // The Walter's own slot (task p4-05): the target's own preconditions are the caller's to ask
  // (core/update/self_update.h, canReceive()); here only a write already open refuses.
  if (target.writing() && target.written() == 0) {
    FetchResult result;
    result.own_image = true;
    result.verdict = FetchVerdict::kStoreRefused;
    result.why = "a transfer is already open on that slot";
    return result;
  }
  return fetchInto(base_url, target, nullptr, image::Which::kCandidate, board::Slot{}, budget, authorization,
                   file_dir, listener);
}

FetchResult Fetch::fetchInto(const char* base_url, image::TransferTarget& target, image::Store* store,
                             image::Which which, board::Slot slot, const net::Budget& budget,
                             const char* authorization, const char* file_dir, net::RangeListener* listener) {
  FetchResult result;
  // No store beside the target is the Walter's own slot (`runInto`): the slot carries no progress
  // record, which is the whole of the difference kBudget's sentence has to tell (task p10-04).
  result.own_image = store == nullptr;
  const std::uint64_t started = clock_.nowMs();
  auto finish = [&](FetchVerdict verdict, const char* why) {
    result.verdict = verdict;
    result.why = why;
    result.elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - started);
    return result;
  };
  // The slot cleared, so that nothing resumes into bytes that are not this file: the store's own
  // erase, or - for the Walter's slot, whose beginWrite erases - nothing more than the abandon.
  auto clear = [&]() {
    target.abandonWrite();
    if (store != nullptr) store->erase(which, slot);
  };

  // 1. The manifest and its signature, small and bounded.
  net::Options small;
  small.authorization = authorization;
  // A credential in the head is wiped by the transport the moment the head is on the wire (task
  // p4-03's rule, core/net/client.h): the agent's offer fetches with the server's token.
  small.sensitive = authorization != nullptr;
  small.total_ms = kManifestRequestMs;
  small.bound = image::transfer::kMaxManifestBytes;
  if (!buildUrl(base_url, "manifest.json")) return finish(FetchVerdict::kBadUrl, "the base URL and 'manifest.json' do not make a URL this firmware handles");
  BufferSink manifest_sink(manifest_, sizeof manifest_);
  net::Result r = client_.get(url_, small, manifest_sink);
  result.status = r.status;
  if (r.verdict == net::Verdict::kRefusedUrl) return finish(FetchVerdict::kBadUrl, r.why);
  if (r.verdict == net::Verdict::kDenied) return finish(FetchVerdict::kDenied, r.why);
  if (r.verdict == net::Verdict::kLater) {
    result.retry_after_s = r.retry_after_s;
    return finish(FetchVerdict::kLater, r.why);
  }
  if (r.verdict == net::Verdict::kTooLarge) return finish(FetchVerdict::kFailed, "the manifest is larger than this firmware reads (8 KB)");
  if (r.verdict != net::Verdict::kOk) return finish(FetchVerdict::kFailed, r.why);
  printf("fetch: manifest.json, %lu bytes\n", static_cast<unsigned long>(manifest_sink.size()));

  small.bound = image::transfer::kMaxSignatureBytes;
  if (!buildUrl(base_url, "manifest.json.sig")) return finish(FetchVerdict::kBadUrl, "the base URL is too long");
  BufferSink signature_sink(reinterpret_cast<std::uint8_t*>(signature_), sizeof signature_);
  r = client_.get(url_, small, signature_sink);
  result.status = r.status;
  if (r.verdict == net::Verdict::kDenied) return finish(FetchVerdict::kDenied, r.why);
  if (r.verdict == net::Verdict::kLater) {
    result.retry_after_s = r.retry_after_s;
    return finish(FetchVerdict::kLater, r.why);
  }
  if (r.verdict == net::Verdict::kTooLarge) return finish(FetchVerdict::kFailed, "the signature is longer than one line (128 bytes)");
  if (r.verdict != net::Verdict::kOk) return finish(FetchVerdict::kFailed, r.why);
  printf("fetch: manifest.json.sig, %lu bytes\n", static_cast<unsigned long>(signature_sink.size()));

  // 2. Checks 1, 2, 3 and the manifest's half of 6 - before a byte of the image is asked for.
  const image::Outcome judged = verdict_.judgeManifest(manifest_, manifest_sink.size(), signature_,
                                                       signature_sink.size(), target.needs(),
                                                       target.use(), target.profile());
  if (!judged.ok()) {
    result.check = judged.failed;
    printf("fetch: refused at %s: %s\n", image::describe(judged.failed), judged.why);
    return finish(FetchVerdict::kRefused, judged.why);
  }
  const image::ExpectedFile& expected = *verdict_.expected();
  std::memcpy(result.file_name, expected.name(), sizeof result.file_name);
  std::memcpy(result.version, expected.version(), sizeof result.version);
  std::memcpy(result.sha256, expected.sha256(), sizeof result.sha256);
  result.expected = expected.size();
  printf("fetch: check 1, the signature: passed (key %s)\n", "this build trusts");
  print("fetch: check 2, the manifest: passed\n");
  printf("fetch: check 3, the image for this slot: passed - %s, %lu bytes, version %s\n",
         expected.name(), static_cast<unsigned long>(expected.size()), expected.version());
  print("fetch: check 6, the manifest's region against this build's: passed\n");

  // 3. The file, into the slot. The store resumes an interrupted write of this very file.
  const char* refused = target.beginWrite(expected);
  if (refused != nullptr) return finish(FetchVerdict::kStoreRefused, refused);
  result.resumed_from = target.written();
  if (target.written() != 0) {
    printf("fetch: resuming at %lu of %lu bytes already in flash\n",
           static_cast<unsigned long>(target.written()), static_cast<unsigned long>(expected.size()));
  }
  // The file's URL: beside the manifest, or under `file_dir` when the server keeps its files in a
  // directory of their own (meshmaster.dk's `{base}/files/{name}`).
  char path[image::kMaxFileName + 32];
  if (file_dir != nullptr && file_dir[0] != 0) {
    const int n = std::snprintf(path, sizeof path, "%s/%s", file_dir, expected.name());
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof path) {
      target.abandonWrite();
      return finish(FetchVerdict::kBadUrl, "the file's directory and its name do not make a path this firmware handles");
    }
  } else {
    std::snprintf(path, sizeof path, "%s", expected.name());
  }
  if (!buildUrl(base_url, path)) {
    target.abandonWrite();
    return finish(FetchVerdict::kBadUrl, "the base URL and the file's name do not make a URL this firmware handles");
  }
  TargetSink sink(target, store, which, slot, expected);
  RangePrinter printer(out_, listener);
  const net::DownloadResult d = download_.fetch(url_, expected.size(), budget, sink, authorization, &printer);
  result.bytes = d.bytes;
  result.status = d.last_status;
  result.retry_after_s = d.retry_after_s;
  switch (d.verdict) {
    case net::DownloadVerdict::kComplete:
      break;
    case net::DownloadVerdict::kBudget:
      target.abandonWrite();
      result.held = d.held;
      return finish(FetchVerdict::kBudget, d.why);
    case net::DownloadVerdict::kDenied:
      target.abandonWrite();
      result.held = d.held;
      return finish(FetchVerdict::kDenied, d.why);
    case net::DownloadVerdict::kLater:
      target.abandonWrite();
      result.held = d.held;
      return finish(FetchVerdict::kLater, d.why);
    case net::DownloadVerdict::kFailed:
      target.abandonWrite();
      result.held = d.held;
      return finish(FetchVerdict::kFailed, d.why);
    case net::DownloadVerdict::kRefusedUrl:
      target.abandonWrite();
      return finish(FetchVerdict::kBadUrl, d.why);
    case net::DownloadVerdict::kWrongFile:
      // Not this file: the bytes are nobody's. The slot is cleared so that nothing resumes into
      // them.
      clear();
      return finish(FetchVerdict::kWrongFile, d.why);
    case net::DownloadVerdict::kSinkFailed:
      target.abandonWrite();
      return finish(FetchVerdict::kStoreRefused, sink.why()[0] != 0 ? sink.why() : d.why);
  }
  printf("fetch: %lu bytes in %lu range(s), %lu retr%s, %lu restart(s), %lu ms\n",
         static_cast<unsigned long>(d.bytes), static_cast<unsigned long>(d.ranges),
         static_cast<unsigned long>(d.retries), d.retries == 1 ? "y" : "ies",
         static_cast<unsigned long>(d.restarts), static_cast<unsigned long>(d.elapsed_ms));

  // Checks 4, 5 and the rest of 6 over the bytes as they lie in flash, then the record.
  refused = target.endWrite();
  if (refused != nullptr) {
    clear();
    return finish(FetchVerdict::kStoreRefused, refused);
  }
  const char* why = "";
  const flasher::ImageSource* staged = target.staged(&why);
  if (staged == nullptr) {
    clear();
    return finish(FetchVerdict::kStoreRefused, why);
  }
  const image::Outcome file = verdict_.judgeFile(*staged);
  if (!file.ok()) {
    result.check = file.failed;
    printf("fetch: refused at %s: %s\n", image::describe(file.failed), file.why);
    clear();  // the bytes are not this file: nothing may resume into them
    return finish(FetchVerdict::kRefused, file.why);
  }
  print("fetch: check 4, the file's size and hash: passed\n");
  print("fetch: check 5, the descriptor inside the file: passed\n");
  print("fetch: check 6, the region: passed\n");
  const image::AcceptedImage& accepted = *verdict_.accepted();
  std::memcpy(result.image_id, accepted.image().id, sizeof result.image_id);
  result.descriptor = accepted.descriptor();
  refused = target.commit(accepted);
  if (refused != nullptr) {
    target.abandonWrite();
    return finish(FetchVerdict::kStoreRefused, refused);
  }
  return finish(FetchVerdict::kStored, "stored");
}

}  // namespace update
}  // namespace core
