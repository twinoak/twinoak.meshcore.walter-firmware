// interfaces::Scratch on the host (task p16-01, extended by task p16-05): room a test hands over,
// a heap that can be made to have none, and an external memory a test scripts as absent, as
// present, as failing to come up or as **wrong**.
//
// It is `new`/`delete` here on purpose - a host test is not the firmware and may allocate - and
// it counts what it handed out, so a test can show that `rak say` gives back exactly what it
// took and that nothing is still out when the command is over.
//
// -- the external memory, and how a test makes one that lies -------------------------------------
//
// Three of the four states need nothing clever: `noExternal()`, `externalOf(bytes)` and
// `externalWillNotComeUp()` are flags. The fourth - a chip that comes up and does not hold what
// is written to it - cannot be a flag, because what reads it back is the firmware's own pattern
// inside one call. So `externalHasADeadAddressLine()` hands out a block that genuinely aliases
// itself: two mappings of one memfd, the upper half of the room being the lower half, which is
// what a dead address line on a PSRAM looks like. The pattern of
// `core::update::patternHolds()` is a value of each word's own index for exactly this failure.
//
// That is POSIX (`memfd_create`, `mmap`), and the host tests of this repository are built and run
// in the pinned ESP-IDF image, which is Linux (tools/verify.sh). Nothing else in the fake needs
// it.
#pragma once

#include <doctest.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

#include "interfaces/scratch.h"

namespace fakes {

class FakeScratch final : public interfaces::Scratch {
 public:
  ~FakeScratch() override { undoTheAliasedRoom(); }

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
    if (room == aliased_) {
      undoTheAliasedRoom();
      return;
    }
    if (room == external_room_) {
      external_room_ = nullptr;
      external_out_ = 0;
    }
    delete[] static_cast<std::uint8_t*>(room);
  }

  std::size_t largestBlock() override { return largest_; }

  // Read only: a test that calls this must see the same three states `heap` prints.
  ExternalRoom external() override {
    ++external_reads;
    ExternalRoom room;
    if (external_bytes_ == 0) {
      room.state = External::kNone;
      return room;
    }
    if (!brought_up_) {
      room.state = asked_ ? External::kFailed : External::kAsleep;
      return room;
    }
    room.state = External::kUp;
    room.bytes = external_bytes_;
    room.free_bytes = external_bytes_ - external_out_;
    return room;
  }

  void* takeExternal(std::size_t bytes) override {
    ++external_asks;
    if (bytes == 0 || external_bytes_ == 0) {
      ++external_refusals;
      return nullptr;
    }
    // The bring-up, once a boot, exactly as the board's is: a chip that did not answer is not
    // asked again in the same boot.
    if (!brought_up_) {
      if (asked_) {
        ++external_refusals;
        return nullptr;
      }
      ++bring_ups;
      asked_ = true;
      if (!comes_up_) {
        ++external_refusals;
        return nullptr;
      }
      brought_up_ = true;
    }
    if (bytes > external_bytes_ - external_out_) {
      ++external_refusals;
      return nullptr;
    }
    ++out;
    external_out_ += bytes;
    external_last_asked = bytes;
    if (dead_address_line_) {
      external_room_ = nullptr;
      return theAliasedRoom(bytes);
    }
    external_room_ = new std::uint8_t[bytes];
    return external_room_;
  }

  // -- what a test scripts ----------------------------------------------------------------------

  // The largest block the internal heap has in one piece. `onlyHas(0)` is a board with nothing
  // to lend.
  void onlyHas(std::size_t bytes) { largest_ = bytes; }

  // A board with no external memory at all - the default, so that every test written before task
  // p16-05 is a board with none and still means what it meant.
  void noExternal() {
    external_bytes_ = 0;
    comes_up_ = true;
    dead_address_line_ = false;
  }

  // A board with `bytes` of external memory that comes up and holds what is written to it. 2 MB
  // is the N16R2's own figure.
  void externalOf(std::size_t bytes) {
    external_bytes_ = bytes;
    comes_up_ = true;
    dead_address_line_ = false;
  }

  // There is some, and it does not come up: the 8 MB octal variant driven in quad mode, a chip
  // that has stopped answering, a board whose PSRAM was never fitted.
  void externalWillNotComeUp(std::size_t bytes = 2u * 1024u * 1024u) {
    external_bytes_ = bytes;
    comes_up_ = false;
    dead_address_line_ = false;
  }

  // It comes up, and the upper half of every room it gives out is the lower half of it.
  void externalHasADeadAddressLine(std::size_t bytes = 2u * 1024u * 1024u) {
    external_bytes_ = bytes;
    comes_up_ = true;
    dead_address_line_ = true;
  }

  // A deep sleep is a boot (interfaces/scratch.h): the chip is down again and nothing has asked.
  void asAfterABoot() {
    brought_up_ = false;
    asked_ = false;
    external_out_ = 0;
  }

  int asks = 0;
  int refusals = 0;
  int gives = 0;
  int out = 0;  // blocks handed out and not yet given back, of either memory
  std::size_t last_asked = 0;

  int external_asks = 0;
  int external_refusals = 0;
  int external_reads = 0;
  int bring_ups = 0;  // how many times anything has asked this board to bring its PSRAM up
  std::size_t external_last_asked = 0;

 private:
  // Two mappings of one half-length memfd, back to back: writing the upper half writes the lower.
  void* theAliasedRoom(std::size_t bytes) {
    undoTheAliasedRoom();
    const std::size_t half = (bytes / 2 + 4095u) & ~static_cast<std::size_t>(4095u);
    const int fd = memfd_create("fake-psram", 0);
    REQUIRE(fd >= 0);
    REQUIRE(ftruncate(fd, static_cast<off_t>(half)) == 0);
    void* base = mmap(nullptr, half * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(base != MAP_FAILED);
    char* at = static_cast<char*>(base);
    REQUIRE(mmap(at, half, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED);
    REQUIRE(mmap(at + half, half, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) !=
            MAP_FAILED);
    close(fd);
    aliased_ = base;
    aliased_bytes_ = half * 2;
    return base;
  }

  void undoTheAliasedRoom() {
    if (aliased_ == nullptr) return;
    munmap(aliased_, aliased_bytes_);
    aliased_ = nullptr;
    aliased_bytes_ = 0;
    external_out_ = 0;
  }

  std::size_t largest_ = 64u * 1024u;

  std::size_t external_bytes_ = 0;  // 0: this board has none
  std::size_t external_out_ = 0;
  bool comes_up_ = true;
  bool dead_address_line_ = false;
  bool brought_up_ = false;
  bool asked_ = false;
  std::uint8_t* external_room_ = nullptr;
  void* aliased_ = nullptr;
  std::size_t aliased_bytes_ = 0;
};

}  // namespace fakes
