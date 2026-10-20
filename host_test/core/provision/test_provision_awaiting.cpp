// Awaiting its first install (task p5-03): after `rak provision` the radio has a bootloader and no
// application and is silent on the link - exactly what the ladder would answer with a reset, a
// power cycle or a golden image. The slot is marked, the supervisor leaves the radio alone and
// says why, the wake cycle does not count it as silent, a Walter restart and a power cut keep the
// mark, and only a committed `rak update --first-install` clears it - which also shows the
// flasher accepting a radio whose bootloader settings page is erased, as it is after ERASEALL.
#include <doctest.h>

#include <string>

#include "core/provision/provision_bench.h"

using bench::Factory;
using bench::ProvUnit;
using bench::Slot;
using bench::Which;
namespace prov = core::provision;
namespace sup = core::supervisor;
namespace upd = core::update;

TEST_CASE("a provisioned radio left alone across a simulated week of wakes") {
  ProvUnit u(Factory::kImprovedOpen);
  REQUIRE(bench::hasLine(u.runLine("rak provision 0badf00ddeadbeef confirm"), "PROVISIONED"));
  REQUIRE(u.v().supervisor.awaitingFirstInstall());
  const unsigned pulses = u.world.s.reset_pulses_seen;
  const unsigned kills = u.world.s.kill_cycles_seen;
  const unsigned writes = u.chip().counters().writes;
  const unsigned erase_alls = u.chip().counters().erase_alls;
  const std::uint64_t t0 = u.clock().nowMs();
  unsigned wakes = 0;
  while (u.clock().nowMs() - t0 < 7ull * 24 * 60 * 60 * 1000 && wakes < 2000) {
    const core::cycle::Pass& pass = u.wake();
    ++wakes;
    CHECK(pass.radio == sup::Health::kAwaitingFirstInstall);
    CHECK(pass.silence == sup::Silence::kAwaitingFirstInstall);
    CHECK(!pass.climbed);
    CHECK(pass.ending != core::cycle::Ending::kRunning);
  }
  CHECK(wakes >= 7);
  CHECK(u.world.s.reset_pulses_seen == pulses);
  CHECK(u.world.s.kill_cycles_seen == kills);
  CHECK(u.chip().counters().writes == writes);
  CHECK(u.chip().counters().erase_alls == erase_alls);
  CHECK(u.v().supervisor.incidentCount() == 0);
  CHECK(u.v().supervisor.awaitingFirstInstall());
  u.checkChipHoldsBase();
  const std::string watch = u.runLine("rak watch");
  CHECK(bench::hasLine(watch, "awaiting its first install"));
  CHECK(bench::hasLine(watch, "the slot awaits its first install (rak provision, journal entry"));
  CHECK(bench::hasLine(u.runLine("status"), "radio health: awaiting its first install"));
  MESSAGE("a week of " << wakes << " wakes: no reset, no kill, no flash, no incident; the radio " << std::string(sup::describe(sup::Health::kAwaitingFirstInstall)));
}

TEST_CASE("a Walter restart and a power cut keep the mark; reset-caps does not clear it") {
  ProvUnit u(Factory::kLegacyLocked);
  REQUIRE(bench::hasLine(u.runLine("rak provision locked confirm"), "PROVISIONED"));
  const std::uint32_t since = u.v().supervisor.status().awaiting_since_seq;
  CHECK(since != 0);
  u.reboot();
  CHECK(u.v().supervisor.awaitingFirstInstall());
  CHECK(u.v().supervisor.status().awaiting_since_seq == since);
  u.powerCut();  // RTC memory gone: read back from the journal
  CHECK(u.v().supervisor.awaitingFirstInstall());
  CHECK(u.v().supervisor.status().awaiting_since_seq == since);
  u.runLine("rak watch reset-caps");
  CHECK(u.v().supervisor.awaitingFirstInstall());
  // And a wake after all that still leaves the radio alone.
  const core::cycle::Pass& pass = u.wake();
  CHECK(pass.radio == sup::Health::kAwaitingFirstInstall);
  CHECK(!pass.climbed);
}

