// Rails R10 and R14 (task p3-06):
//
//   * **never both updates in one wake; never an update on an unconfirmed Walter image** -
//     spec/SYSTEM.md S-7 and S-6, asserted on the **journal** of a simulated month in which the
//     backend asks for both updates at every single wake, which is the worst thing a server can
//     do to this rail;
//   * **no mesh request is sent by this firmware** - spec/SYSTEM.md §8 and
//     tasks/phase-3/README.md: this pile ends at a seam, the mesh client's calls belong to the
//     agent, and the agent is not here.
//
// The journal is where this has to be read, and not only the Pass: a wake has one update step, so
// "one step a wake" is true by the shape of the code, but what the two machines *wrote down* is
// what the next boot resumes. So the month's journal is walked, cut into wakes at each
// `kind::kWakeCycle` record, and no wake's slice may carry an entry of both update kinds.
//
// The grep half of R14 - that the twelve mesh request types are named nowhere above
// components/core/link/ - is tools/check_rails.py.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/cycle/wake_bench.h"

using bench::WakeBench;
using core::cycle::Ending;
using core::cycle::Outcome;
using core::cycle::Pass;
using core::cycle::Step;

namespace {

namespace upd = core::update;

struct Logged {
  std::uint32_t seq;
  std::uint16_t kind;
};

// Is this (ask, why) among the refusals the telemetry ring kept for the next exchange? S-7's own
// refusal is `kOneUpdateOnly`, and the ring is where the wake writes it down so that a backend
// which still wants the thing learns why it did not happen (the same reading
// host_test/core/cycle/test_wake_backend.cpp does).
bool refused(bench::WakeBench& b, interfaces::Ask ask, interfaces::Refusal why) {
  const core::cycle::TelemetryRing& ring = b.v().readings;
  for (std::size_t i = 0; i < ring.refusals(); ++i) {
    const interfaces::BackendRefusal r = ring.refusalAt(i);
    const bool same = r.ask == static_cast<std::uint8_t>(ask) &&
                      r.why == static_cast<std::uint8_t>(why);
    if (same) return true;
  }
  return false;
}

// Every entry the journal holds, oldest first.
std::vector<Logged> entries(upd::Journal& journal) {
  std::vector<Logged> found;
  upd::Cursor cursor;
  upd::Entry e;
  bool more = journal.first(&cursor, &e);
  while (more && found.size() < 2048) {
    found.push_back(Logged{e.seq, e.kind});
    more = journal.previous(&cursor, &e);
  }
  std::vector<Logged> oldest_first(found.rbegin(), found.rend());
  return oldest_first;
}

// The journal cut into wakes: a slice ends at each wake record, which is the last thing a wake
// writes (core/cycle/sleep.h: the journal, then SLEEP_NOTICE, then the modem down, then sleep).
struct Slice {
  unsigned radio_updates = 0;
  unsigned self_updates = 0;
  unsigned supervisor = 0;
};

std::vector<Slice> wakes(const std::vector<Logged>& all) {
  std::vector<Slice> found;
  Slice current;
  for (const Logged& e : all) {
    if (e.kind == upd::kind::kRadioUpdate) current.radio_updates++;
    if (e.kind == upd::kind::kSelfUpdate) current.self_updates++;
    if (e.kind == upd::kind::kSupervisor) current.supervisor++;
    if (e.kind == upd::kind::kWakeCycle) {
      found.push_back(current);
      current = Slice{};
    }
  }
  if (current.radio_updates + current.self_updates + current.supervisor != 0) {
    found.push_back(current);  // a wake whose record was not news enough to journal
  }
  return found;
}

}  // namespace

