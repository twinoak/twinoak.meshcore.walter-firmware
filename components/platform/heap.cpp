#include "platform/heap.h"

#include "esp_system.h"

namespace platform {

// esp_get_free_heap_size() and esp_get_minimum_free_heap_size(): "the available heap size, in
// bytes" and "the minimum heap that has ever been available", both over every capability
// (docs/en/api-reference/system/mem_alloc.rst and esp_system.h, v5.5.5). The minimum is kept
// across a software reset by the heap component and is **not** kept across a deep sleep, where
// the heap is built again from nothing - interfaces/heap.h says so, because the heartbeat's
// figure is then this wake's.
std::uint32_t EspHeap::freeBytes() { return esp_get_free_heap_size(); }
std::uint32_t EspHeap::minFreeBytes() { return esp_get_minimum_free_heap_size(); }

}  // namespace platform
