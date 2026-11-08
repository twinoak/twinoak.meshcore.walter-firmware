// The radio's identity in three states (task p10-03): `core::link::identityState` and the two
// readings beside it, held to the sentences of core/link/radio_identity.h.
//
// Why this file is small and the table is whole: this is the **one place** that decides which of
// the three a radio is in, and `agent id`, the heartbeat's counters and the update's health check
// all read it. Every row of the table is here, and so is the property that matters for the first
// unit of 6 November: *a radio that answered a key is never anything but known*.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "core/link/radio_identity.h"

namespace link = core::link;

namespace {

link::IdentityReading reading(bool key, bool refused_state, std::uint32_t caps,
                              bool attention = false) {
  link::IdentityReading r;
  r.key = key;
  r.refused_state = refused_state;
  r.caps = caps;
  r.attention_identity_lost = attention;
  return r;
}

// What a repeater with its key advertises (the bench's radio, and the first unit's at v0.1.1):
// everything but LOCAL_REQUEST, which link 1.2 added.
constexpr std::uint32_t kHealthyCaps = link::cap::kCli | link::cap::kPower | link::cap::kEnv |
                                       link::cap::kMeshClient | link::cap::kObservations |
                                       link::cap::kAttention | link::cap::kTimePersist;

}  // namespace

TEST_CASE("the three states, every row of the table") {
  SUBCASE("a key is known, and nothing else in the answer can take that away") {
    CHECK(link::identityState(reading(true, false, kHealthyCaps)) == link::IdentityState::kKnown);
    // The one that matters: a radio that answered a key while the session happened to carry the
    // lost mode's caps is still a radio with a key. The key is what the server knows it by.
    CHECK(link::identityState(reading(true, false, link::kCapsIdentityLost)) ==
          link::IdentityState::kKnown);
    CHECK(link::identityState(reading(true, true, link::kCapsIdentityLost, true)) ==
          link::IdentityState::kKnown);
  }

  SUBCASE("no key and nothing else said: not made yet - a radio awaiting its first install") {
    CHECK(link::identityState(reading(false, false, 0)) == link::IdentityState::kNotMadeYet);
    CHECK(link::identityState(reading(false, false, kHealthyCaps)) ==
          link::IdentityState::kNotMadeYet);
  }

  SUBCASE("ERR_STATE is the refusal itself: lost") {
    CHECK(link::identityState(reading(false, true, 0)) == link::IdentityState::kLost);
    CHECK(link::identityState(reading(false, true, kHealthyCaps)) == link::IdentityState::kLost);
  }

  SUBCASE("the attention the radio raises about it: reason 6, detail 1, is lost on its own") {
    CHECK(link::identityState(reading(false, false, 0, true)) == link::IdentityState::kLost);
    CHECK(link::kFaultIdentityLost == 1u);
    // §8's reason the detail belongs to, which this firmware already had a name for.
    CHECK(link::attention::kFault == 6u);
  }

  SUBCASE("caps 0x21 with no key behind it: lost - an answer this end did not see") {
    CHECK(link::identityState(reading(false, false, link::kCapsIdentityLost)) ==
          link::IdentityState::kLost);
  }
}

TEST_CASE("caps 0x21 is the lost mode's own, and not 'CLI and ATTENTION are there'") {
  CHECK(link::kCapsIdentityLost == 0x21u);
  CHECK(link::capsSayIdentityLost(0x21u));
  // A repeater with its key has those two bits and six more, and is not in the lost mode.
  CHECK_FALSE(link::capsSayIdentityLost(kHealthyCaps));
  CHECK_FALSE(link::capsSayIdentityLost(0xffu));
  CHECK_FALSE(link::capsSayIdentityLost(0u));
  // One bit more than the lost mode's is a build this firmware has not met, and not the mode.
  CHECK_FALSE(link::capsSayIdentityLost(0x21u | link::cap::kLocalRequest));
}

TEST_CASE("a key of all zeros is not a key") {
  std::uint8_t key[link::kPubkeySize] = {};
  CHECK_FALSE(link::keyIsReal(key, sizeof key));
  key[link::kPubkeySize - 1] = 1;
  CHECK(link::keyIsReal(key, sizeof key));
  // Not 32 bytes, or no bytes at all: §5.6 says 32 and a body this end half understood is no key.
  CHECK_FALSE(link::keyIsReal(key, sizeof key - 1));
  CHECK_FALSE(link::keyIsReal(nullptr, sizeof key));
  CHECK_FALSE(link::keyIsReal(key, 0));
}

TEST_CASE("each state has a phrase of its own") {
  const std::string known = link::describe(link::IdentityState::kKnown);
  const std::string not_yet = link::describe(link::IdentityState::kNotMadeYet);
  const std::string lost = link::describe(link::IdentityState::kLost);
  CHECK(known == "known");
  CHECK(not_yet == "not made yet");
  CHECK(lost.find("refuses") != std::string::npos);
  const bool all_different = known != not_yet;
  CHECK(all_different);
  CHECK(not_yet != lost);
}
