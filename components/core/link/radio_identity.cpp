#include "core/link/radio_identity.h"

namespace core {
namespace link {

const char* describe(IdentityState state) {
  switch (state) {
    case IdentityState::kKnown: return "known";
    case IdentityState::kNotMadeYet: return "not made yet";
    case IdentityState::kLost: return "lost: the radio refuses";
  }
  return "?";
}

bool keyIsReal(const std::uint8_t* key, std::size_t size) {
  if (key == nullptr || size != kPubkeySize) return false;
  for (std::size_t i = 0; i < size; ++i) {
    if (key[i] != 0) return true;
  }
  return false;
}

bool capsSayIdentityLost(std::uint32_t caps) { return caps == kCapsIdentityLost; }

IdentityState identityState(const IdentityReading& reading) {
  // A key answered is a key, whatever else the session says: the state is what the radio *has*,
  // and nothing below may take an identified radio's key away from it.
  if (reading.key) return IdentityState::kKnown;
  // The refusal itself, then the radio's own word for which fault it is, then the capability set
  // that only the lost mode advertises - which is what tells a refusal we did not see (one
  // `IDENTITY` lost to a retry, a wake that never asked) from a radio awaiting its first install.
  if (reading.refused_state) return IdentityState::kLost;
  if (reading.attention_identity_lost) return IdentityState::kLost;
  if (capsSayIdentityLost(reading.caps)) return IdentityState::kLost;
  return IdentityState::kNotMadeYet;
}

}  // namespace link
}  // namespace core
