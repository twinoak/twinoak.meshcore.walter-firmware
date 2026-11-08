// The two questions a classification answers, over every value `core::supervisor::Health` has
// (task p9-03). The review of phase 8 found a flag built out of *there is no frame session* being
// handed to both of them, which made a slot running NoiseScope and a slot awaiting its first
// install into dead radios: an apply of theirs lost its link-down and battery-unknown refusals,
// and a self-switch's trial stopped asking anything of the radio. The rule is one function per
// question and lives beside the enumeration, so that the wake cycle and the agent's firmware jobs
// cannot drift apart - and so that a health added later has to be given an answer to both on
// purpose, which the last case here is what holds.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "core/supervisor/supervisor.h"

namespace sup = core::supervisor;

namespace {

// Every value of the enumeration, in its own order. The count check below is why this list is
// written out rather than looped over a range.
constexpr sup::Health kAll[] = {sup::Health::kUnknown, sup::Health::kHealthy, sup::Health::kScanner,
                                sup::Health::kSilent, sup::Health::kAwaitingFirstInstall};
constexpr std::size_t kAllCount = sizeof kAll / sizeof kAll[0];

// The table, at compile time as well: these are what the two update machines are let past, and a
// change to either has to come here first.
static_assert(!sup::linkKnownDown(sup::Health::kUnknown), "nothing classified: both refusals stand");
static_assert(!sup::linkKnownDown(sup::Health::kHealthy), "a radio that answers is not down");
static_assert(!sup::linkKnownDown(sup::Health::kScanner), "a scanner answers in its own way");
static_assert(sup::linkKnownDown(sup::Health::kSilent), "the one classification an apply is let past");
static_assert(!sup::linkKnownDown(sup::Health::kAwaitingFirstInstall),
              "a bootloader with no application is not a radio the ladder gave up on");

static_assert(!sup::radioKnownDown(sup::Health::kUnknown), "a wake that classified nothing waives nothing");
static_assert(!sup::radioKnownDown(sup::Health::kHealthy), "the trial holds it to the HELLO it answered");
static_assert(sup::radioKnownDown(sup::Health::kScanner), "text, not frames: SelfUpdate counts it up anyway");
static_assert(sup::radioKnownDown(sup::Health::kSilent), "a dead radio does not condemn a good Walter image");
static_assert(sup::radioKnownDown(sup::Health::kAwaitingFirstInstall),
              "it cannot answer until it is flashed, and a trial it can never pass is a rollback");

}  // namespace

TEST_CASE("the link is known down for `silent` and for nothing else") {
  CHECK_FALSE(sup::linkKnownDown(sup::Health::kUnknown));
  CHECK_FALSE(sup::linkKnownDown(sup::Health::kHealthy));
  CHECK_FALSE(sup::linkKnownDown(sup::Health::kScanner));
  CHECK(sup::linkKnownDown(sup::Health::kSilent));
  CHECK_FALSE(sup::linkKnownDown(sup::Health::kAwaitingFirstInstall));
}

TEST_CASE("the radio is known not to answer a frame HELLO for the three classified silences") {
  CHECK_FALSE(sup::radioKnownDown(sup::Health::kUnknown));
  CHECK_FALSE(sup::radioKnownDown(sup::Health::kHealthy));
  CHECK(sup::radioKnownDown(sup::Health::kScanner));
  CHECK(sup::radioKnownDown(sup::Health::kSilent));
  CHECK(sup::radioKnownDown(sup::Health::kAwaitingFirstInstall));
}

TEST_CASE("every health has a word and an answer, and the link being down implies the HELLO will not come") {
  // A value added to Health without a line in kAll makes this fail, which is the point: the two
  // answers above are a decision and not a default.
  CHECK(static_cast<std::size_t>(sup::Health::kAwaitingFirstInstall) + 1 == kAllCount);
  for (std::size_t i = 0; i < kAllCount; ++i) {
    const sup::Health health = kAll[i];
    const std::string word = sup::describe(health);
    CHECK_FALSE(word.empty());
    // One is stricter than the other: a link the supervisor has given up on is certainly not
    // going to answer a frame HELLO. The other way round does not hold - a scanner will not
    // answer frames and is still not a radio to flash past.
    const bool implied = !sup::linkKnownDown(health) || sup::radioKnownDown(health);
    CHECK_MESSAGE(implied, word);
  }
}
