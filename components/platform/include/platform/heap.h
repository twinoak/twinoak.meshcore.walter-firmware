// interfaces::Heap on ESP-IDF: the two figures esp_system keeps. And, since task p11-02,
// core::console::HeapReport: the four figures of the internal heap that say whether this image
// had room to start (docs/ARCHITECTURE.md §14), which `heap` prints and which `app_main` prints
// one line of at every boot. And, since task p16-01, interfaces::Scratch: room borrowed for the
// length of one command and handed straight back.
#pragma once

#include <cstddef>

#include "core/console/heap_report.h"
#include "interfaces/heap.h"
#include "interfaces/scratch.h"

namespace platform {

class EspHeap final : public interfaces::Heap {
 public:
  std::uint32_t freeBytes() override;
  std::uint32_t minFreeBytes() override;
};

class EspHeapReport final : public core::console::HeapReport {
 public:
  Figures read() override;
};

// interfaces::Scratch over the internal 8-bit heap (task p16-01): the heap a task stack comes
// out of, which is the one `heap` reports and the one rail R22 is about. `rak say` borrows
// 8 192 bytes of it for the length of the command and hands them back before the prompt, so
// nothing of this is in the image's start-up arrangement.
class EspScratch final : public interfaces::Scratch {
 public:
  void* take(std::size_t bytes) override;
  void give(void* room) override;
  std::size_t largestBlock() override;
};

}  // namespace platform
