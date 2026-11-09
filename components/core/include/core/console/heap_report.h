// The console's one seam into the heap as the chip keeps it (task p11-02): what `heap` prints,
// and the one line `app_main` prints at boot.
//
// Why these four figures and not the two of `interfaces/heap.h`. That interface is the agent's
// heartbeat - "the available heap size" over every capability, and the least it has ever been -
// and it is a figure about a running unit. This one is about **the arrangement that decides
// whether an image starts at all** (docs/ARCHITECTURE.md §14): v0.1.3 could not create its main
// task because no heap region had a free block of 16 384 bytes, and the figure that would have
// shown it is the largest free *block* of the internal heap, which no interface of this firmware
// reported. So this seam reports, for `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` - the capabilities
// a task stack asks for (`freertos/heap_idf.c`) - the free, the largest free block, the least
// free since this boot, and the main task's own stack.
//
// It decides nothing. Nothing in this firmware refuses to do something because of these numbers;
// what refuses is `tools/heap_at_start.py`, at build time, over the build's map and DWARF, and
// these are the board's own account of what the model predicted (`docs/bench.md` §1).
//
// The implementation is `platform::EspHeapReport` (three `heap_caps_*` calls and
// `uxTaskGetStackHighWaterMark`), because core/ may include nothing of ESP-IDF (rail R12). A
// build that hands over none - every host test but one - says so in one line.
#pragma once

#include <cstdint>

namespace core {
namespace console {

class HeapReport {
 public:
  virtual ~HeapReport() = default;

  // All of it read in one call, so that the four numbers are of one moment.
  struct Figures {
    // heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT): every registered heap that
    // has both capabilities, the 22 308-byte region the ROM's stack was in included - it is
    // registered at the top of `main_task`, so by the time anything of ours can read this it is
    // part of the figure and it was *not* part of the heap the main task's stack came from.
    std::uint32_t free = 0;
    // heap_caps_get_largest_free_block(same): the one that decides whether the next big thing
    // fits. A free of 33 kB in two heaps is no use to a 20 kB object.
    std::uint32_t largest_block = 0;
    // heap_caps_get_minimum_free_size(same): the least `free` has been since this boot. A deep
    // sleep is a boot - the heap is built again from nothing.
    std::uint32_t min_free = 0;
    // CONFIG_ESP_MAIN_TASK_STACK_SIZE, and uxTaskGetStackHighWaterMark of the task that is
    // asking: on a board that is the main task, because this console runs in `app_main`. ESP-IDF
    // returns that mark in **bytes** (not words, as vanilla FreeRTOS does) - the smallest the
    // free stack has ever been. 16 384 of DRAM is the biggest single thing this firmware asks
    // for, so what it never used is the first place to look if it ever has to come down.
    std::uint32_t stack_bytes = 0;
    std::uint32_t stack_unused = 0;
  };

  virtual Figures read() = 0;
};

}  // namespace console
}  // namespace core
