// Writing a payload: the mirror of codec/reader.h. The little-endian scalars of
// spec/LINK-PROTOCOL.md §2 into a caller's buffer, with one rule - a write that does not fit
// fails, nothing is half-written past what the caller is told about, and the failure is sticky.
//
// Nothing here allocates: the buffer belongs to whoever is about to send the frame.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"

namespace core {
namespace link {

class Writer {
 public:
  Writer(std::uint8_t* out, std::size_t size) : out_(out), size_(out == nullptr ? 0 : size) {}

  bool ok() const { return ok_; }
  std::size_t size() const { return ok_ ? at_ : 0; }
  std::size_t room() const { return ok_ ? size_ - at_ : 0; }

  // What an encoder calls when what it was given cannot go on the wire - a password over the 15
  // bytes §6.2 allows, a path of four-byte hashes. Nothing is sent rather than something wrong.
  void fail() { ok_ = false; }

  bool u8(std::uint8_t value) {
    std::uint8_t* at = take(1);
    if (at == nullptr) {
      return false;
    }
    at[0] = value;
    return true;
  }

  bool i8(std::int8_t value) { return u8(static_cast<std::uint8_t>(value)); }

  bool u16(std::uint16_t value) {
    std::uint8_t* at = take(2);
    if (at == nullptr) {
      return false;
    }
    at[0] = static_cast<std::uint8_t>(value & 0xFFu);
    at[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    return true;
  }

  bool i16(std::int16_t value) { return u16(static_cast<std::uint16_t>(value)); }

  bool u32(std::uint32_t value) {
    std::uint8_t* at = take(4);
    if (at == nullptr) {
      return false;
    }
    at[0] = static_cast<std::uint8_t>(value & 0xFFu);
    at[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    at[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    at[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    return true;
  }

  bool span(Bytes value) {
    if (value.size != 0 && value.data == nullptr) {
      ok_ = false;
      return false;
    }
    std::uint8_t* at = take(value.size);
    if (at == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < value.size; ++i) {
      at[i] = value.data[i];
    }
    return true;
  }

  bool text(Text value) {
    return span(Bytes{reinterpret_cast<const std::uint8_t*>(value.data), value.size});
  }

  // A str8: the length and then the bytes (§2). Over 255 bytes there is no length to write.
  bool str8(Text value) {
    if (value.size > 255) {
      ok_ = false;
      return false;
    }
    return u8(static_cast<std::uint8_t>(value.size)) && text(value);
  }

 private:
  std::uint8_t* take(std::size_t n) {
    if (!ok_ || size_ - at_ < n) {  // at_ <= size_ always, so this cannot wrap
      ok_ = false;
      return nullptr;
    }
    std::uint8_t* at = out_ + at_;
    at_ += n;
    return at;
  }

  std::uint8_t* out_;
  std::size_t size_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

}  // namespace link
}  // namespace core
