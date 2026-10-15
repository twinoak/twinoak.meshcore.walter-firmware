// What the battery allows (task p3-04, core/power/interval_policy.h). It is a pure function, so
// these are tables: a grid of voltages and currents against the band and the interval, the
// hysteresis shown not to oscillate, the median and the mean, `wake.interval`'s override of
// three bands and not the fourth, §8's attention reasons 2 and 3, and a run of faults
// lengthening the sleep rather than shortening it.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/power/interval_policy.h"

using core::power::Allowance;
using core::power::Band;
using core::power::Decision;
using core::power::Inputs;
using core::power::IntervalPolicy;

namespace {

// One decision from one voltage, with no history: the thresholds decide outright.
Decision fromOne(std::uint16_t mv, std::int16_t ma = -20, Band was = Band::kUnknown,
                 std::uint32_t setting = 0) {
  IntervalPolicy policy;
  policy.addSample(mv, ma);
  Inputs in{};
  in.was = was;
  in.wake_interval_minutes = setting;
  return policy.decide(in);
}

}  // namespace

TEST_CASE("the table: a voltage, a band, an interval and what the wake may do") {
  struct Row {
    std::uint16_t mv;
    std::int16_t ma;
    Band band;
    std::uint32_t minutes;
    bool modem;
    bool update;
    bool power_cycle;
  };
  // Read straight off docs/power.md's table. Discharging in every row but the last two, so that
  // "or charging steadily" is a case of its own below.
  const Row rows[] = {
      {4200, -20, Band::kPlenty, core::power::kPlentyMinutes, true, true, true},
      {4000, -20, Band::kPlenty, core::power::kPlentyMinutes, true, true, true},
      {3999, -20, Band::kNormal, core::power::kNormalMinutes, true, false, true},
      {3800, -20, Band::kNormal, core::power::kNormalMinutes, true, false, true},
      {3600, -20, Band::kNormal, core::power::kNormalMinutes, true, false, true},
      {3599, -20, Band::kSaving, core::power::kSavingMinutes, true, false, true},
      {3400, -20, Band::kSaving, core::power::kSavingMinutes, true, false, true},
      {3399, -20, Band::kCritical, core::power::kCriticalMinutes, false, false, false},
      {3000, -20, Band::kCritical, core::power::kCriticalMinutes, false, false, false},
  };
  for (const Row& row : rows) {
    const Decision d = fromOne(row.mv, row.ma);
    const std::string where = std::to_string(row.mv) + " mV, " + std::to_string(row.ma) + " mA";
    CHECK_MESSAGE(d.band == row.band, where << " is " << core::power::name(d.band));
    CHECK_MESSAGE(d.interval_minutes == row.minutes, where);
    CHECK_MESSAGE(d.interval_seconds == row.minutes * 60u, where);
    CHECK_MESSAGE(d.may.modem == row.modem, where);
    CHECK_MESSAGE(d.may.update == row.update, where);
    CHECK_MESSAGE(d.may.power_cycle == row.power_cycle, where);
    // Below the floor there is no flash either, and no modem in *any* wake.
    if (row.band == Band::kCritical) {
      CHECK_MESSAGE(!d.may.flash, where);
      CHECK_MESSAGE(!d.modem_this_wake, where);
    }
    CHECK_MESSAGE(std::string(d.why).size() > 0, where);
  }
  // Every band has a name, a sentence and an interval.
  for (std::uint8_t b = 0; b < static_cast<std::uint8_t>(Band::kCount); ++b) {
    const Band band = static_cast<Band>(b);
    CHECK(std::string(core::power::name(band)) != "?");
    CHECK(std::string(core::power::describe(band)) != "?");
    CHECK(core::power::minutesFor(band) > 0);
  }
}

TEST_CASE("charging steadily lifts normal to plenty, and lifts nothing else") {
  const std::int16_t charging = core::power::kChargingMa + 10;
  // In `normal` territory and charging: `plenty`, and an update is allowed.
  const Decision up = fromOne(3800, charging);
  CHECK(up.band == Band::kPlenty);
  CHECK(up.charging);
  CHECK(up.may.update);
  CHECK(std::string(up.why) == "charging steadily");
  // Just under the charging threshold is not charging steadily.
  const Decision idle = fromOne(3800, core::power::kChargingMa - 1);
  CHECK(idle.band == Band::kNormal);
  CHECK_FALSE(idle.charging);
  CHECK_FALSE(idle.may.update);
  // `normal` *and charging* is what unlocks an update; `normal` on its own does not.
  CHECK(fromOne(3800, charging).may.flash);
  CHECK_FALSE(fromOne(3800, -20).may.flash);
  // Below the lower threshold, charging does not lift the band: a cell that was nearly empty a
  // moment ago is not a cell to spend an attach on because the sun came out.
  CHECK(fromOne(3500, charging).band == Band::kSaving);
  CHECK(fromOne(3300, charging).band == Band::kCritical);
  CHECK_FALSE(fromOne(3300, charging).may.modem);
}

