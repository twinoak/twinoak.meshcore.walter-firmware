// The heap behind its seam (task p11-02), on the host: four numbers a test hands over, and a
// count of how often the console asked.
//
// The real one (components/platform/heap.cpp) is three `heap_caps_*` calls and
// `uxTaskGetStackHighWaterMark`, none of which exists here. What *can* be tested here is what the
// console does with them - the shape of the two lines, the sentence that says which figure
// decides an image, and that a build handed no seam says so instead of printing zeros.
#pragma once

#include <cstdint>

#include "core/console/heap_report.h"

namespace fakes {

class FakeHeapReport final : public core::console::HeapReport {
 public:
  // A board that started with room. The first two are what this firmware really read at its own
  // `heap:` line, booted in the QEMU of the pinned image (task p11-02's report): 33 872 bytes
  // free and a largest free block of 18 432, which is what is left of the region the ROM's stack
  // was in. The other three are a run that has gone on a while: a least-free below the free, and
  // a main task that has used about half of its 16 KB. Five different numbers, so that a test can
  // tell which line printed which.
  Figures figures{33'872, 18'432, 28'104, 16'384, 7'560};
  std::uint32_t reads = 0;

  Figures read() override {
    ++reads;
    return figures;
  }
};

}  // namespace fakes
