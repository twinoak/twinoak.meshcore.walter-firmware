// A simulated December (task p3-04): a month of discharge with a short charging bump a day, a
// wake per interval, and the thing the task asks for counted - **the wakes and the modem uses
// per day in each band**. And a week with the radio silent, in which the band is never `plenty`.
//
// What is simulated and what is not. The *policy* is the real one; the pack is not. There is no
// measured discharge curve for this unit - that is `spec/SYSTEM.md` §10 item 5 and
// `docs/bench.md` §14 - so the model below is deliberately crude and deliberately stated: a
// linear drain, a daily bump that shrinks as a Danish December does, and a fixed cost per wake
// with and without the modem. What it is good for is the *shape*: that the bands are reached in
// order, that a unit in `saving` makes about one attach a day, that `critical` makes none, and
// that nothing oscillates. The absolute numbers move the day a meter is put in series.
#include <doctest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "core/power/interval_policy.h"

using core::power::Band;
using core::power::Decision;
using core::power::Inputs;
using core::power::IntervalPolicy;

namespace {

constexpr int kMinutesPerDay = 24 * 60;

// The pack, in hundredths of a millivolt so that a drain of a fraction of a millivolt a minute
// is arithmetic and not rounding.
struct Pack {
  std::int32_t centi_mv = 410000;  // 4.100 V

  std::uint16_t millivolts() const {
    const std::int32_t mv = centi_mv / 100;
    return static_cast<std::uint16_t>(mv < 0 ? 0 : mv);
  }
  void drain(std::int32_t centi) { centi_mv -= centi; }
  void charge(std::int32_t centi) { centi_mv += centi; }
};

struct Day {
  int wakes = 0;
  int modem_wakes = 0;
  Band band_at_end = Band::kUnknown;
  std::uint16_t mv_at_end = 0;
  // Two kinds of band change, counted apart. A **threshold** change is the pack crossing one of
  // the three voltages, and the hysteresis is what keeps those rare: a handful in a month. A
  // **charging** change is the hour of sun lifting `normal` to `plenty` and letting it back
  // down, which happens by design once or twice a day and is news rather than oscillation
  // (core/power/interval_policy.cpp, "charging steadily").
  int threshold_changes = 0;
  int charging_changes = 0;
};

// The cost of a wake, in hundredths of a millivolt. PLACEHOLDERS, like everything else here: a
// wake that only talks to the radio is a second or two of a hundred milliamps, and a wake with an
// attach is a minute or two of it - which on a pack of a couple of amp-hours is a fraction of a
// millivolt off the curve, not several. docs/bench.md §14 is where the real pair comes from, and
// the day it does these two and `kBaselineCentiPerMinute` move together.
constexpr std::int32_t kWakeCenti = 3;         // 0.03 mV
constexpr std::int32_t kModemWakeCenti = 20;   // 0.2 mV

}  // namespace

