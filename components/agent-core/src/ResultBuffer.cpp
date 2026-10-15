#include <agent/ResultBuffer.h>

#include <cstring>

namespace mm {

bool ResultBuffer::push(long long jobId, const char* body, size_t len) {
  if (body == nullptr || len == 0) return false;
  held_++;
  if (len > kCapacityBytes) {
    // Nothing would make room for this one, so the buffer keeps what it has.
    dropped_++;
    return false;
  }
  while (count_ == kMaxResults || used_ + len > kCapacityBytes) {
    popOldest();
    dropped_++;
  }
  Entry& e = items_[count_];
  e.jobId = jobId;
  e.offset = used_;
  e.len = len;
  std::memcpy(arena_ + used_, body, len);
  used_ += len;
  count_++;
  return true;
}

const char* ResultBuffer::oldest(long long& jobId, size_t& len) const {
  if (count_ == 0) {
    jobId = 0;
    len = 0;
    return nullptr;
  }
  jobId = items_[0].jobId;
  len = items_[0].len;
  return arena_ + items_[0].offset;
}

void ResultBuffer::popOldest() {
  if (count_ == 0) return;
  const size_t gone = items_[0].len;
  if (used_ > gone) std::memmove(arena_, arena_ + gone, used_ - gone);
  used_ -= gone;
  for (size_t i = 1; i < count_; i++) {
    items_[i - 1] = items_[i];
    items_[i - 1].offset -= gone;
  }
  count_--;
}

void ResultBuffer::clear() {
  count_ = 0;
  used_ = 0;
}

}  // namespace mm
