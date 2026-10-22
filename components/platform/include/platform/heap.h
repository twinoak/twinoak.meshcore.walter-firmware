// interfaces::Heap on ESP-IDF: the two figures esp_system keeps.
#pragma once

#include "interfaces/heap.h"

namespace platform {

class EspHeap final : public interfaces::Heap {
 public:
  std::uint32_t freeBytes() override;
  std::uint32_t minFreeBytes() override;
};

}  // namespace platform