TEST_CASE("a simulated December: the wakes and the modem uses per day, band by band") {
  Pack pack;
  IntervalPolicy policy;
  Band band = Band::kUnknown;
  std::uint32_t wakes_in_saving = 0;
  std::vector<Day> days;
  // Counted per *wake* and not per day, because the band a day ends in is not the band all its
  // wakes were taken in: a day that began in `saving` and ended in `critical` made its attaches
  // in `saving`, and a rail about `critical` has to be read off the critical wakes.
  std::vector<int> band_wakes(static_cast<std::size_t>(Band::kCount), 0);
  std::vector<int> band_modem(static_cast<std::size_t>(Band::kCount), 0);

  // The baseline drain: what the two boxes take while the Walter sleeps, as a straight line from
  // a full pack to an empty one over about five weeks.
  constexpr std::int32_t kBaselineCentiPerMinute = 3;  // 0.03 mV a minute: ~43 mV a day

  int minutes_to_next_wake = 0;
  bool was_charging_plenty = false;
  for (int day = 0; day < 31; ++day) {
    Day today;
    // A Danish December: an hour of usable sun around noon, and less of it as the month goes on.
    const std::int32_t sun_centi_per_minute = (3100 - day * 90) / 60;
    for (int minute = 0; minute < kMinutesPerDay; ++minute) {
      pack.drain(kBaselineCentiPerMinute);
      if (minute >= 690 && minute < 750 && sun_centi_per_minute > 0) {
        pack.charge(sun_centi_per_minute);
      }
      if (minutes_to_next_wake > 0) {
        --minutes_to_next_wake;
        continue;
      }
      // A wake. Three samples, the middle one sagging by 250 mV and drawing 300 mA, as one taken
      // while the modem is transmitting does: that is the sample the median exists to throw away.
      const std::uint16_t mv = pack.millivolts();
      const bool in_the_sun = minute >= 690 && minute < 750 && sun_centi_per_minute > 0;
      const std::int16_t net_ma = in_the_sun ? 400 : -20;
      policy.forget();
      policy.addSample(mv, net_ma);
      policy.addSample(static_cast<std::uint16_t>(mv > 250 ? mv - 250 : 0), -300);
      policy.addSample(static_cast<std::uint16_t>(mv + 2), net_ma);
      Inputs in{};
      in.was = band;
      in.wakes_in_saving = wakes_in_saving;
      const Decision d = policy.decide(in);
      const bool charging_plenty =
          d.band == Band::kPlenty && d.charging && d.median_mv < core::power::kUpperMv;
      if (d.changed) {
        if (charging_plenty || was_charging_plenty) {
          ++today.charging_changes;
        } else {
          ++today.threshold_changes;
        }
      }
      was_charging_plenty = charging_plenty;
      band = d.band;
      ++today.wakes;
      ++band_wakes[static_cast<std::size_t>(d.band)];
      if (d.modem_this_wake) ++band_modem[static_cast<std::size_t>(d.band)];
      if (d.modem_this_wake) {
        ++today.modem_wakes;
        // 1 and not 0: 0 is "not used since the band was entered", which is a wake it may be
        // used in (core::cycle::WakeReason::modemWasUsed does the same).
        wakes_in_saving = 1;
        pack.drain(kModemWakeCenti);
      } else {
        if (d.band == Band::kSaving) ++wakes_in_saving;
        pack.drain(kWakeCenti);
      }
      minutes_to_next_wake = static_cast<int>(d.interval_minutes) - 1;
      today.band_at_end = d.band;
      today.mv_at_end = mv;
    }
    days.push_back(today);
  }

  // What it looks like, for the report and for a person reading the log.
  std::ostringstream table;
  table << "a simulated December, 31 days (every number a placeholder: docs/power.md)\n";
  table << " day    mV  band ending the day  wakes  modem uses  threshold changes  charging changes\n";
  int plenty_days = 0;
  int normal_days = 0;
  int saving_days = 0;
  int critical_days = 0;
  int total_wakes = 0;
  int total_modem = 0;
  int threshold_changes = 0;
  int charging_changes = 0;
  for (std::size_t i = 0; i < days.size(); ++i) {
    const Day& d = days[i];
    table << "  " << (i + 1 < 10 ? " " : "") << (i + 1) << "  " << d.mv_at_end << "  "
          << core::power::name(d.band_at_end) << "\t\t     " << d.wakes << "\t   "
          << d.modem_wakes << "\t\t  " << d.threshold_changes << "\t\t    "
          << d.charging_changes << "\n";
    total_wakes += d.wakes;
    total_modem += d.modem_wakes;
    threshold_changes += d.threshold_changes;
    charging_changes += d.charging_changes;
    switch (d.band_at_end) {
      case Band::kPlenty: ++plenty_days; break;
      case Band::kNormal: ++normal_days; break;
      case Band::kSaving: ++saving_days; break;
      case Band::kCritical: ++critical_days; break;
      default: break;
    }
  }
  table << "  totals: " << total_wakes << " wakes, " << total_modem << " modem uses, "
        << threshold_changes << " threshold band changes, " << charging_changes
        << " charging band changes; days ending plenty " << plenty_days << ", normal "
        << normal_days << ", saving " << saving_days << ", critical " << critical_days;
  MESSAGE(table.str());

  // The shape, which is what this test is for.
  //
  // 1. It walks down the bands, in order, and ends below the floor: a pack that is drained
  //    faster than an hour of December sun refills it does exactly that.
  CHECK(plenty_days > 0);
  CHECK(normal_days > 0);
  CHECK(saving_days > 0);
  CHECK(critical_days > 0);
  CHECK(days.front().band_at_end == Band::kPlenty);
  CHECK(days.back().band_at_end == Band::kCritical);
  // 2. **Nothing oscillates on a threshold.** Four bands entered once each is three changes,
  //    plus the first wake's no-reading -> plenty: four. Anything much over that is the
  //    hysteresis failing. The sun's `normal` -> `plenty` and back is counted apart and is
  //    bounded by two a day, because that is what one hour of sun a day is.
  CHECK(threshold_changes <= 6);
  CHECK(charging_changes <= 2 * 31);
  for (const Day& d : days) {
    CHECK_MESSAGE(d.threshold_changes <= 2, "a day with " << d.threshold_changes
                                                          << " threshold band changes");
  }
  // 3. **Below the floor the modem is never used, in any wake.** This is the rail the whole band
  //    exists for: a repeater that keeps repeating is worth more than a report that it is about
  //    to stop.
  CHECK(band_wakes[static_cast<std::size_t>(Band::kCritical)] > 0);
  CHECK(band_modem[static_cast<std::size_t>(Band::kCritical)] == 0);
  // 4. In `saving`, the modem is used in one wake in four.
  const int saving_wakes = band_wakes[static_cast<std::size_t>(Band::kSaving)];
  const int saving_modem = band_modem[static_cast<std::size_t>(Band::kSaving)];
  CHECK(saving_wakes > 0);
  CHECK(saving_modem > 0);
  CHECK_MESSAGE(saving_modem * 3 <= saving_wakes,
                saving_modem << " attaches in " << saving_wakes << " saving wakes");
  // In `plenty` and `normal` every wake may use it.
  CHECK(band_modem[static_cast<std::size_t>(Band::kPlenty)] ==
        band_wakes[static_cast<std::size_t>(Band::kPlenty)]);
  CHECK(band_modem[static_cast<std::size_t>(Band::kNormal)] ==
        band_wakes[static_cast<std::size_t>(Band::kNormal)]);
  MESSAGE("per band, over the month: plenty "
          << band_modem[static_cast<std::size_t>(Band::kPlenty)] << "/"
          << band_wakes[static_cast<std::size_t>(Band::kPlenty)] << " modem wakes, normal "
          << band_modem[static_cast<std::size_t>(Band::kNormal)] << "/"
          << band_wakes[static_cast<std::size_t>(Band::kNormal)] << ", saving " << saving_modem
          << "/" << saving_wakes << ", critical "
          << band_modem[static_cast<std::size_t>(Band::kCritical)] << "/"
          << band_wakes[static_cast<std::size_t>(Band::kCritical)]);
  // 5. The month gets cheaper as it goes on: the last week costs less than the first.
  int first_week_modem = 0;
  int last_week_modem = 0;
  for (std::size_t i = 0; i < 7; ++i) first_week_modem += days[i].modem_wakes;
  for (std::size_t i = days.size() - 7; i < days.size(); ++i) last_week_modem += days[i].modem_wakes;
  CHECK(last_week_modem < first_week_modem);
  MESSAGE("the first week made " << first_week_modem << " modem uses and the last "
                                 << last_week_modem);
}