TEST_CASE("a committed first install clears the mark, and a first install accepts a radio whose settings page is erased") {
  ProvUnit u(Factory::kImprovedOpen);
  u.world.wholeArray();  // provisioned and then given its application: the whole flash
  u.world.putCandidate();
  REQUIRE(bench::hasLine(u.runLine("rak provision 0badf00ddeadbeef confirm"), "PROVISIONED"));
  REQUIRE(u.v().supervisor.awaitingFirstInstall());
  CHECK(u.chip().flashWord(u.world.profile.settings_page) == 0xFFFFFFFF);  // the settings page is erased
  const std::string out = u.runLine("rak update --first-install");
  INFO(out);
  REQUIRE(bench::hasLine(out, "rak update: done:"));
  CHECK(!u.v().supervisor.awaitingFirstInstall());
  CHECK(u.world.s.u.radio_runs);
  // The bootloader would start what is there: a valid record on the settings page now.
  CHECK(u.chip().bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock));
  CHECK(u.chip().flashWord(u.world.profile.settings_page) != 0xFFFFFFFF);
  // The next wake finds a healthy radio and the mark gone from its words.
  const core::cycle::Pass& pass = u.wake();
  CHECK(pass.radio == sup::Health::kHealthy);
  CHECK(!bench::hasLine(u.runLine("rak watch"), "awaits its first install"));
  // The journal tells the story: the mark, the first install's entries, the mark cleared.
  bool marked = false, cleared = false;
  upd::Cursor cursor;
  upd::Entry e;
  bool more = u.v().journal.first(&cursor, &e);
  while (more) {
    sup::Payload p;
    if (sup::decode(e, &p)) {
      if (p.what == sup::Wrote::kAwaitingFirstInstall) marked = true;
      if (p.what == sup::Wrote::kFirstInstallDone) cleared = true;
    }
    more = u.v().journal.previous(&cursor, &e);
  }
  CHECK(marked);
  CHECK(cleared);
  MESSAGE("the first install over an erased settings page: " << u.world.s.u.running_version << " runs, the mark cleared");
}

TEST_CASE("a first install that was committed before the mark does not clear it; one after it, found at a restart, does") {
  ProvUnit u(Factory::kImprovedOpen);
  u.world.wholeArray();
  u.world.putCandidate();
  // A first install first (on a chip the flasher refuses: RUI3 has no SoftDevice structure) -
  // stuck, not done - then a real one after the provision.
  CHECK(bench::hasLine(u.runLine("rak update --first-install"), "STUCK"));
  u.world.s.u.store.store.erase(Which::kCandidate, Slot{0});  // the stuck one is on the bad list
  u.world.putCandidate("update-golden");
  REQUIRE(bench::hasLine(u.runLine("rak provision 0badf00ddeadbeef confirm"), "PROVISIONED"));
  CHECK(u.v().supervisor.awaitingFirstInstall());
  // The supervisor sees the first install commit only when it steps or begins: here at a restart.
  REQUIRE(bench::hasLine(u.runLine("rak update --first-install"), "rak update: done:"));
  u.reboot();
  CHECK(!u.v().supervisor.awaitingFirstInstall());
}

TEST_CASE("a new `rak provision` marks the slot again after a first install cleared it") {
  ProvUnit u(Factory::kImprovedOpen);
  u.world.wholeArray();
  u.world.putCandidate();
  REQUIRE(bench::hasLine(u.runLine("rak provision 0badf00ddeadbeef confirm"), "PROVISIONED"));
  REQUIRE(bench::hasLine(u.runLine("rak update --first-install"), "rak update: done:"));
  CHECK(!u.v().supervisor.awaitingFirstInstall());
  // The radio runs an application now and its port is open: it is named by its DEVICEID still.
  // The link answers HELLO, so the battery is read - and the application is erased with the
  // radio, which is what the typed DEVICEID consents to on an open radio (S-5a).
  const std::string again = u.runLine("rak provision 0badf00ddeadbeef confirm");
  INFO(again);
  REQUIRE(bench::hasLine(again, "PROVISIONED"));
  CHECK(u.v().supervisor.awaitingFirstInstall());
  CHECK(u.chip().counters().erase_alls == 2);
}
