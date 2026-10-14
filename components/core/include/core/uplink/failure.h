// The failure taxonomy of the uplink (task p3-01; docs/ARCHITECTURE.md §9: "a taxonomy of
// failures ... with a backoff for each"). Five named ways of not getting onto the network, each
// with what it means, what the console says, and when this firmware may try again - as an enum
// and a table, so that a sixth kind of silence has to be given a name and a backoff before any
// code can report it.
//
// The table is the whole of the policy's "when": core/uplink/policy.cpp reads it and nothing
// else decides. A unit with no coverage that searched at every wake would be flat in a week,
// and a unit that gave up for ever on the first refusal would need a person on a mast; the two
// mistakes are what the `Retry` column is there to keep apart.
//
//   | failure         | means                                        | try again                |
//   |-----------------|----------------------------------------------|--------------------------|
//   | modem-silent    | no answer to the first command               | after a modem reset,     |
//   |                 |                                              | once per wake            |
//   | no-sim          | no card, or it does not answer               | next wake; not sooner    |
//   | sim-locked      | a PIN is wanted and none is set, or the one  | never automatically      |
//   |                 | set was refused                              |                          |
//   | not-registered  | searching, denied, or no network within the  | growing: next wake, then |
//   |                 | deadline                                     | every 2nd, 4th... capped |
//   | no-data         | registered, no packet data                   | as not-registered        |
//
// `no-time` is not in it: the task's table has it, and says of it "not a failure of the uplink;
// reported beside it". It is interfaces::TimeOrigin and core::uplink::Result::time, and an
// uplink that came up without a time is up.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace uplink {

// The stored numbers are the journal's and the retained record's, so they do not move.
enum class Failure : std::uint8_t {
  kNone = 0,            // there was no failure: the uplink is up
  kModemSilent = 1,
  kNoSim = 2,
  kSimLocked = 3,
  kNotRegistered = 4,
  kNoData = 5,
};
constexpr std::size_t kFailureCount = 5;

// When this firmware may try again after one of them.
enum class Retry : std::uint8_t {
  kNotNeeded = 0,       // kNone
  kAfterModemReset = 1, // once more this wake, after the modem has been reset; then next wake
  kNextWake = 2,        // the next wake and not sooner: a person has to act for it to change
  kNever = 3,           // never automatically: trying again costs something that cannot be undone
  kGrowing = 4,         // next wake, then every second, fourth ... up to kBackoffCapWakes
};

const char* describe(Retry retry);

// One row of the table. `name` is the word the console and the report use - the task's own
// spelling, hyphens and all - and `means` is the sentence beside it.
struct Row {
  Failure failure;
  const char* name;
  const char* means;
  Retry retry;
};

// The table itself, in the enum's order, indexable by the enum's value.
const Row& row(Failure failure);

// The word: "modem-silent", "no-sim", ... "none".
const char* describe(Failure failure);

// The whole line a console prints for a failure: the word, then what it means.
const char* means(Failure failure);

// -- the backoff ---------------------------------------------------------------------------------
//
// Counted in **wakes**, not in seconds. A Walter has no clock it can trust across a deep sleep
// (interfaces/clock.h: esp_timer restarts at zero on waking) and no time of day at all until
// this very task has given it one, so "every fourth wake" is a thing it can count and "in four
// hours" is not. How long a wake is apart is the duty cycle's business (task p3-04), and that is
// exactly where the battery's own answer belongs.
//
// The growth is a doubling of the *interval between attempts*: after the first failure every
// wake, after the second every second wake, after the third every fourth, and so on until the
// cap. At the duty cycle's longest interval the cap is most of a day, which is as long as a unit
// may sit unreachable without somebody being told.

// The longest interval a growing backoff reaches, in wakes. TO BE SET ON THE BENCH together with
// the duty cycle's interval table (task p3-04): the pair of them is "how long may a unit with no
// coverage stay dark", and only the measured cost of a wake says what that may be.
constexpr std::uint32_t kBackoffCapWakes = 16;

// How many wakes there are between attempts after `failures` consecutive failures: 1 is "every
// wake", 2 "every second wake", and 0 is "never again by itself". Pure arithmetic, so that the
// table, the policy and the test all read the same thing.
//
//   kNotNeeded, kAfterModemReset, kNextWake   1, always
//   kNever                                    0, always
//   kGrowing      failures 0 or 1 -> 1, 2 -> 2, 3 -> 4, 4 -> 8 ... capped at kBackoffCapWakes
std::uint32_t intervalAfter(Retry retry, std::uint32_t failures);

}  // namespace uplink
}  // namespace core
