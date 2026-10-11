// Retained storage as a map: survives as long as the object does, which is "across resets" in a
// test that constructs the core objects again over it. powerCut() clears it.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "interfaces/retained.h"

namespace fakes {

class FakeRetained final : public interfaces::Retained {
 public:
  int stores = 0;

  bool load(interfaces::RetainedSlot slot, void* out, std::size_t size) override {
    const auto it = records_.find(slot);
    if (it == records_.end() || it->second.size() != size) return false;
    std::memcpy(out, it->second.data(), size);
    return true;
  }

  bool store(interfaces::RetainedSlot slot, const void* data, std::size_t size) override {
    if (size > interfaces::kRetainedRecordMax) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    records_[slot] = std::vector<std::uint8_t>(bytes, bytes + size);
    stores++;
    return true;
  }

  void powerCut() { records_.clear(); }
  bool has(interfaces::RetainedSlot slot) const { return records_.count(slot) != 0; }

  // Corrupts a record's size, as a firmware with another layout would leave it.
  void truncate(interfaces::RetainedSlot slot) {
    auto it = records_.find(slot);
    if (it != records_.end() && !it->second.empty()) it->second.pop_back();
  }

 private:
  std::map<interfaces::RetainedSlot, std::vector<std::uint8_t>> records_;
};

}  // namespace fakes
