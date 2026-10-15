#include "core/power/interval_policy.h"

namespace core {
namespace power {

namespace {

// The three samples the median is taken over, sorted. An insertion sort over kMedianOf == 3: no
// allocation, no <algorithm>, and the whole of it is three comparisons.
void sortSmall(std::uint16_t* values, std::size_t n) {
  for (std::size_t i = 1; i < n; ++i) {
    const std::uint16_t v = values[i];
    std::size_t j = i;
    while (j > 0 && values[j - 1] > v) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = v;
  }
}

// What sampleAt() answers for an index there is no sample at. A file-scope constant and not a
// function static: core/ has no heap and no guard variable either.
constexpr Sample kNoSample{};

// One step worse, for the floor §8's attention reasons put under the band.
Band worseOf(Band a, Band b) {
  if (a == Band::kUnknown) return b;
  if (b == Band::kUnknown) return a;
  return static_cast<std::uint8_t>(a) > static_cast<std::uint8_t>(b) ? a : b;
}

}  // namespace

const char* name(Band band) {
  switch (band) {
    case Band::kUnknown: return "no-reading";
    case Band::kPlenty: return "plenty";
    case Band::kNormal: return "normal";
    case Band::kSaving: return "saving";
    case Band::kCritical: return "critical";
    case Band::kCount: break;
  }
  return "?";
}

const char* describe(Band band) {
  switch (band) {
    case Band::kUnknown:
      return "the battery cannot be read: a medium interval, and the supervisor's own rules";
    case Band::kPlenty: return "everything a wake can do, an update included";
    case Band::kNormal: return "everything but an update of either MCU unless charging";
    case Band::kSaving: return "the radio and the power reading; the modem only every few wakes";
    case Band::kCritical: return "the radio only: no modem, no flash, no power cycle";
    case Band::kCount: break;
  }
  return "?";
}

std::uint32_t minutesFor(Band band) {
  switch (band) {
    case Band::kUnknown: return kUnknownMinutes;
    case Band::kPlenty: return kPlentyMinutes;
    case Band::kNormal: return kNormalMinutes;
    case Band::kSaving: return kSavingMinutes;
    case Band::kCritical: return kCriticalMinutes;
    case Band::kCount: break;
  }
  return kUnknownMinutes;
}

Allowance allowanceFor(Band band) {
  Allowance a{};
  switch (band) {
    case Band::kPlenty:
      a.modem = true;
      a.update = true;
      a.flash = true;
      a.power_cycle = true;
      break;
    case Band::kNormal:
      // "everything but an update of either MCU unless charging": the `unless charging` half is
      // added by decide(), which is the only place that knows whether it is.
      a.modem = true;
      a.update = false;
      a.flash = false;
      a.power_cycle = true;
      break;
    case Band::kSaving:
      // "check the radio, read power; modem only every Nth wake". A power cycle is left to the
      // supervisor, which has a battery floor of its own (kPowerCycleMinMv) - taking a dead
      // repeater off and on again is the cheapest thing on its ladder and is not what empties a
      // pack; an attach is.
      a.modem = true;
      a.update = false;
      a.flash = false;
      a.power_cycle = true;
      break;
    case Band::kCritical:
      // "check the radio; **no modem, no flash, no power cycle**".
      break;
    case Band::kUnknown:
      // The radio is silent, so there is nothing to read the battery with. The modem may still
      // be used - a Walter whose radio has died is exactly the Walter that should report it -
      // but nothing that spends charge on the strength of a reading nobody has: no update, no
      // flash. The power cycle is the supervisor's rung 3 and its own floor decides
      // (core/supervisor/supervisor.h, kPowerCycleMinMv over the last good reading).
      a.modem = true;
      a.power_cycle = true;
      break;
    case Band::kCount: break;
  }
  return a;
}

Band bandFor(std::uint16_t mv, Band was) {
  // Hysteresis, in the direction of travel: entering a better band needs the threshold plus the
  // margin, entering a worse one the threshold minus it, and in between the band stays what it
  // was. With no previous band (a power cut, a first boot) the thresholds decide outright.
  const bool have = was != Band::kUnknown;
  const std::uint16_t up_upper = static_cast<std::uint16_t>(kUpperMv + kHysteresisMv);
  const std::uint16_t up_lower = static_cast<std::uint16_t>(kLowerMv + kHysteresisMv);
  const std::uint16_t up_floor = static_cast<std::uint16_t>(kFloorMv + kHysteresisMv);
  const std::uint16_t down_upper = static_cast<std::uint16_t>(kUpperMv - kHysteresisMv);
  const std::uint16_t down_lower = static_cast<std::uint16_t>(kLowerMv - kHysteresisMv);
  const std::uint16_t down_floor = static_cast<std::uint16_t>(kFloorMv - kHysteresisMv);

  if (!have) {
    if (mv >= kUpperMv) return Band::kPlenty;
    if (mv >= kLowerMv) return Band::kNormal;
    if (mv >= kFloorMv) return Band::kSaving;
    return Band::kCritical;
  }

  switch (was) {
    case Band::kPlenty:
      // It leaves `plenty` only below the upper threshold less the margin.
      if (mv > down_upper) return Band::kPlenty;
      if (mv >= down_lower) return Band::kNormal;
      if (mv >= down_floor) return Band::kSaving;
      return Band::kCritical;
    case Band::kNormal:
      if (mv >= up_upper) return Band::kPlenty;
      if (mv > down_lower) return Band::kNormal;
      if (mv >= down_floor) return Band::kSaving;
      return Band::kCritical;
    case Band::kSaving:
      if (mv >= up_upper) return Band::kPlenty;
      if (mv >= up_lower) return Band::kNormal;
      if (mv > down_floor) return Band::kSaving;
      return Band::kCritical;
    case Band::kCritical:
      if (mv >= up_upper) return Band::kPlenty;
      if (mv >= up_lower) return Band::kNormal;
      if (mv >= up_floor) return Band::kSaving;
      return Band::kCritical;
    case Band::kUnknown:
    case Band::kCount:
      break;
  }
  return Band::kCritical;
}

void IntervalPolicy::addSample(std::uint16_t millivolts, std::int16_t current_ma,
                               bool current_known) {
  samples_[next_] = Sample{millivolts, current_ma, current_known};
  next_ = (next_ + 1) % kSamplesKept;
  if (count_ < kSamplesKept) ++count_;
}

void IntervalPolicy::noReading() { ++misses_; }

void IntervalPolicy::forget() {
  count_ = 0;
  next_ = 0;
  misses_ = 0;
}

const Sample& IntervalPolicy::sampleAt(std::size_t index) const {
  // Newest first: `next_` is where the next one goes, so `next_ - 1` is the newest.
  if (index >= count_) return kNoSample;
  const std::size_t at = (next_ + kSamplesKept - 1 - index) % kSamplesKept;
  return samples_[at];
}

bool IntervalPolicy::median(std::uint16_t* out) const {
  if (count_ == 0) return false;
  const std::size_t take = count_ < kMedianOf ? count_ : kMedianOf;
  std::uint16_t values[kMedianOf] = {};
  for (std::size_t i = 0; i < take; ++i) values[i] = sampleAt(i).millivolts;
  sortSmall(values, take);
  *out = values[take / 2];
  return true;
}

bool IntervalPolicy::mean(std::int16_t* out) const {
  std::int32_t total = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    const Sample& s = sampleAt(i);
    if (!s.current_known) continue;
    total += s.current_ma;
    ++n;
  }
  if (n == 0) return false;
  // Rounded towards zero, which for a current near the charging threshold is the conservative
  // way: 49.9 mA is not charging steadily.
  *out = static_cast<std::int16_t>(total / static_cast<std::int32_t>(n));
  return true;
}

Decision IntervalPolicy::decide(const Inputs& in) const {
  Decision d{};
  d.was = in.was;
  d.samples = count_;

  std::uint16_t mv = 0;
  d.reading_known = median(&mv);
  if (!d.reading_known && in.radio_detail != 0 &&
      (in.radio_attention == 2 || in.radio_attention == 3)) {
    // We have no reading of our own and the radio's attention carried millivolts (§8: `detail`
    // is the battery millivolts for reasons 2 and 3). Better its number than none.
    mv = in.radio_detail;
    d.reading_known = true;
    d.from_radio = true;
  }
  d.median_mv = d.reading_known ? mv : 0;
  d.current_known = mean(&d.mean_ma);
  d.charging = d.current_known && d.mean_ma >= kChargingMa;

  if (!d.reading_known) {
    d.band = Band::kUnknown;
    d.why = "no reading: the radio did not answer POWER_GET";
  } else {
    d.band = bandFor(mv, in.was);
    d.why = d.band == in.was ? "the reading, and the band it was already in"
                             : "the reading, against the thresholds";
    // "above the upper threshold, **or charging steadily**". Charging lifts `normal` to `plenty`
    // and nothing else: a cell below the lower threshold was recently nearly empty, and one
    // sunny quarter of an hour is not a reason to spend an attach on it - the band will rise on
    // its own as soon as the voltage does, which is what the panel is for. Said out loud here
    // and in docs/power.md because the task's table reads as a plain "or".
    if (d.charging && d.band == Band::kNormal) {
      d.band = Band::kPlenty;
      d.why = "charging steadily";
    }
  }

  // §8's attention reasons 2 and 3 move the band down at once. `worseOf` and not an assignment,
  // so that a radio saying "low" cannot *raise* a band our own reading put at critical.
  if (in.radio_attention == 3) {
    d.band = worseOf(d.band, Band::kCritical);
    d.why = "the radio raised attention 3: the battery is critical";
  } else if (in.radio_attention == 2) {
    d.band = worseOf(d.band, Band::kSaving);
    if (d.band == Band::kSaving) d.why = "the radio raised attention 2: the battery is low";
  }

  d.changed = d.band != in.was;
  d.may = allowanceFor(d.band);
  // `normal`: "everything but an update of either MCU unless charging".
  if (d.band == Band::kNormal && d.charging) {
    d.may.update = true;
    d.may.flash = true;
  }

  d.band_minutes = minutesFor(d.band);
  std::uint32_t minutes = d.band_minutes;

  // `wake.interval` overrides the first three bands and never `critical`.
  if (in.wake_interval_minutes != 0 && d.band != Band::kCritical) {
    minutes = in.wake_interval_minutes;
    d.overridden = true;
  }

  // A run of crashes or watchdogs lengthens it, and never past the longest interval there is.
  if (in.faults_in_a_row != 0) {
    d.fault_doublings =
        in.faults_in_a_row < kMaxFaultDoublings ? in.faults_in_a_row : kMaxFaultDoublings;
    for (std::uint32_t i = 0; i < d.fault_doublings; ++i) {
      if (minutes >= kCriticalMinutes) break;
      minutes *= 2;
    }
    if (minutes > kCriticalMinutes) minutes = kCriticalMinutes;
  }

  d.interval_minutes = minutes;
  d.interval_seconds = minutes * 60u;

  // `saving`: the modem only every Nth wake. Counted in wakes, as the uplink's own backoff is -
  // a Walter has no clock it can trust across a deep sleep (interfaces/clock.h).
  d.modem_this_wake = d.may.modem;
  if (d.band == Band::kSaving && d.may.modem) {
    const std::uint32_t since = in.wakes_in_saving;
    d.modem_this_wake = since == 0 || since >= kSavingModemEveryNth;
    d.wakes_until_modem = d.modem_this_wake ? 0 : kSavingModemEveryNth - since;
  }
  return d;
}

}  // namespace power
}  // namespace core
