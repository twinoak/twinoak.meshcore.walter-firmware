// The readings kept while there is nobody to give them to (task p3-05): a week without an
// uplink fills the ring, drops the oldest and counts; the uplink comes back and everything kept
// is offered once and removed only when the backend says it took it; and a restart in between
// loses nothing that was stored.
#include <doctest.h>

#include <string>

#include "core/cycle/telemetry_ring.h"
#include "core/cycle/wake_bench.h"
#include "fakes/fake_retained.h"

using bench::WakeBench;
using core::cycle::kReadingsKept;
using core::cycle::Taken;
using core::cycle::TelemetryRing;
using core::cycle::Step;
using interfaces::Ask;
using interfaces::Refusal;

namespace {

Taken reading(std::uint32_t epoch, std::uint16_t mv) {
  Taken t{};
  t.time_known = epoch != 0;
  t.at_epoch = epoch;
  t.battery_known = true;
  t.millivolts = mv;
  t.current_known = true;
  t.current_ma = -40;
  return t;
}

}  // namespace

TEST_CASE("the ring keeps the newest, drops the oldest and counts what it dropped") {
  fakes::FakeRetained retained;
  TelemetryRing ring{retained};
  ring.begin();
  CHECK(ring.count() == 0);
  CHECK(ring.dropped() == 0);

  // A week of quarter-hourly wakes with nobody to give them to: 672 readings.
  for (std::uint32_t i = 0; i < 672; ++i) {
    ring.add(reading(1760000000u + i * 900u, static_cast<std::uint16_t>(3900 + (i % 7))));
  }
  CHECK(ring.count() == kReadingsKept);
  CHECK(ring.dropped() == 672 - kReadingsKept);
  // What is held is the **newest** ten, oldest first.
  for (std::size_t i = 0; i < kReadingsKept; ++i) {
    const std::uint32_t which = 672 - kReadingsKept + static_cast<std::uint32_t>(i);
    CHECK(ring.at(i).at_epoch == 1760000000u + which * 900u);
  }
  // And an index past the end is a zeroed reading rather than anything of the ring's.
  CHECK(ring.at(kReadingsKept).at_epoch == 0);
}

TEST_CASE("a reading taken before the clock was known is marked as such, never guessed") {
  fakes::FakeRetained retained;
  TelemetryRing ring{retained};
  ring.begin();
  ring.add(reading(0, 3800));             // no time of day
  ring.add(reading(1760000000u, 3810));   // and then the network gave one

  CHECK_FALSE(ring.at(0).time_known);
  CHECK(ring.at(0).at_epoch == 0);
  CHECK(ring.at(1).time_known);
  CHECK(ring.at(1).at_epoch == 1760000000u);

  // And what crosses the seam says the same thing: the order in the array is the only ordering
  // there is for the first of them.
  interfaces::BackendKnowledge known{};
  const std::size_t offered = ring.offerTo(&known);
  CHECK(offered == 2);
  CHECK_FALSE(known.reading[0].time_known);
  CHECK(known.reading[0].at_epoch == 0);
  CHECK(known.reading[1].time_known);
}

TEST_CASE("the ring survives a restart and is emptied only when the backend took them") {
  fakes::FakeRetained retained;
  {
    TelemetryRing ring{retained};
    ring.begin();
    for (int i = 0; i < 4; ++i) ring.add(reading(1760000000u + i, 3900));
    CHECK(ring.count() == 4);
  }
  // A restart of the Walter: the object is gone, RTC memory is not.
  {
    TelemetryRing ring{retained};
    ring.begin();
    CHECK(ring.survived());
    CHECK(ring.count() == 4);
    CHECK(ring.at(0).at_epoch == 1760000000u);
    CHECK(ring.at(3).millivolts == 3900);
    // Offered, and not taken: nothing is forgotten.
    interfaces::BackendKnowledge known{};
    CHECK(ring.offerTo(&known) == 4);
    CHECK(ring.count() == 4);
    // Offered and taken: the four that were offered go, and nothing else.
    ring.add(reading(1760000099u, 3950));  // a reading taken after the offer
    ring.wereTaken(4);
    CHECK(ring.count() == 1);
    CHECK(ring.at(0).at_epoch == 1760000099u);
    CHECK(ring.taken() == 4);
  }
  // And a power cut is what loses them, which is the honest limit of RTC memory.
  retained.powerCut();
  {
    TelemetryRing ring{retained};
    ring.begin();
    CHECK_FALSE(ring.survived());
    CHECK(ring.count() == 0);
  }
}

