// interfaces::Heap on the host (task p4-04): two numbers a test hands over, and the figure a
// board would never show - the free heap *below* the minimum ever free, which is what a
// platform that reported the two from different places would look like.
#pragma once

#include <cstdint>

#include "interfaces/heap.h"

namespace fakes {

class FakeHeap final : public interfaces::Heap {
 public:
  std::uint32_t freeBytes() override {
    ++reads_;
    return free_;
  }
  std::uint32_t minFreeBytes() override {
    ++reads_;
    return min_free_;
  }

  // The two, as a board would keep them: a free figure, and the least it has been.
  void say(std::uint32_t free_bytes, std::uint32_t min_free_bytes) {
    free_ = free_bytes;
    min_free_ = min_free_bytes;
  }
  std::uint32_t reads() const { return reads_; }

 private:
  std::uint32_t free_ = 180000;
  std::uint32_t min_free_ = 160000;
  std::uint32_t reads_ = 0;
};

}  // namespace fakes
