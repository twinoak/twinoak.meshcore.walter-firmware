// Retained storage behind the seam, on a host (task p1.6-22): `mm::IRetained` over a vector.
//
// What a store is to the library is three things - a capacity, one image written whole, that
// image read back - and this is those three with a test's hands on them: the capacity is set,
// the image can be torn short or have a byte flipped, a write can be made to fail, and every
// write and read is counted, which is what "a wake writes at most one image, and none when
// nothing changed" is asserted against. The bytes themselves are readable as text, which is how
// `TheImageHoldsNothingAResultDoesNot` greps an image for a password.
//
// A power cut is `clear()`; a sleep is nothing at all - the vector is the flash, and it stays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <agent/IRetained.h>

namespace fake {

class FakeRetained : public mm::IRetained {
 public:
  explicit FakeRetained(size_t capacity = 32768) : capacity_(capacity) {}

  // ------------------------------------------------------------------ IRetained

  size_t capacity() const override { return capacity_; }

  bool write(const uint8_t* image, size_t len) override {
    writes_++;
    if (refuse_ || image == nullptr || len > capacity_) return false;
    image_.assign(image, image + len);
    return true;
  }

  size_t read(uint8_t* out, size_t cap) override {
    reads_++;
    const size_t n = image_.size() < cap ? image_.size() : cap;
    if (n > 0) std::memcpy(out, image_.data(), n);
    return n;
  }

  // ------------------------------------------------------------------ what a test drives

  void setCapacity(size_t bytes) { capacity_ = bytes; }
  void refuseWrites(bool refuse) { refuse_ = refuse; }
  // A power cut mid-write, or an erased region: the image is gone.
  void clear() { image_.clear(); }
  // A torn write: only the first `keep` bytes made it.
  void tear(size_t keep) {
    if (keep < image_.size()) image_.resize(keep);
  }
  // One bit of the image, the wrong way round.
  void flip(size_t at) {
    if (at < image_.size()) image_[at] ^= 0x01;
  }
  void set(std::vector<uint8_t> bytes) { image_ = std::move(bytes); }

  const std::vector<uint8_t>& image() const { return image_; }
  std::string text() const { return std::string(image_.begin(), image_.end()); }
  int writes() const { return writes_; }
  int reads() const { return reads_; }
  void forgetCounts() {
    writes_ = 0;
    reads_ = 0;
  }

 private:
  size_t capacity_;
  bool refuse_ = false;
  std::vector<uint8_t> image_;
  int writes_ = 0;
  int reads_ = 0;
};

}  // namespace fake
