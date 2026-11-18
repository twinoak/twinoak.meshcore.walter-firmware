// interfaces::Scratch on the host (task p16-01): room a test hands over, and a heap that can be
// made to have none.
//
// It is `new`/`delete` here on purpose - a host test is not the firmware and may allocate - and
// it counts what it handed out, so a test can show that `rak say` gives back exactly what it
// took and that nothing is still out when the command is over.
#pragma once

#include <doctest.h>

#include <cstddef>
#include <cstdint>

#include "interfaces/scratch.h"

namespace fakes {

class FakeScratch final : public interfaces::Scratch {
 public:
  void* take(std::size_t bytes) override {
    ++asks;
    if (bytes == 0 || bytes > largest_) {
      ++refusals;
      return nullptr;
    }
    ++out;
    last_asked = bytes;
    return new std::uint8_t[bytes];
  }

  void give(void* room) override {
    if (room == nullptr) return;
    REQUIRE(out > 0);
    --out;
    ++gives;
    delete[] static_cast<std::uint8_t*>(room);
  }

  std::size_t largestBlock() override { return largest_; }

  // The largest block this heap has in one piece. `onlyHas(0)` is a board with nothing to lend.
  void onlyHas(std::size_t bytes) { largest_ = bytes; }

  int asks = 0;
  int refusals = 0;
  int gives = 0;
  int out = 0;  // blocks handed out and not yet given back
  std::size_t last_asked = 0;

 private:
  std::size_t largest_ = 64u * 1024u;
};

}  // namespace fakes
