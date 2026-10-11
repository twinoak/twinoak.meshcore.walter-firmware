// The two views the codec passes around: a run of bytes and a run of text, each a pointer and a
// length and nothing else. core/ has no heap, so a decoded message does not own its variable-length
// fields - it points into the payload it was decoded from, which is the receiver's buffer and is
// valid until the next frame is read. Whoever wants to keep one copies it into its own storage
// (the client, task 02, does).
//
// Not std::string_view or std::span: std::span is C++20 and this is C++17, and a string_view in a
// struct that is meant to read like a specification table says more than it is (no terminator is
// promised, and these are UTF-8 bytes, not characters).
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace link {

struct Bytes {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

// A str8's bytes, or a *rest* of text: UTF-8, no terminator (§2). The codec never decodes it into
// characters and never checks that it is valid UTF-8 - a response is not malformed because a
// stranger's name has a bad byte in it - so this is a run of bytes that happens to be text.
struct Text {
  const char* data = nullptr;
  std::size_t size = 0;
};

constexpr Bytes bytes(const std::uint8_t* data, std::size_t size) { return Bytes{data, size}; }

// The length of a C string, counted here so that the codec needs nothing of <cstring>.
constexpr std::size_t textLength(const char* text) {
  std::size_t n = 0;
  while (text != nullptr && text[n] != 0) {
    ++n;
  }
  return n;
}

// A literal or any terminated string as a Text. The terminator is not part of it (§2: a str8 is a
// length and that many bytes, no terminator).
constexpr Text text(const char* value) { return Text{value, textLength(value)}; }

constexpr Text text(const char* value, std::size_t size) { return Text{value, size}; }

inline bool equal(Bytes a, Bytes b) {
  if (a.size != b.size) {
    return false;
  }
  for (std::size_t i = 0; i < a.size; ++i) {
    if (a.data[i] != b.data[i]) {
      return false;
    }
  }
  return true;
}

inline bool equal(Text a, Text b) {
  if (a.size != b.size) {
    return false;
  }
  for (std::size_t i = 0; i < a.size; ++i) {
    if (a.data[i] != b.data[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace link
}  // namespace core
