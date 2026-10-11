// Reading a field out of spec/fixtures/link/link-v1.json, bounded and typed, for the tests that
// walk the cases (tasks p1-01 and p1-02). The codec's own test file (test_link_fixtures.cpp) has
// these in its anonymous namespace; this is the same thing where more than one file needs it.
//
// Every read goes through jsonfix::Value::operator[], which remembers that somebody read the key -
// that is what makes the coverage test of the fixtures a property of the tests and not of
// anybody's attention.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/link/codec/view.h"
#include "fixtures/link_fixture.h"

namespace fixtures {

// doctest stringifies a `const char*` variable as a pointer, so a message that should name the
// field would print an address. A std::string prints as its text.
inline std::string named(const char* key) { return std::string(key); }

inline long long intOf(const jsonfix::Value& fields, const char* key) {
  return fields[key].integer();
}

inline std::uint8_t u8(const jsonfix::Value& fields, const char* key) {
  const long long value = intOf(fields, key);
  REQUIRE_MESSAGE(value >= 0, named(key));
  REQUIRE_MESSAGE(value <= 0xFF, named(key));
  return static_cast<std::uint8_t>(value);
}

inline std::uint16_t u16(const jsonfix::Value& fields, const char* key) {
  const long long value = intOf(fields, key);
  REQUIRE_MESSAGE(value >= 0, named(key));
  REQUIRE_MESSAGE(value <= 0xFFFF, named(key));
  return static_cast<std::uint16_t>(value);
}

inline std::uint32_t u32(const jsonfix::Value& fields, const char* key) {
  const long long value = intOf(fields, key);
  REQUIRE_MESSAGE(value >= 0, named(key));
  REQUIRE_MESSAGE(value <= 0xFFFFFFFFll, named(key));
  return static_cast<std::uint32_t>(value);
}

inline std::int8_t i8(const jsonfix::Value& fields, const char* key) {
  const long long value = intOf(fields, key);
  REQUIRE_MESSAGE(value >= -128, named(key));
  REQUIRE_MESSAGE(value <= 127, named(key));
  return static_cast<std::int8_t>(value);
}

inline std::int16_t i16(const jsonfix::Value& fields, const char* key) {
  const long long value = intOf(fields, key);
  REQUIRE_MESSAGE(value >= -32768, named(key));
  REQUIRE_MESSAGE(value <= 32767, named(key));
  return static_cast<std::int16_t>(value);
}

inline std::string textOf(const jsonfix::Value& fields, const char* key) {
  return fields[key].string();
}

inline std::vector<std::uint8_t> hexOf(const jsonfix::Value& fields, const char* key) {
  return fixtures::fromHex(fields[key].string());
}

inline core::link::Bytes viewOf(const std::vector<std::uint8_t>& bytes) {
  return core::link::Bytes{bytes.data(), bytes.size()};
}

inline core::link::Text viewOf(const std::string& text) {
  return core::link::text(text.data(), text.size());
}

inline std::string asString(core::link::Text text) {
  return text.size == 0 ? std::string() : std::string(text.data, text.size);
}

inline std::string asHex(core::link::Bytes bytes) {
  return bytes.size == 0 ? std::string() : fixtures::toHex(bytes.data, bytes.size);
}

// The fixtures' first case of a kind whose `type` is this one, or nullptr. The response a radio
// gives a request of that type: the OK one if the fixtures have it, otherwise whatever they give
// (SEND_TRACE's only response case is its ERR_RANGE refusal) - the same rule
// host_test/sim/scripted_radio.cpp answers by.
inline const jsonfix::Value* responseCaseFor(std::uint8_t type) {
  const jsonfix::Value& all = fixtures::linkFixture()["cases"];
  const jsonfix::Value* fallback = nullptr;
  for (std::size_t i = 0; i < all.size(); ++i) {
    const jsonfix::Value& one = all.at(i);
    if (one["kind"].string() != "response" ||
        static_cast<std::uint8_t>(one["type"].integer()) != type) {
      continue;
    }
    if (one["fields"]["status"].integer() == 0) {
      return &one;
    }
    fallback = &one;
  }
  return fallback;
}

}  // namespace fixtures
