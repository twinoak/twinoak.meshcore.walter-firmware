// The settings store as a map (task p3-03): what stands behind interfaces::KeyValue on the host.
// It survives as long as the object does, which is "across a reboot" in a test that builds the
// core objects again over it - and `powerCut()` is a store that was never written.
//
// What a test can make it do, because each of these is a unit in the field:
//
//   * `notReady()` - a `nvs` partition that is missing or would not open: every key falls back;
//   * `put()` - a value nobody of this firmware wrote: a store from another firmware, a value a
//     person set with `nvs_set_str`, a flipped bit. Settings::begin() must drop it and say so;
//   * `readOnly()` - a `set` the flash refuses, so the console must not answer `ok`;
//   * `refuseErase()` - an `erase confirm` that cannot finish.
#pragma once

#include <cstring>
#include <map>
#include <string>

#include "interfaces/key_value.h"

namespace fakes {

class SimKeyValue final : public interfaces::KeyValue {
 public:
  int writes = 0;
  int erases = 0;

  bool ready() const override { return ready_; }

  bool read(const char* name, char* out, std::size_t cap) override {
    if (out == nullptr || cap == 0) return false;
    out[0] = 0;
    if (!ready_ || name == nullptr) return false;
    const auto it = values_.find(name);
    if (it == values_.end()) return false;
    if (it->second.size() + 1 > cap) return false;  // NVS's own answer: it does not fit
    std::memcpy(out, it->second.c_str(), it->second.size() + 1);
    return true;
  }

  bool write(const char* name, const char* value) override {
    if (!ready_ || read_only_ || name == nullptr || value == nullptr) return false;
    values_[name] = value;
    writes++;
    return true;
  }

  bool erase(const char* name) override {
    if (!ready_ || read_only_ || refuse_erase_ || name == nullptr) return false;
    values_.erase(name);
    erases++;
    return true;
  }

  // -- what a test does to it ------------------------------------------------------------------

  void notReady() { ready_ = false; }
  void readOnly(bool on = true) { read_only_ = on; }
  void refuseErase(bool on = true) { refuse_erase_ = on; }
  void powerCut() { values_.clear(); }

  // A value put there by something that is not this firmware's Settings.
  void put(const char* name, const char* value) { values_[name] = value; }
  bool has(const char* name) const { return values_.count(name) != 0; }
  std::string raw(const char* name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? std::string() : it->second;
  }
  std::size_t size() const { return values_.size(); }

 private:
  std::map<std::string, std::string> values_;
  bool ready_ = true;
  bool read_only_ = false;
  bool refuse_erase_ = false;
};

}  // namespace fakes
