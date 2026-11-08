// Hex, both ways, for the fixtures that are written as hex - the link's cases
// (fixtures/link_fixture.h) and the manifest's keys and digests (fixtures/manifest_files.h).
//
// It is one header because the two used to have a copy each, in the same namespace, and nothing
// included both until task p9-06 wanted the link's 1.2 answers on the agent's wake bench
// (sim/local_door.h); the second include was then a redefinition of each. One definition, and the
// two cannot drift apart.
#pragma once

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fixtures {

inline std::vector<std::uint8_t> fromHex(const std::string& hex) {
  REQUIRE_MESSAGE(hex.size() % 2 == 0, "a hex string of an odd length: " << hex);
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

inline std::string toHex(const std::uint8_t* data, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}

inline std::string toHex(const std::vector<std::uint8_t>& bytes) {
  return toHex(bytes.data(), bytes.size());
}

}  // namespace fixtures