TEST_CASE("no reading at all is its own band: a medium interval, and never plenty") {
  IntervalPolicy policy;
  policy.noReading();
  const Decision d = policy.decide(Inputs{});
  CHECK(d.band == Band::kUnknown);
  CHECK_FALSE(d.reading_known);
  CHECK(d.interval_minutes == core::power::kUnknownMinutes);
  CHECK(d.interval_minutes == core::power::kNormalMinutes);
  CHECK(d.band != Band::kPlenty);
  // The modem may still be used - a Walter whose radio has gone quiet is the one that should
  // report it - and nothing that spends charge on the strength of a reading nobody has.
  CHECK(d.may.modem);
  CHECK_FALSE(d.may.update);
  CHECK_FALSE(d.may.flash);
  // The power cycle is the supervisor's rung 3 and its own battery floor decides.
  CHECK(d.may.power_cycle);
  CHECK(policy.misses() == 1);
  // And a reading that was there before is not forgotten by one miss.
  policy.addSample(4100, -10);
  policy.noReading();
  CHECK(policy.decide(Inputs{}).band == Band::kPlenty);
}

TEST_CASE("the hysteresis: a voltage wandering around each threshold changes band at most once") {
  const std::uint16_t thresholds[] = {core::power::kUpperMv, core::power::kLowerMv,
                                      core::power::kFloorMv};
  for (std::uint16_t threshold : thresholds) {
    Band was = Band::kUnknown;
    int changes = 0;
    // +-20 mV around the threshold, up and down, twenty times over.
    const int offsets[] = {0, 20, -20, 10, -10, 20, -20, 0, -20, 20};
    for (int lap = 0; lap < 2; ++lap) {
      for (int offset : offsets) {
        const std::uint16_t mv = static_cast<std::uint16_t>(threshold + offset);
        IntervalPolicy policy;
        // Three identical samples: the median of them is the voltage itself.
        policy.addSample(mv, -20);
        policy.addSample(mv, -20);
        policy.addSample(mv, -20);
        Inputs in{};
        in.was = was;
        const Decision d = policy.decide(in);
        if (d.changed) ++changes;
        was = d.band;
      }
    }
    CHECK_MESSAGE(changes == 1, "wandering +-20 mV around " << threshold << " mV changed band "
                                                            << changes << " time(s)");
  }
}

TEST_CASE("the hysteresis needs a real margin to leave a band, in either direction") {
  const std::uint16_t h = core::power::kHysteresisMv;
  // Up out of `normal` into `plenty`: the threshold alone is not enough.
  CHECK(fromOne(core::power::kUpperMv, -20, Band::kNormal).band == Band::kNormal);
  CHECK(fromOne(static_cast<std::uint16_t>(core::power::kUpperMv + h - 1), -20, Band::kNormal)
            .band == Band::kNormal);
  CHECK(fromOne(static_cast<std::uint16_t>(core::power::kUpperMv + h), -20, Band::kNormal).band ==
        Band::kPlenty);
  // Down out of `plenty`: the same, the other way.
  CHECK(fromOne(static_cast<std::uint16_t>(core::power::kUpperMv - 1), -20, Band::kPlenty).band ==
        Band::kPlenty);
  CHECK(fromOne(static_cast<std::uint16_t>(core::power::kUpperMv - h), -20, Band::kPlenty).band ==
        Band::kNormal);
  // Up out of `critical`: a pack that has been charged needs the margin before the modem is used
  // again, which is the whole point of the floor.
  CHECK(fromOne(core::power::kFloorMv, -20, Band::kCritical).band == Band::kCritical);
  CHECK(fromOne(static_cast<std::uint16_t>(core::power::kFloorMv + h), -20, Band::kCritical)
            .band == Band::kSaving);
  // And a collapse skips bands: there is no rule that says one step at a time.
  CHECK(fromOne(3100, -20, Band::kPlenty).band == Band::kCritical);
}

