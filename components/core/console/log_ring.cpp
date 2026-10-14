#include "core/console/log_ring.h"

namespace core {
namespace console {

void LogRing::append(const char* text, std::size_t length) {
  if (quiet_ > 0 || text == nullptr) return;
  for (std::size_t i = 0; i < length; ++i) {
    if (wrapped_) ++dropped_;
    bytes_[next_] = text[i];
    if (++next_ == kBytes) {
      next_ = 0;
      wrapped_ = true;
    }
  }
}

void LogRing::clear() {
  next_ = 0;
  wrapped_ = false;
  dropped_ = 0;
}

void LogRing::writeTo(interfaces::TextOut& out) const {
  if (!wrapped_) {
    if (next_ > 0) out.write(bytes_, next_);
    return;
  }
  // Wrapped: the oldest byte is the one about to be overwritten. Walk forward to the first line
  // ending and start after it, so the first line printed is a whole one.
  std::size_t begin = next_;
  std::size_t skipped = 0;
  while (skipped < kBytes && bytes_[begin] != '\n') {
    begin = begin + 1 == kBytes ? 0 : begin + 1;
    ++skipped;
  }
  if (skipped == kBytes) return;  // not one line ending in the whole ring: nothing whole to print
  begin = begin + 1 == kBytes ? 0 : begin + 1;
  ++skipped;
  const std::size_t bytes = kBytes - skipped;
  if (bytes == 0) return;
  // At most two pieces: from `begin` to the end of the buffer, then from the start.
  const std::size_t first = begin + bytes <= kBytes ? bytes : kBytes - begin;
  out.write(bytes_ + begin, first);
  if (first < bytes) out.write(bytes_, bytes - first);
}

}  // namespace console
}  // namespace core
