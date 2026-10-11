// Where an image's bytes come from (task p1-05). The flasher reads an image a page at a time,
// more than once, so it takes a source it can ask for any range rather than a pointer to the
// whole: phase 2's image store hands over the bytes from the Walter's own flash partition, the
// tests hand over an array, and `rak flash-test` hands over a pattern that is computed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace flasher {

class ImageSource {
 public:
  virtual ~ImageSource() = default;
  virtual std::uint32_t size() const = 0;
  // `count` bytes from `offset` into `out`; false when the range is not inside the image or the
  // bytes cannot be read (the flasher then stops with the step that asked).
  virtual bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const = 0;
};

class MemoryImage final : public ImageSource {
 public:
  MemoryImage(const std::uint8_t* bytes, std::uint32_t size) : bytes_(bytes), size_(size) {}
  std::uint32_t size() const override { return size_; }
  bool read(std::uint32_t offset, std::uint8_t* out, std::uint32_t count) const override {
    if (bytes_ == nullptr || offset > size_ || count > size_ - offset) return false;
    for (std::uint32_t i = 0; i < count; ++i) out[i] = bytes_[offset + i];
    return true;
  }

 private:
  const std::uint8_t* bytes_;
  std::uint32_t size_;
};

// An image and where it goes: the raw binary, its load address, and the SHA-256 it must have once
// read back (spec/FIRMWARE-MANIFEST.md §2's `sha256` of the `swd` file; nullptr for an image that
// has none to be held to, which only the bench's test pattern is).
struct Image {
  const ImageSource* source = nullptr;
  std::uint32_t load_address = 0;
  const std::uint8_t* sha256 = nullptr;  // 32 bytes, or nullptr
};

}  // namespace flasher
}  // namespace core
