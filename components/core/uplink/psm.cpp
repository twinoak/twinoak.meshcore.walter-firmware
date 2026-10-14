#include "core/uplink/psm.h"

namespace core {
namespace uplink {

namespace {

// The units of each information element, indexed by the three bits. 0 means "not a duration":
// unit 111 is "deactivated" in both, and T3324's reserved units are read as multiples of one
// minute, which is what TS 24.008 §10.5.7.3 says to do with them.
constexpr std::uint32_t kTauUnit[8] = {600, 3600, 36000, 2, 30, 60, 1152000, 0};
constexpr std::uint32_t kActiveUnit[8] = {2, 60, 360, 60, 60, 60, 60, 0};

constexpr std::uint8_t kValueMax = 31;

std::uint32_t secondsOf(const std::uint32_t (&unit)[8], std::uint8_t octet) {
  const std::uint32_t base = unit[(octet >> 5) & 0x7u];
  if (base == 0) return 0;  // deactivated
  return base * static_cast<std::uint32_t>(octet & kValueMax);
}

// The octet closest to `seconds` from below, or the smallest representable duration when
// `seconds` is below all of them. Every unit is tried; a unit whose multiplier would be 0 or
// over 31 cannot carry this duration.
std::uint8_t octetOf(const std::uint32_t (&unit)[8], std::uint32_t seconds,
                     std::uint32_t* actual) {
  std::uint8_t best = 0;
  std::uint32_t best_seconds = 0;
  std::uint32_t smallest = 0;
  std::uint8_t smallest_octet = 0;
  for (std::uint8_t u = 0; u < 8; ++u) {
    const std::uint32_t base = unit[u];
    if (base == 0) continue;
    // The smallest thing this element can say at all: a value of 1 in its smallest unit.
    if (smallest == 0 || base < smallest) {
      smallest = base;
      smallest_octet = static_cast<std::uint8_t>((u << 5) | 1u);
    }
    const std::uint32_t multiplier = seconds / base;
    if (multiplier == 0 || multiplier > kValueMax) continue;
    const std::uint32_t carried = base * multiplier;
    if (carried > best_seconds) {
      best_seconds = carried;
      best = static_cast<std::uint8_t>((u << 5) | (multiplier & kValueMax));
    }
  }
  if (best_seconds == 0) {
    best = smallest_octet;
    best_seconds = smallest;
  }
  if (actual != nullptr) *actual = best_seconds;
  return best;
}

}  // namespace

std::uint32_t tauSeconds(std::uint8_t octet) { return secondsOf(kTauUnit, octet); }

std::uint32_t activeTimeSeconds(std::uint8_t octet) { return secondsOf(kActiveUnit, octet); }

std::uint8_t tauOctet(std::uint32_t seconds, std::uint32_t* actual) {
  return octetOf(kTauUnit, seconds, actual);
}

std::uint8_t activeTimeOctet(std::uint32_t seconds, std::uint32_t* actual) {
  return octetOf(kActiveUnit, seconds, actual);
}

bool parseTimerBits(const char* bits, std::uint8_t* out) {
  if (bits == nullptr || out == nullptr) return false;
  std::uint8_t value = 0;
  for (int i = 0; i < 8; ++i) {
    if (bits[i] != '0' && bits[i] != '1') return false;
    value = static_cast<std::uint8_t>((value << 1) | (bits[i] == '1' ? 1u : 0u));
  }
  if (bits[8] != 0) return false;
  *out = value;
  return true;
}

void timerBits(std::uint8_t octet, char out[9]) {
  for (int i = 0; i < 8; ++i) {
    out[i] = ((octet >> (7 - i)) & 1u) != 0 ? '1' : '0';
  }
  out[8] = 0;
}

}  // namespace uplink
}  // namespace core
