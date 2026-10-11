// Reading a payload: the little-endian scalars of spec/LINK-PROTOCOL.md §2 over a byte span,
// with one rule - a read that would go past the end fails and leaves everything alone.
//
// The failure is sticky, so a decoder can read a whole message and ask once at the end whether it
// was there (codec/messages.cpp does). That is what keeps "say malformed instead of reading past
// the end" from being a check the author of each message has to remember.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/view.h"

namespace core {
namespace link {

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(data == nullptr ? 0 : size) {}
  explicit Reader(Bytes span) : Reader(span.data, span.size) {}

  // False once any read has failed. Nothing read after that is meaningful.
  bool ok() const { return ok_; }
  std::size_t remaining() const { return ok_ ? size_ - at_ : 0; }
  std::size_t position() const { return at_; }

  // What a decoder calls when a value it read is not a legal one: the same outcome as running off
  // the end, because the payload is malformed either way (§3, ERR_MALFORMED).
  void fail() { ok_ = false; }

  bool u8(std::uint8_t* out) {
    const std::uint8_t* at = take(1);
    if (at == nullptr) {
      return false;
    }
    *out = at[0];
    return true;
  }

  bool i8(std::int8_t* out) {
    std::uint8_t raw = 0;
    if (!u8(&raw)) {
      return false;
    }
    *out = static_cast<std::int8_t>(raw);  // two's complement (§2)
    return true;
  }

  bool u16(std::uint16_t* out) {
    const std::uint8_t* at = take(2);
    if (at == nullptr) {
      return false;
    }
    *out = static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
    return true;
  }

  bool i16(std::int16_t* out) {
    std::uint16_t raw = 0;
    if (!u16(&raw)) {
      return false;
    }
    *out = static_cast<std::int16_t>(raw);
    return true;
  }

  bool u32(std::uint32_t* out) {
    const std::uint8_t* at = take(4);
    if (at == nullptr) {
      return false;
    }
    *out = static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
    return true;
  }

  // `n` bytes as a view into the payload. No copy: core/ has no heap (codec/view.h).
  bool span(std::size_t n, Bytes* out) {
    const std::uint8_t* at = take(n);
    if (at == nullptr) {
      return false;
    }
    *out = Bytes{at, n};
    return true;
  }

  // A str8: a u8 length and that many bytes of UTF-8, no terminator (§2).
  bool str8(Text* out) {
    std::uint8_t length = 0;
    if (!u8(&length)) {
      return false;
    }
    const std::uint8_t* at = take(length);
    if (at == nullptr) {
      return false;
    }
    *out = Text{reinterpret_cast<const char*>(at), length};
    return true;
  }

  // *rest*: every remaining byte of the payload (§2). Empty is a legal rest.
  Bytes rest() {
    if (!ok_ || at_ == size_) {
      at_ = size_;
      return Bytes{};  // an empty rest, and no pointer arithmetic on a payload that may be null
    }
    const Bytes all{data_ + at_, size_ - at_};
    at_ = size_;
    return all;
  }

  Text restText() {
    const Bytes all = rest();
    return Text{reinterpret_cast<const char*>(all.data), all.size};
  }

 private:
  // The only place a bound is checked, and the only place at_ moves.
  const std::uint8_t* take(std::size_t n) {
    if (!ok_ || size_ - at_ < n) {  // at_ <= size_ always, so this cannot wrap
      ok_ = false;
      return nullptr;
    }
    const std::uint8_t* at = data_ + at_;
    at_ += n;
    return at;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

}  // namespace link
}  // namespace core