TEST_CASE("the voltage is a median and the current a mean, over the samples we have") {
  IntervalPolicy policy;
  // A sample taken while the modem was transmitting: 300 mV of sag, and the median ignores it.
  policy.addSample(3900, -20);
  policy.addSample(3600, -400);  // the burst
  policy.addSample(3910, -20);
  const Decision d = policy.decide(Inputs{});
  CHECK(d.median_mv == 3900);
  CHECK(d.band == Band::kNormal);
  CHECK(d.samples == 3);
  // The current is the mean of the three, which the burst does move - and that is wanted: a mean
  // that ignored the burst would call a discharging pack idle.
  CHECK(d.current_known);
  CHECK(d.mean_ma == (-20 + -400 + -20) / 3);
  // The median is of the newest kMedianOf, not of everything kept.
  IntervalPolicy five;
  five.addSample(3000, 0);
  five.addSample(3000, 0);
  five.addSample(4100, 0);
  five.addSample(4100, 0);
  five.addSample(4100, 0);
  CHECK(five.decide(Inputs{}).median_mv == 4100);
  // One sample is a reading, and the decision says how few it had.
  IntervalPolicy one;
  one.addSample(3700, -5);
  const Decision from_one = one.decide(Inputs{});
  CHECK(from_one.samples == 1);
  CHECK(from_one.median_mv == 3700);
  // A current nobody read is not a current of zero.
  IntervalPolicy no_current;
  no_current.addSample(3700, 0, false);
  const Decision d2 = no_current.decide(Inputs{});
  CHECK(d2.reading_known);
  CHECK_FALSE(d2.current_known);
  CHECK_FALSE(d2.charging);
  // The ring is five deep and the newest is first.
  IntervalPolicy ring;
  for (int i = 0; i < 7; ++i) ring.addSample(static_cast<std::uint16_t>(3500 + i * 10), -1);
  CHECK(ring.sampleCount() == core::power::kSamplesKept);
  CHECK(ring.sampleAt(0).millivolts == 3560);
  CHECK(ring.sampleAt(4).millivolts == 3520);
  ring.forget();
  CHECK(ring.sampleCount() == 0);
  CHECK(ring.decide(Inputs{}).band == Band::kUnknown);
}

TEST_CASE("wake.interval overrides three bands and never critical") {
  const std::uint32_t asked = 30;
  const struct {
    std::uint16_t mv;
    Band band;
    bool overridden;
  } rows[] = {
      {4100, Band::kPlenty, true},
      {3800, Band::kNormal, true},
      {3500, Band::kSaving, true},
      {3300, Band::kCritical, false},
  };
  for (const auto& row : rows) {
    const Decision d = fromOne(row.mv, -20, Band::kUnknown, asked);
    CHECK_MESSAGE(d.band == row.band, row.mv);
    CHECK_MESSAGE(d.overridden == row.overridden, core::power::name(d.band));
    CHECK_MESSAGE(d.interval_minutes == (row.overridden ? asked : core::power::kCriticalMinutes),
                  core::power::name(d.band));
    // The band's own interval is reported beside it, so the console can say which is which.
    CHECK_MESSAGE(d.band_minutes == core::power::minutesFor(row.band), core::power::name(d.band));
  }
  // `auto` - 0 - is the battery deciding, which is the default.
  CHECK_FALSE(fromOne(3800, -20, Band::kUnknown, 0).overridden);
  CHECK(fromOne(3800, -20, Band::kUnknown, 0).interval_minutes == core::power::kNormalMinutes);
}

TEST_CASE("saving uses the modem every few wakes, and says how many are left") {
  IntervalPolicy policy;
  policy.addSample(3500, -20);
  // The wake in which the modem was last used (the counter at zero) is a modem wake.
  Inputs in{};
  in.was = Band::kSaving;
  in.wakes_in_saving = 0;
  CHECK(policy.decide(in).band == Band::kSaving);
  CHECK(policy.decide(in).modem_this_wake);
  // The ones after it are not, until the Nth.
  for (std::uint32_t since = 1; since < core::power::kSavingModemEveryNth; ++since) {
    in.wakes_in_saving = since;
    const Decision d = policy.decide(in);
    CHECK_MESSAGE(!d.modem_this_wake, since);
    CHECK_MESSAGE(d.wakes_until_modem == core::power::kSavingModemEveryNth - since, since);
    CHECK_MESSAGE(d.may.modem, since);  // the band allows it; this wake is not its turn
  }
  in.wakes_in_saving = core::power::kSavingModemEveryNth;
  CHECK(policy.decide(in).modem_this_wake);
  CHECK(policy.decide(in).wakes_until_modem == 0);
  // In the other bands the counter means nothing.
  IntervalPolicy full;
  full.addSample(4100, -20);
  Inputs plenty{};
  plenty.was = Band::kPlenty;
  plenty.wakes_in_saving = 2;
  CHECK(full.decide(plenty).modem_this_wake);
}

