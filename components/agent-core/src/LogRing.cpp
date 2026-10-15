#include <agent/LogRing.h>

#include <cstring>

namespace mm {

namespace {

// The one ring, at namespace scope rather than inside logRing(): a function-local static would
// need a guard variable and a lock on the first call, and this is called from the log.
LogRing g_ring;

struct CopySink {
  char* out;
  size_t cap;
  size_t written;
};

void copySink(void* ctx, const char* text, size_t len) {
  CopySink* c = static_cast<CopySink*>(ctx);
  if (c->cap == 0) return;
  size_t room = c->cap - 1 - c->written;
  if (room > len) room = len;
  std::memcpy(c->out + c->written, text, room);
  c->written += room;
}

}  // namespace

LogRing& logRing() { return g_ring; }

void LogRing::add(const char* text, size_t len) {
  if (text == nullptr || len == 0) return;
  if (len > kCapacity) {
    text += len - kCapacity;
    len = kCapacity;
  }
  for (size_t i = 0; i < len; i++) {
    if (size_ == kCapacity) {
      // next_ is the oldest byte: overwriting a newline is one line lost from the front.
      if (buf_[next_] == '\n') dropped_++;
      wrapped_ = true;
    } else {
      size_++;
    }
    buf_[next_] = text[i];
    next_ = next_ + 1 == kCapacity ? 0 : next_ + 1;
  }
  lines_++;
}

void LogRing::add(const char* text) { add(text, text == nullptr ? 0 : std::strlen(text)); }

void LogRing::forEachChunk(void* ctx, void (*sink)(void* ctx, const char* text, size_t len)) const {
  if (sink == nullptr || size_ == 0) return;
  size_t start = (next_ + kCapacity - size_) % kCapacity;
  size_t n = size_;
  if (wrapped_) {
    size_t skip = 0;
    while (skip < n && buf_[(start + skip) % kCapacity] != '\n') skip++;
    skip++;  // the newline that ends the partial line goes too
    if (skip >= n) return;
    start = (start + skip) % kCapacity;
    n -= skip;
  }
  const size_t first = n <= kCapacity - start ? n : kCapacity - start;
  sink(ctx, buf_ + start, first);
  if (n > first) sink(ctx, buf_, n - first);
}

size_t LogRing::copyTo(char* out, size_t cap) const {
  if (out == nullptr || cap == 0) return 0;
  CopySink c = {out, cap, 0};
  forEachChunk(&c, &copySink);
  out[c.written] = '\0';
  return c.written;
}

void LogRing::clear() {
  next_ = 0;
  size_ = 0;
  wrapped_ = false;
}

}  // namespace mm
