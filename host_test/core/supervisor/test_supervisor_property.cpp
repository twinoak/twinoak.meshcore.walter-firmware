// The property (task p2-05): for random sequences of radio behaviour over a simulated month, the
// daily counts never exceed the caps - and what the radio actually got, counted at the pins,
// never exceeds them either. A count that is right while the wires say otherwise would be the
// bug this test exists to catch.
//
// The Walter is restarted, slept and power-cut at random inside the month, because every one of
// those is a way the day's record can be lost, and the caps have to hold across all of them.
// (A Walter that was built again has fresh pin counters, so the per-window comparison against
// the wires begins again there; the counts themselves are checked after every single step.)
#include <doctest.h>

#include "core/supervisor/supervisor_month.h"

using bench::Month;
using bench::runMonth;
namespace sup = core::supervisor;


TEST_CASE("a simulated month of random radio behaviour never exceeds a day's caps") {
  const Month seen = runMonth(1, 30);
  // The test is only worth something if the month actually happened.
  CHECK(seen.climbs > 20);
  CHECK(seen.windows > 1);
  CHECK(seen.incidents > 0);
  CHECK(seen.flashes > 0);
  MESSAGE("seed 1, 30 simulated days: " << seen.climbs << " climbs, " << seen.windows
                                        << " cap windows, " << seen.flashes
                                        << " golden-image flashes");
}

TEST_CASE("a second fortnight, another seed") {
  const Month seen = runMonth(20251014, 14);
  CHECK(seen.climbs > 10);
  CHECK(seen.windows >= 1);
  MESSAGE("seed 20251014, 14 simulated days: " << seen.climbs << " climbs, " << seen.windows
                                               << " cap windows, " << seen.flashes
                                               << " golden-image flashes");
}
