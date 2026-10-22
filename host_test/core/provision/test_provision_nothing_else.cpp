// Nothing else reaches the door (task p5-03; spec/SYSTEM.md S-5a: "Nothing automatic leads
// there: not the supervisor, not an update, not the agent, not a job from the server"). The
// simulated chip's tripwire stays armed - no test here calls allowProvisioning() - and a factory
// RUI3 radio is given to the supervisor's ladder, to the radio's update and to a simulated week
// of whole wakes. The tripwire never fires, the chip is never erased, and the ladder still ends at
// *stop* for a radio it cannot bring back.
#include <doctest.h>

#include <string>

#include "core/provision/provision_bench.h"

using bench::Candidate;
using bench::Factory;
using bench::ProvUnit;
using bench::Slot;
using bench::StoreError;
using bench::Which;
namespace sup = core::supervisor;

namespace {

void checkDoorNeverOpened(ProvUnit& u) {
  CHECK(u.chip().counters().tripwire == 0);
  CHECK(u.chip().counters().erase_alls == 0);
  CHECK(u.chip().counters().uicr_writes == 0);
  CHECK(u.chip().errors().empty());
  CHECK(u.chip().uicrWord(0x10001014) == 0x000F4000);  // NRFFW[0] as RAK left it
}

}  // namespace

TEST_CASE("the supervisor's ladder against a factory RUI3 radio ends at stop, and the door never opened") {
  for (const Factory f : {Factory::kLegacyOpen, Factory::kImprovedLocked}) {
    ProvUnit u(f, false);
    // A golden image in the store, as a unit in the field has: rung 4 finds the chip differs
    // and asks the update machine for the golden image back - which the flasher refuses, since
    // RUI3 has no SoftDevice information structure at 0x3000 (or the port is closed).
    REQUIRE(u.world.s.u.store.put(Which::kGolden, Slot{0}, Candidate("update-golden")) == StoreError::kNone);
    const std::string out = u.runLine("rak ladder");
    INFO(out);
    CHECK(bench::hasLine(out, "STOPPED"));
    CHECK(u.v().supervisor.status().stopped);
    CHECK(u.world.s.reset_pulses_seen >= 1);
    CHECK(u.world.s.kill_cycles_seen >= 1);
    checkDoorNeverOpened(u);
    CHECK(u.chip().counters().writes == 0);
    CHECK(u.chip().counters().erases == 0);
    CHECK(!u.v().supervisor.awaitingFirstInstall());
    MESSAGE(std::string(bench::describe(f)) << ": the ladder ended " << std::string(sup::describe(u.v().supervisor.status().stop_reason))
                               << "; tripwire " << u.chip().counters().tripwire);
  }
}

TEST_CASE("the radio's update against a factory RUI3 radio: refused or stuck, and the door never opened") {
  ProvUnit u(Factory::kLegacyOpen, false);
  u.world.putCandidate();
  // No golden image: an update is refused at check. A first install is a person's command and
  // the flasher refuses the chip (no SoftDevice where the profile says): stuck, nothing erased.
  std::string out = u.runLine("rak update");
  CHECK(bench::hasLine(out, "refused at check"));
  out = u.runLine("rak update --first-install");
  INFO(out);
  CHECK(bench::hasLine(out, "STUCK"));
  checkDoorNeverOpened(u);
  CHECK(u.chip().counters().writes == 0);
  CHECK(u.chip().counters().erases == 0);
}

TEST_CASE("a simulated week of whole wakes against a factory RUI3 radio: the ladder inside its caps, the door never opened") {
  ProvUnit u(Factory::kImprovedLocked, false);
  REQUIRE(u.world.s.u.store.put(Which::kGolden, Slot{0}, Candidate("update-golden")) == StoreError::kNone);
  u.world.putCandidate();
  const std::uint64_t t0 = u.clock().nowMs();
  unsigned wakes = 0;
  unsigned climbs = 0;
  while (u.clock().nowMs() - t0 < 7ull * 24 * 60 * 60 * 1000 && wakes < 2000) {
    const core::cycle::Pass& pass = u.wake();
    ++wakes;
    if (pass.climbed) ++climbs;
    CHECK(pass.ending != core::cycle::Ending::kRunning);
  }
  CHECK(wakes >= 7);
  checkDoorNeverOpened(u);
  CHECK(u.chip().counters().writes == 0);
  CHECK(u.chip().counters().erases == 0);
  CHECK(u.world.s.kill_cycles_seen <= 7 * sup::kPowerCyclesPerDay + 1);
  CHECK(!u.v().supervisor.awaitingFirstInstall());
  MESSAGE("a week: " << wakes << " wakes, " << climbs << " climbs, " << u.world.s.reset_pulses_seen << " reset pulses, "
                     << u.world.s.kill_cycles_seen << " kill cycles; tripwire " << u.chip().counters().tripwire
                     << ", chip erases " << u.chip().counters().erase_alls);
}
