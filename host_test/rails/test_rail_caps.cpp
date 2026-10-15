// Rail R11 (task p3-06): **the supervisor's daily caps**, which are docs/ARCHITECTURE.md §8's
// promise that everything this firmware does to the radio is slow and bounded - at most so many
// resets, so many power cycles and so many flashes a day, and a cap forgiven only when a whole
// day can be shown to have passed.
//
// It is phase 2 task 05's property test (host_test/core/supervisor/supervisor_month.h), run here
// **over a longer horizon**: a quarter rather than a month, and a second run at another seed, so
// that the window arithmetic is walked across many more windows - and across many more of the
// three ways a day's record can be lost, which the generator does at random inside the run: the
// Walter restarted, slept (the monotonic clock back to zero), and power-cut (RTC memory gone).
//
// The caps are checked after **every single step** inside `runMonth`, at the counts and at the
// wires both; what this file adds is the horizon and the statement that the rail is the suite's.
#include <doctest.h>

#include "core/supervisor/supervisor_month.h"

using bench::Month;
using bench::runMonth;
namespace sup = core::supervisor;

TEST_CASE("R11: a simulated quarter - the daily caps hold at the counts and at the wires") {
  // Ninety days. The caps' window is a day, so this is ninety windows' worth of arithmetic, each
  // of which may be re-anchored by a sleep whose clock restarted or forgotten by a power cut.
  const Month seen = runMonth(306, 90);
  CHECK(seen.climbs > 60);
  CHECK(seen.windows > 5);
  CHECK(seen.incidents > 0);
  CHECK(seen.flashes > 0);
  MESSAGE("seed 306, 90 simulated days: " << seen.climbs << " climbs of the ladder, "
          << seen.windows << " cap windows, " << seen.incidents << " incidents kept, "
          << seen.flashes << " golden-image flashes - the caps ("
          << sup::kResetsPerDay << " resets, " << sup::kPowerCyclesPerDay << " power cycles, "
          << sup::kFlashesPerDay << " flashes a day) held at every step");
}

TEST_CASE("R11: and again at another seed, so the horizon is not one sequence") {
  const Month seen = runMonth(20261015, 60);
  CHECK(seen.climbs > 40);
  CHECK(seen.windows > 3);
  MESSAGE("seed 20261015, 60 simulated days: " << seen.climbs << " climbs, " << seen.windows
          << " cap windows, " << seen.flashes << " golden-image flashes");
}