TEST_CASE("R10: a month of wakes asking for both updates - never both in one wake") {
  WakeBench bench;
  bench.modemWorks();

  // The store has no candidate and this bench has no simulated nRF52, so neither update can
  // actually run here - the radio's is refused by its own preconditions and the Walter's own for
  // want of an image in the other slot. **That is not what this test is about.** What it is about
  // is that S-7 is applied *before* either of those: the Walter's own goes first when both are
  // asked and the radio's is refused with `kOneUpdateOnly`, every wake, and written down where
  // the next exchange will carry it; that a wake carries at most one update ask; and that no wake
  // of the month leaves both machines' entries in the journal.
  unsigned both_asked = 0;   // wakes in which the backend was reached and asked for both
  unsigned no_exchange = 0;  // wakes with no exchange: nothing was asked, so nothing was refused
  unsigned carried_radio = 0;
  unsigned carried_self = 0;
  unsigned refused_steps = 0;
  unsigned s7_refusals = 0;   // ... naming S-7 itself (kOneUpdateOnly)
  unsigned band_refusals = 0; // ... naming the band, which refuses both before S-7 is reached
  unsigned unrefused = 0;     // ... naming nothing: the rail would be silent, and must not be
  const int kWakes = 400;
  for (int i = 0; i < kWakes; ++i) {
    bench.world.rak.battery_mv = static_cast<std::uint16_t>(4100 - (i % 10) * 20);
    bench.world.backend.forget();
    bench.world.backend.askFor(interfaces::Ask::kUpdateRadio);
    bench.world.backend.askFor(interfaces::Ask::kUpdateSelf);  // both, every wake

    const Pass& pass = bench.oneWake();
    REQUIRE_MESSAGE(pass.ending == Ending::kSlept, i);
    // One carried ask, and it is one of the two or nothing - never a wake that did both.
    const interfaces::Ask carried = pass.carried;
    const bool one_or_none = carried == interfaces::Ask::kNothing ||
                             carried == interfaces::Ask::kUpdateRadio ||
                             carried == interfaces::Ask::kUpdateSelf;
    CHECK_MESSAGE(one_or_none, i);
    if (carried == interfaces::Ask::kUpdateRadio) ++carried_radio;
    if (carried == interfaces::Ask::kUpdateSelf) ++carried_self;
    if (pass.outcomeOf(Step::kUpdate) == Outcome::kRefused) ++refused_steps;
    // **S-7's own refusal**, written down where the next exchange will carry it: the Walter's own
    // update goes first when both are asked, and the radio's is refused with `kOneUpdateOnly`.
    // Counted only over the wakes that *reached* the backend: a wake whose uplink did not come up
    // (the backoff, and this month has one) asked for nothing, so there was nothing to refuse -
    // and a successful exchange is what clears the ring, so the two have to be counted together
    // or the number means nothing.
    const bool asked_both =
        (pass.asks & interfaces::askBit(interfaces::Ask::kUpdateRadio)) != 0 &&
        (pass.asks & interfaces::askBit(interfaces::Ask::kUpdateSelf)) != 0;
    if (!asked_both) {
      ++no_exchange;
    } else {
      ++both_asked;
      // One of the two was refused, and the refusal names why. `kOneUpdateOnly` is S-7 itself;
      // `kBandForbids` is the battery refusing both before S-7 is reached, which is also "never
      // both" and is also written down. What must never happen is **neither**: a wake that was
      // asked for two updates, did at most one, and said nothing about the other.
      const bool by_s7 =
          refused(bench, interfaces::Ask::kUpdateRadio, interfaces::Refusal::kOneUpdateOnly) ||
          refused(bench, interfaces::Ask::kUpdateSelf, interfaces::Refusal::kOneUpdateOnly);
      const bool by_band =
          refused(bench, interfaces::Ask::kUpdateRadio, interfaces::Refusal::kBandForbids) ||
          refused(bench, interfaces::Ask::kUpdateSelf, interfaces::Refusal::kBandForbids);
      if (by_s7) ++s7_refusals;
      if (by_band && !by_s7) ++band_refusals;
      const bool kept = by_s7 || by_band;
      if (!kept) ++unrefused;
      CHECK_MESSAGE(kept, "wake " << i << " was asked for both updates and kept no refusal of "
                    "either: the rail would be silent");
    }
    // Nothing was skipped silently.
    for (std::size_t s = 0; s < core::cycle::kStepCount; ++s) {
      CHECK_MESSAGE(pass.step[s].outcome != Outcome::kNotRun, i);
    }
  }

  // -- the journal, cut into wakes --------------------------------------------------------------
  const std::vector<Logged> all = entries(bench.v().journal);
  const std::vector<Slice> slices = wakes(all);
  unsigned wake_records = 0;
  unsigned radio_entries = 0;
  unsigned self_entries = 0;
  unsigned both_in_one_wake = 0;
  for (const Logged& e : all) {
    if (e.kind == upd::kind::kWakeCycle) ++wake_records;
    if (e.kind == upd::kind::kRadioUpdate) ++radio_entries;
    if (e.kind == upd::kind::kSelfUpdate) ++self_entries;
  }
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const bool both = slices[i].radio_updates > 0 && slices[i].self_updates > 0;
    if (both) ++both_in_one_wake;
    CHECK_MESSAGE(!both, "wake slice " << i << " of the journal carries " << slices[i].radio_updates
                  << " radio-update entries and " << slices[i].self_updates
                  << " of the Walter's own: S-7 says never both at once");
  }
  CHECK(both_in_one_wake == 0);
  CHECK(wake_records > 0);
  // Every wake that was asked for both kept a refusal of one of them, and most of them named
  // S-7 itself: a month in which nothing was refused would be a month that did not test the rail.
  CHECK(unrefused == 0);
  CHECK(s7_refusals + band_refusals == both_asked);
  CHECK(s7_refusals > static_cast<unsigned>(kWakes) / 2);
  CHECK(both_asked + no_exchange == static_cast<unsigned>(kWakes));
  // The machines are idle at the end: nothing was left half-journalled.
  CHECK(bench.v().update.state() == core::update::State::kIdle);
  CHECK_FALSE(bench.v().self.onTrial());

  MESSAGE("a month of wakes with both updates asked for at every one: " << kWakes << " wakes, "
          << both_asked << " of which reached the backend and asked for both (" << no_exchange
          << " had no exchange: the uplink's backoff); " << carried_radio << " carried the radio's update, " << carried_self
          << " the Walter's own, " << refused_steps << " refused the step, " << s7_refusals
          << " kept S-7's own refusal (kOneUpdateOnly) for the next exchange and " << band_refusals
          << " the band's, which refuses both before S-7 is reached (" << unrefused
          << " kept neither); the journal holds " << all.size() << " entries ("
          << wake_records << " wake records, " << radio_entries << " of the radio's update, "
          << self_entries << " of the Walter's own) in " << slices.size()
          << " wake slices, none of which carries both kinds");
}

