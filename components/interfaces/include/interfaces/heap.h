// What is free of the heap, and the least that ever was (task p4-04). Two numbers, reported and
// never decided on: the agent's heartbeat carries them to the server and the console prints
// them, and nothing in this firmware refuses to do something because of them.
//
// This firmware's own code allocates nothing (every `no_allocation` check in host_test/), so
// what these two numbers watch is the one part that does: the copied library's reply parsers,
// each of which holds an ArduinoJson document that allocates while it parses and frees when the
// call returns (components/agent-core, docs/library.md §6). A minimum that keeps falling across
// a week of wakes is the thing worth seeing, which is why the *least* is here beside the free.
//
// On the ESP32-S3 it is `esp_get_free_heap_size()` and `esp_get_minimum_free_heap_size()`
// (platform/heap); host_test/ gives numbers a test hands over.
#pragma once

#include <cstdint>

namespace interfaces {

class Heap {
 public:
  virtual ~Heap() = default;

  virtual std::uint32_t freeBytes() = 0;
  // The smallest `freeBytes()` has been since this boot. A deep sleep is a boot.
  virtual std::uint32_t minFreeBytes() = 0;
};

}  // namespace interfaces
