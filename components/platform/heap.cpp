#include "platform/heap.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace platform {

// esp_get_free_heap_size() and esp_get_minimum_free_heap_size(): "the available heap size, in
// bytes" and "the minimum heap that has ever been available", both over every capability
// (docs/en/api-reference/system/mem_alloc.rst and esp_system.h, v5.5.5). The minimum is kept
// across a software reset by the heap component and is **not** kept across a deep sleep, where
// the heap is built again from nothing - interfaces/heap.h says so, because the heartbeat's
// figure is then this wake's.
std::uint32_t EspHeap::freeBytes() { return esp_get_free_heap_size(); }
std::uint32_t EspHeap::minFreeBytes() { return esp_get_minimum_free_heap_size(); }

// The four figures of core/console/heap_report.h (task p11-02). The capabilities are the ones a
// task stack asks for - MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT (freertos/heap_idf.c's
// portFREERTOS_HEAP_CAPS) - so that what is printed is about the same heaps the main task's stack
// came out of, and not about a total that includes memory no stack could have used.
//
//   heap_caps_get_free_size          "Amount of free bytes in the regions"
//   heap_caps_get_largest_free_block "the largest value of s for which heap_caps_malloc(s, caps)
//                                     will succeed"
//   heap_caps_get_minimum_free_size  the regions' low watermarks added up ("a worst case
//                                     indication for all-time minimum free heap"; heaps added at
//                                     run time are not in it, and this firmware adds none)
//
// all three from esp_heap_caps.h (ESP-IDF v5.5.5). The stack's mark is
// uxTaskGetStackHighWaterMark(NULL) - the calling task's, which on this board is the main task
// because everything of ours runs in app_main - and it is the smallest the free stack has ever
// been, in **bytes**: FreeRTOS counts it in StackType_t units and on Xtensa that type is
// `uint8_t` (freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h,
// `#define portSTACK_TYPE uint8_t`).
core::console::HeapReport::Figures EspHeapReport::read() {
  constexpr std::uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Figures f;
  f.free = static_cast<std::uint32_t>(heap_caps_get_free_size(kCaps));
  f.largest_block = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(kCaps));
  f.min_free = static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kCaps));
  f.stack_bytes = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
  f.stack_unused = static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  return f;
}

}  // namespace platform
