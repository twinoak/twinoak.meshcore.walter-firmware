// The radio's identity, in three states and in one place (task p10-03).
//
// Until this file there were two: the radio had a key, or it had not. The second covered two
// cases that need opposite things, and the first unit met both inside a week:
//
//   - **not made yet** - an all-zero key, or no answer to §5.6 at all: a RAK with no
//     application, a slot awaiting its first install after `rak provision`, a unit registered at
//     meshmaster.dk before its repeater. It needs its install, and the unit checks in meanwhile
//     with no key, no radio block and no job types, which is how it says *radio not identified*;
//   - **lost** - the radio *refuses*. From repeater `v0.1.2` a node whose file system would not
//     mount, or whose settings are there and whose key file is not, makes no key: it keeps the
//     link up, answers `IDENTITY` with **`ERR_STATE`**, advertises `caps 0x21` (`CLI` and
//     `ATTENTION` and nothing else), raises `EV_ATTENTION` reason 6 detail 1 once, and waits for
//     a person to type `erase` and `reboot` at its console (the repeater firmware's
//     `tasks/phase-3.9/reports/01.md`, its `docs/flashing.md` §3a and its
//     `src/roles/repeater/app/IdentityGuard.h`). It needs a person. No image can mend it, so an
//     update must not spend a rollback on it, and the heartbeat must not say *awaiting its first
//     install* about it.
//
// The rule, from §5.6's and §5.1's answers and from nothing else: a key that is not all zeros is
// **known**; `ERR_STATE`, an attention of reason 6 detail 1, or a session that advertises exactly
// the lost mode's two capabilities with no key behind it, is **lost**; anything else is **not made
// yet**. One function, so that `agent id`, the heartbeat's counters and the update's health check
// cannot answer the question three ways.
//
// Logic: no ESP-IDF include, no heap, no state.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/wire.h"

namespace core {
namespace link {

// §8's `detail` for an attention of reason 6 (`attention::kFault`) that means a lost identity.
// **The radio firmware's own code**, not ours: `kFaultCodeIdentityLost = 1` in its
// `src/roles/repeater/app/IdentityGuard.h`, tabled in its `docs/link.md` §2c.
constexpr std::uint16_t kFaultIdentityLost = 1;

// What a radio in the lost mode advertises in `HELLO` and in nothing else: `CLI` for the two
// words it knows and `ATTENTION` for the event it raises. Its `IDENTITY`, `TIME_*`, `POWER_GET`,
// `ENV_GET`, the mesh client, the observations and `LOCAL_REQUEST` are all absent, because that
// boot has no node behind them (the repeater's `linkBeginIdentityLost`, its `docs/link.md` §2).
constexpr std::uint32_t kCapsIdentityLost = cap::kCli | cap::kAttention;

enum class IdentityState : std::uint8_t {
  kKnown = 0,       // §5.6 answered 32 bytes that are not all zero
  kNotMadeYet = 1,  // no key yet: a radio awaiting its first install, or one with no application
  kLost = 2,        // the radio refuses: no key was made, and a person is the way out
};

// One phrase each, for a line a person reads.
const char* describe(IdentityState state);

// §5.6's 32 bytes as this firmware reads them: a key, or the all-zero answer of a radio that has
// not made one - which is not a key, and a unit that sent it would be asking the server to know
// it by nothing. False for a null pointer or a length that is not `kPubkeySize`.
bool keyIsReal(const std::uint8_t* key, std::size_t size);

// A session's `caps` are the lost mode's exactly. Not a test for "`CLI` and `ATTENTION` are
// there": a repeater with its key advertises those two and six more.
bool capsSayIdentityLost(std::uint32_t caps);

// What the radio answered, as the three states are decided from it. Every field is a fact off
// the wire: nothing here is a guess about what a radio might be doing.
struct IdentityReading {
  // §5.6 answered OK and `keyIsReal` of its 32 bytes.
  bool key = false;
  // §5.6 was answered with §3's `ERR_STATE` (`status::kState`) - the refusal itself.
  bool refused_state = false;
  // §5.1's `caps` of the session the answer came out of; 0 when there is no session.
  std::uint32_t caps = 0;
  // An `EV_ATTENTION` of reason 6, detail 1, seen in this session (§8). Corroboration, and
  // enough on its own: the radio has said in so many words which fault it has.
  bool attention_identity_lost = false;
};

IdentityState identityState(const IdentityReading& reading);

}  // namespace link
}  // namespace core