TEST_CASE("R10: no radio update while this firmware's own image is unconfirmed") {
  // The other slot was made the next to boot and the board restarted into it: the bootloader
  // marks it pending-verify, so this wake is a trial (host_test/fakes/fake_own_update.h).
  WakeBench bench;
  bench.modemWorks();
  bench.world.own.slot(1).has_image = true;
  bench.world.own.slot(1).version = "0.4.0+trial00";
  bench.world.own.slot(1).seq = 99;
  bench.world.own.slot(1).state = interfaces::OwnImageState::kNew;
  bench.world.own.reboot();
  REQUIRE(bench.world.own.runningIndex() == 1);

  bench.world.backend.askFor(interfaces::Ask::kUpdateRadio);  // the server asks all the same
  bench.boot(interfaces::WokeBy::kNotFromSleep, core::health::ResetReason::kSoftware,
             core::health::Restart::kSelfUpdateSwitch);
  bench.v().begin();
  REQUIRE(bench.v().self.onTrial());
  const Pass& pass = bench.v().run();

  // The one update step of this wake finished the trial; the radio's was not begun.
  CHECK(pass.on_trial);
  CHECK(pass.carried == interfaces::Ask::kUpdateSelf);
  CHECK(bench.v().update.state() == core::update::State::kIdle);
  // And the journal says so: this wake wrote the Walter's own entries and none of the radio's.
  const std::vector<Slice> slices = wakes(entries(bench.v().journal));
  unsigned radio_entries = 0;
  for (const Slice& s : slices) radio_entries += s.radio_updates;
  CHECK(radio_entries == 0);
  CHECK(bench.v().self.status().state == core::update::SelfState::kConfirmed);
  CHECK(pass.ending == Ending::kSlept);
}

TEST_CASE("R14: over a month of wakes, no mesh request is ever sent") {
  // The same world, driven hard: a radio that is sometimes a scanner, a backend asking for
  // everything in the closed set, a battery that sinks. Nothing of it needs the mesh, and
  // nothing of it may touch the mesh - the agent is not in these piles (spec/SYSTEM.md §8).
  WakeBench bench;
  bench.modemWorks();
  const interfaces::Ask kAsks[] = {interfaces::Ask::kFetchRelease, interfaces::Ask::kUpdateRadio,
                                   interfaces::Ask::kUpdateSelf, interfaces::Ask::kRunLadder};
  for (int i = 0; i < 300; ++i) {
    bench.world.rak.battery_mv = static_cast<std::uint16_t>(4150 - (i % 30) * 25);
    bench.world.rak.deaf = (i % 17 == 0);
    bench.world.rak.scanner = (i % 23 == 0);
    bench.world.backend.forget();
    bench.world.backend.askFor(kAsks[i % 4]);
    if (i % 5 == 0) bench.world.backend.takeReadings();
    const Pass& pass = bench.oneWake(i % 7 == 0 ? interfaces::WokeBy::kRadioLine
                                                : interfaces::WokeBy::kTimer);
    CHECK_MESSAGE(pass.ending == Ending::kSlept, i);
  }

  unsigned mesh = 0;
  unsigned observations = 0;
  const std::vector<std::uint8_t> types = bench.requestTypes();
  for (std::uint8_t type : types) {
    const std::uint32_t needs = core::link::capabilityOf(type);
    if (needs == core::link::cap::kMeshClient) ++mesh;
    if (needs == core::link::cap::kObservations) ++observations;
  }
  CHECK(mesh == 0);
  CHECK(observations == 0);
  // And this is not a test that passed by asking the radio nothing at all.
  CHECK(types.size() > 300);
  MESSAGE("300 wakes, every ask of the closed set in turn: " << types.size()
          << " requests to the radio, 0 of them a mesh request and 0 an observation pull");
}