TEST_CASE("the radio's attention 2 and 3 move the band down at once") {
  // A full pack by our own reading, and a radio that says the battery is critical: the radio
  // wins downwards. It has the gauge; we have a number over a link.
  IntervalPolicy policy;
  policy.addSample(4100, -10);
  Inputs low{};
  low.radio_attention = 2;
  low.radio_detail = 3550;
  const Decision after_low = policy.decide(low);
  CHECK(after_low.band == Band::kSaving);
  CHECK(std::string(after_low.why).find("attention 2") != std::string::npos);

  Inputs critical{};
  critical.radio_attention = 3;
  critical.radio_detail = 3380;
  const Decision after_critical = policy.decide(critical);
  CHECK(after_critical.band == Band::kCritical);
  CHECK_FALSE(after_critical.may.modem);
  CHECK_FALSE(after_critical.may.power_cycle);
  CHECK(std::string(after_critical.why).find("attention 3") != std::string::npos);

  // It never moves a band *up*: a radio saying "low" to a Walter whose own reading is below the
  // floor leaves it below the floor.
  IntervalPolicy empty;
  empty.addSample(3100, -20);
  CHECK(empty.decide(low).band == Band::kCritical);

  // And with no reading of our own, the millivolts the event carried are the reading.
  IntervalPolicy blind;
  blind.noReading();
  const Decision from_radio = blind.decide(critical);
  CHECK(from_radio.reading_known);
  CHECK(from_radio.from_radio);
  CHECK(from_radio.median_mv == 3380);
  CHECK(from_radio.band == Band::kCritical);
  // An attention of another reason is not a battery reading.
  Inputs person{};
  person.radio_attention = 4;
  person.radio_detail = 0;
  IntervalPolicy blind2;
  blind2.noReading();
  CHECK_FALSE(blind2.decide(person).reading_known);
  CHECK(blind2.decide(person).band == Band::kUnknown);
}

TEST_CASE("faults in a row lengthen the next sleep, and never past the longest interval") {
  IntervalPolicy policy;
  policy.addSample(4100, -10);  // plenty: the shortest interval there is
  std::uint32_t previous = 0;
  for (std::uint32_t faults = 0; faults <= core::power::kMaxFaultDoublings + 3; ++faults) {
    Inputs in{};
    in.was = Band::kPlenty;
    in.faults_in_a_row = faults;
    const Decision d = policy.decide(in);
    CHECK_MESSAGE(d.band == Band::kPlenty, faults);
    CHECK_MESSAGE(d.interval_minutes >= previous, faults);  // never shorter than the last
    CHECK_MESSAGE(d.interval_minutes <= core::power::kCriticalMinutes, faults);
    if (faults == 0) {
      CHECK(d.interval_minutes == core::power::kPlentyMinutes);
      CHECK(d.fault_doublings == 0);
    }
    if (faults >= 1) CHECK_MESSAGE(d.interval_minutes > core::power::kPlentyMinutes, faults);
    if (faults >= core::power::kMaxFaultDoublings) {
      CHECK_MESSAGE(d.fault_doublings == core::power::kMaxFaultDoublings, faults);
    }
    previous = d.interval_minutes;
  }
  // One fault doubles it, exactly.
  Inputs one{};
  one.was = Band::kPlenty;
  one.faults_in_a_row = 1;
  CHECK(policy.decide(one).interval_minutes == core::power::kPlentyMinutes * 2);
  // And the longest interval cannot be lengthened at all.
  IntervalPolicy flat;
  flat.addSample(3100, -20);
  Inputs many{};
  many.was = Band::kCritical;
  many.faults_in_a_row = 10;
  CHECK(flat.decide(many).interval_minutes == core::power::kCriticalMinutes);
}

TEST_CASE("every threshold and interval is a number somebody chose") {
  // The same rail as the uplink's table test (host_test/core/uplink/test_uplink_table.cpp): a
  // placeholder is a number with a reason, not a zero nobody noticed.
  CHECK(core::power::kUpperMv > core::power::kLowerMv);
  CHECK(core::power::kLowerMv > core::power::kFloorMv);
  CHECK(core::power::kFloorMv > 3000);   // a 1S LiPo below 3.0 V is a damaged 1S LiPo
  CHECK(core::power::kUpperMv < 4250);   // and above 4.2 V it is not a 1S LiPo
  CHECK(core::power::kHysteresisMv >= 20);
  CHECK(core::power::kHysteresisMv < (core::power::kLowerMv - core::power::kFloorMv) / 2);
  CHECK(core::power::kChargingMa > 0);
  CHECK(core::power::kPlentyMinutes >= 5);
  CHECK(core::power::kCriticalMinutes <= 7 * 24 * 60);
  CHECK(core::power::kSavingModemEveryNth >= 2);
  CHECK(core::power::kMaxFaultDoublings >= 1);
  CHECK(core::power::kSamplesKept >= core::power::kMedianOf);
}
