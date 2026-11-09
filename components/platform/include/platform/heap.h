// interfaces::Heap on ESP-IDF: the two figures esp_system keeps. And, since task p11-02,
// core::console::HeapReport: the four figures of the internal heap that say whether this image
// had room to start (docs/ARCHITECTURE.md §14), which `heap` prints and which `app_main` prints
// one line of at every boot.
#pragma once

#include "core/console/heap_report.h"
#include "interfaces/heap.h"

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

}  // namespace platform