TEST_CASE("a week with the radio silent: a medium interval, the supervisor's rules, never plenty") {
  IntervalPolicy policy;
  Band band = Band::kUnknown;
  int wakes = 0;
  int plenty = 0;
  int updates_allowed = 0;
  // A week at the medium interval. The radio answers nothing, so there is no reading at all -
  // the Walter has no gauge of its own (the INA3221 is on the radio's I2C).
  for (int minute = 0; minute < 7 * kMinutesPerDay; minute += static_cast<int>(core::power::kUnknownMinutes)) {
    policy.forget();
    policy.noReading();
    Inputs in{};
    in.was = band;
    const Decision d = policy.decide(in);
    band = d.band;
    ++wakes;
    CHECK(d.band == Band::kUnknown);
    CHECK(d.interval_minutes == core::power::kUnknownMinutes);
    if (d.band == Band::kPlenty) ++plenty;
    if (d.may.update) ++updates_allowed;
    // The modem is allowed: a Walter whose radio has gone quiet is the one that should say so.
    CHECK(d.may.modem);
    // And the power cycle is left to the supervisor, which has a battery floor of its own over
    // the last good reading (core/supervisor/supervisor.h, kPowerCycleMinMv).
    CHECK(d.may.power_cycle);
  }
  CHECK(wakes == 7 * kMinutesPerDay / static_cast<int>(core::power::kUnknownMinutes));
  CHECK(plenty == 0);
  CHECK(updates_allowed == 0);
  MESSAGE("a week with the radio silent: " << wakes << " wakes at "
                                           << core::power::kUnknownMinutes
                                           << " min, band no-reading throughout, no update "
                                              "allowed, the modem allowed");
}