TEST_CASE("the refusals cross the sleep, are not repeated, and are forgotten when they are told") {
  fakes::FakeRetained retained;
  {
    TelemetryRing ring{retained};
    ring.begin();
    ring.refused(Ask::kUpdateRadio, Refusal::kBandForbids);
    ring.refused(Ask::kUpdateRadio, Refusal::kBandForbids);  // the same one, again
    CHECK(ring.refusals() == 1);
    ring.refused(Ask::kSetSettings, Refusal::kSecretKey);
    CHECK(ring.refusals() == 2);
  }
  {
    TelemetryRing ring{retained};
    ring.begin();
    CHECK(ring.refusals() == 2);
    CHECK(ring.refusalAt(0).ask == static_cast<std::uint8_t>(Ask::kUpdateRadio));
    CHECK(ring.refusalAt(1).why == static_cast<std::uint8_t>(Refusal::kSecretKey));
    ring.refusalsTold();
    CHECK(ring.refusals() == 0);
  }
  {
    TelemetryRing ring{retained};
    ring.begin();
    CHECK(ring.refusals() == 0);
  }
}

TEST_CASE("the record fits a retained slot, and the ring is bounded by what it can hold") {
  CHECK(core::cycle::kRecordBytes <= interfaces::kRetainedRecordMax);
  CHECK(kReadingsKept <= interfaces::kBackendReadings);
  // The arithmetic in the header, as a check: the header, the refusals and the readings.
  CHECK(core::cycle::kRecordBytes ==
        core::cycle::kRecordHeaderBytes + core::cycle::kRefusalsKept * 2 +
            kReadingsKept * core::cycle::kReadingBytes);
}

TEST_CASE("a week without an uplink fills the ring over whole wakes, and the uplink empties it") {
  WakeBench bench;
  bench.modemSilent();  // no modem at all: nobody to give a reading to
  // A week of six-hourly wakes is 28; the ring holds ten and counts the rest.
  for (int i = 0; i < 28; ++i) {
    const core::cycle::Pass& pass = bench.oneWake();
    CHECK(pass.ending == core::cycle::Ending::kSlept);
  }
  CHECK(bench.v().readings.count() == kReadingsKept);
  CHECK(bench.v().readings.dropped() == 28 - kReadingsKept);

  // The uplink comes back and the backend says it took them: everything kept is offered once and
  // removed. The backoff is forgotten first, because twenty-eight failures have grown it to
  // every sixteenth wake - which is the uplink's own rail and not this test's subject.
  bench.modemWorks();
  bench.world.backend.takeReadings();
  bench.boot();
  bench.v().begin();
  bench.v().uplink.forgetBackoff();
  const core::cycle::Pass& pass = bench.v().run();
  CHECK(pass.ok(Step::kBackend));
  // Ten were held, and this wake added one before the exchange: eleven were offered, which is
  // one more than the ring holds, so ten crossed and the ring is empty afterwards.
  CHECK(pass.readings_offered == kReadingsKept);
  CHECK(pass.readings_taken);
  CHECK(bench.v().readings.count() == 0);
  // And the backend was told how many it never saw.
  CHECK(bench.world.backend.last().readings_dropped == 28 - kReadingsKept + 1);
}
