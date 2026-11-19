#include "platform/heap.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_private/esp_psram_extram.h"
#include "esp_psram.h"
#endif

namespace platform {

// The capabilities every figure in this file is about: MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, the
// ones a task stack asks for (freertos/heap_idf.c's portFREERTOS_HEAP_CAPS). **Internal and
// nothing else**, so that a session which has 2 MB of PSRAM in the heap for twenty minutes
// (task p16-05) does not move a single figure this firmware reports or decides on.
constexpr std::uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

// interfaces::Heap: "what is free of the heap, and the least that ever was", the two figures the
// agent's heartbeat carries and the console prints.
//
// Until task p16-05 these were esp_get_free_heap_size() and esp_get_minimum_free_heap_size() -
// "the available heap size, in bytes" and "the minimum heap that has ever been available", over
// MALLOC_CAP_DEFAULT (docs/en/api-reference/system/mem_alloc.rst and esp_system.h, v5.5.5). With
// no PSRAM registered those two are the internal 8-bit heaps and nothing else, which is what the
// figures were always about; a PSRAM pool is registered with MALLOC_CAP_SPIRAM **and**
// MALLOC_CAP_DEFAULT (esp_psram.c, esp_psram_extram_add_to_heap_allocator), so the moment a
// session's room brings it up `esp_get_free_heap_size()` would jump by two megabytes and the
// watermark with it. So they are read at kInternalCaps instead: the same number as before on
// every board, and still the same number during a session. `interfaces/heap.h` says what they are
// about.
//
// The minimum is kept across a software reset by the heap component and is **not** kept across a
// deep sleep, where the heap is built again from nothing - interfaces/heap.h says so, because the
// heartbeat's figure is then this wake's.
std::uint32_t EspHeap::freeBytes() {
  return static_cast<std::uint32_t>(heap_caps_get_free_size(kInternalCaps));
}
std::uint32_t EspHeap::minFreeBytes() {
  return static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kInternalCaps));
}

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
  constexpr std::uint32_t kCaps = kInternalCaps;
  Figures f;
  f.free_bytes = static_cast<std::uint32_t>(heap_caps_get_free_size(kCaps));
  f.largest_block = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(kCaps));
  f.min_free = static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kCaps));
  f.stack_bytes = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
  f.stack_unused = static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  return f;
}

// interfaces::Scratch (task p16-01). MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT: the same
// capabilities EspHeapReport above reports and a task stack asks for, so that what `heap` says
// has in one block is what this can hand out. heap_caps_get_largest_free_block is documented as
// "the largest value of s for which heap_caps_malloc(s, caps) will succeed"
// (esp_heap_caps.h, ESP-IDF v5.5.5), so a caller told it cannot have 8 192 bytes is told how
// much the board does have in one piece.
constexpr std::uint32_t kScratchCaps = kInternalCaps;

void* EspScratch::take(std::size_t bytes) {
  return bytes == 0 ? nullptr : heap_caps_malloc(bytes, kScratchCaps);
}

void EspScratch::give(void* room) {
  if (room != nullptr) {
    heap_caps_free(room);
  }
}

std::size_t EspScratch::largestBlock() {
  return static_cast<std::size_t>(heap_caps_get_largest_free_block(kScratchCaps));
}

// -- the external half: the PSRAM, for the room of a session and nothing else (task p16-05) -----
//
// `sdkconfig.defaults` configures the module's 2 MB of quad PSRAM with **CONFIG_SPIRAM_BOOT_INIT
// off**: the chip's hardware and its MSPI timing are brought up at boot (CONFIG_SPIRAM_BOOT_HW_INIT,
// which has to be early - the timing it sets is shared with the flash), the *memory* is not. No
// page of it is mapped, no pool of it is in the heap, and `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`
// is nought, until something calls esp_psram_init(). That pair of options is ESP-IDF's own
// supported way to delay this - Kconfig.spiram.common's help for
// CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION says so in those words, and
// components/esp_psram/test_apps/psram's `psram_no_boot_init` configuration (BOOT_HW_INIT=y,
// BOOT_INIT=n, PRE_CONFIGURE_MEMORY_PROTECTION=y) is the same three lines with
// esp_psram_init() called from a test case.
//
// Two calls, in this order, because with BOOT_INIT off ESP-IDF makes neither:
//
//   esp_psram_init()                          maps the PSRAM (esp_psram.h: "Initializes the
//                                             PSRAM hardware and ... maps the PSRAM memory";
//                                             ESP_FAIL for "wrong VDDSDIO, invalid chip ID")
//   esp_psram_extram_add_to_heap_allocator()  registers it as a heap with MALLOC_CAP_SPIRAM.
//                                             esp_psram.c's ESP_SYSTEM_INIT_FN(add_psram_to_heap)
//                                             is the boot's own call and is inside
//                                             `#if CONFIG_SPIRAM_BOOT_INIT`.
//
// Once, at the first ask of a boot: a deep sleep is a boot, so there is nothing to undo.
#if CONFIG_SPIRAM
bool EspScratch::bringUp() {
  if (esp_psram_is_initialized()) return true;
  // Asked once in this boot and it did not answer: nothing is gained by asking the MSPI again
  // in the same boot, and `heap` wants to be able to say "none found" rather than "not yet".
  if (asked_) return false;
  asked_ = true;
  if (esp_psram_init() != ESP_OK) return false;
  return esp_psram_extram_add_to_heap_allocator() == ESP_OK;
}

interfaces::Scratch::ExternalRoom EspScratch::external() {
  ExternalRoom room;
  if (esp_psram_is_initialized()) {
    room.state = External::kUp;
    room.bytes = esp_psram_get_size();
    room.free_bytes = static_cast<std::size_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  } else {
    room.state = asked_ ? External::kFailed : External::kAsleep;
  }
  return room;
}

// MALLOC_CAP_SPIRAM and nothing else: external memory asked for **by name**, which is the one
// place in this firmware that may (rail R24). A pool that is not there answers nullptr, and the
// caller takes the small room of the internal heap instead.
void* EspScratch::takeExternal(std::size_t bytes) {
  if (bytes == 0 || !bringUp()) return nullptr;
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}
#else
// A build configured for no external RAM at all. Kept compiling and kept honest rather than
// left out, so that "a board with none holds its sessions in the small room" is one line of
// configuration away from being tried.
bool EspScratch::bringUp() { return false; }

interfaces::Scratch::ExternalRoom EspScratch::external() { return ExternalRoom{}; }

void* EspScratch::takeExternal(std::size_t bytes) {
  (void)bytes;
  return nullptr;
}
#endif  // CONFIG_SPIRAM

}  // namespace platform
