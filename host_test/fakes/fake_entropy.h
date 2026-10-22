// interfaces::Entropy on the host (task p4-04): xorshift32, seeded, so that two runs of a test
// draw the same numbers and a failure is the same failure twice. The agent library's own
// FakePlatform uses the same generator with the same default seed, for the same reason.
#pragma once

#include <cstdint>

#include "interfaces/entropy.h"

namespace fakes {

class FakeEntropy final : public interfaces::Entropy {
 public:
  explicit FakeEntropy(std::uint32_t seed = 0x5eed1234u) : state_(seed) {}

  std::uint32_t bits32() override {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    ++draws_;
    return state_;
  }

  std::uint32_t draws() const { return draws_; }
  void reseed(std::uint32_t seed) { state_ = seed; }

 private:
  std::uint32_t state_;
  std::uint32_t draws_ = 0;
};

}  // namespace fakes
