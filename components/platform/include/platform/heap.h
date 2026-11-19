// interfaces::Heap on ESP-IDF: the two figures esp_system keeps. And, since task p11-02,
// core::console::HeapReport: the four figures of the internal heap that say whether this image
// had room to start (docs/ARCHITECTURE.md §14), which `heap` prints and which `app_main` prints
// one line of at every boot. And, since task p16-01, interfaces::Scratch: room borrowed for the
// length of one command and handed straight back - out of the internal heap, or out of the
// module's PSRAM when a session asks for the big room (task p16-05).
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

// interfaces::Scratch over the internal 8-bit heap (task p16-01) and - for the room of a session
// and nothing else (task p16-05) - over the module's PSRAM. The internal half is the heap a task
// stack comes out of, which is the one `heap` reports and the one rail R22 is about: `rak say`
// borrows 8 192 bytes of it for the length of the command and hands them back before the prompt,
// so nothing of this is in the image's start-up arrangement.
//
// The external half brings the PSRAM up at the first `takeExternal()` of a boot - the mapping and
// the heap pool, which `CONFIG_SPIRAM_BOOT_INIT=n` leaves to the application - and at no other
// moment. One board's PSRAM that does not come up is one board's sessions in the small room, and
// nothing else in this firmware notices.
class EspScratch final : public interfaces::Scratch {
 public:
  void* take(std::size_t bytes) override;
  void give(void* room) override;
  std::size_t largestBlock() override;
  ExternalRoom external() override;
  void* takeExternal(std::size_t bytes) override;

 private:
  // esp_psram_init() and esp_psram_extram_add_to_heap_allocator(), once, at the first ask of a
  // boot. True when the PSRAM is mapped and its pool is in the heap.
  bool bringUp();

  // Whether this boot has asked for the PSRAM. `esp_psram_is_initialized()` says whether it came
  // up but not whether anything tried, and `heap` has to tell "nothing has asked yet" from "it
  // was asked and none answered".
  bool asked_ = false;
};

}  // namespace platform
