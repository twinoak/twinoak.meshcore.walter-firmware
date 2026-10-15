// What the battery allows, and how long until the next check-in (task p3-04;
// docs/ARCHITECTURE.md §10, docs/power.md). The Walter shares one 1S LiPo with the repeater it
// looks after - "a single 1S LiPo feeds both boxes", the hardware repository's
// hardware/pcb/INTERCONNECT.md at b798aab, *Power architecture* - and an LTE attach costs more
// than the repeater spends in an hour. So the battery chooses the interval, and below a floor it
// takes the modem away altogether: **a repeater that keeps repeating is worth more than a report
// that it is about to stop.**
//
// Four bands, with hysteresis between them, and for each an interval and what a wake in it may
// do. **It is a pure function**: `decide()` takes the readings this wake has, the band the last
// wake ended in, the settings and the faults, and answers; it is `const`, it keeps nothing
// across a sleep and it touches no radio, modem or pin. What crosses the sleep - the band, and
// `saving`'s every-Nth counter - is `core::cycle::WakeReason`'s retained record, because there
// is one record for the whole duty cycle (`interfaces/retained.h`, `kDutyCycle`). That is what
// makes the table test and the simulated December possible (host_test/core/power/).
//
// **Every number below is a PLACEHOLDER.** No cell's discharge curve has been measured, nothing
// has drawn current from this pack, and the three figures that would settle them - the Walter's
// draw asleep, the energy of one whole wake with an attach, and this pack's voltage against its
// state of charge - are bench measurements (`spec/SYSTEM.md` §10 item 5, `docs/bench.md` §15).
// Change a number and its sentence together.
//
// -- the readings ------------------------------------------------------------------------------
//
// A reading is POWER_GET's channel 2 over the box link: the battery's bus voltage in millivolts
// and its current in milliamps, **positive = charging** (`spec/LINK-PROTOCOL.md` §5.8, and the
// shunt is already applied by the radio). The Walter has no gauge of its own.
//
// One sample is not a reading. The voltage used is the **median** of the last few, because a
// sample taken while the modem is transmitting sags by whatever the pack's internal resistance
// and the burst's current make it; the current used is the **mean** over the samples we have,
// because what matters is whether the panel is actually putting charge in, and one instant of
// sun is not that. A band is never decided on one sample unless one sample is all there has ever
// been - and then the decision says so (`Decision::samples`).
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace power {

// -- the bands ---------------------------------------------------------------------------------

enum class Band : std::uint8_t {
  kUnknown = 0,   // no reading at all: the radio is silent. Its own case, and never kPlenty
  kPlenty = 1,    // above the upper threshold, or charging steadily
  kNormal = 2,    // between
  kSaving = 3,    // below the lower threshold
  kCritical = 4,  // below the floor
  kCount,
};

const char* name(Band band);
const char* describe(Band band);  // one line: what a wake in it may do

// -- the thresholds, in millivolts of one LiPo cell --------------------------------------------
//
// A 1S LiPo is about 4.2 V full and 3.0 V empty, and the useful part of the curve is the top: a
// cell sits near 3.7 V for most of its charge and then falls away. The three numbers below are
// read off that shape and nothing else, which is why they are placeholders:
//
//   kUpperMv   4000  about three quarters of a charge. Above it a wake every quarter of an hour
//                    is affordable even in December, when the panel gives an hour of sun a day.
//   kLowerMv   3600  about a quarter left. Below it the pack is on the steep part of the curve,
//                    and an attach that fails costs a measurable share of what is left.
//   kFloorMv   3400  the floor. Below it the modem is not used at all: the rest of the charge is
//                    what the repeater's own draw is for, and the Walter's job becomes "stay
//                    alive, keep watching the radio, say nothing".
constexpr std::uint16_t kUpperMv = 4000;
constexpr std::uint16_t kLowerMv = 3600;
constexpr std::uint16_t kFloorMv = 3400;
static_assert(kUpperMv > kLowerMv && kLowerMv > kFloorMv, "the thresholds are in order");

// The hysteresis margin. Leaving a band needs this much beyond the threshold that entered it, in
// the direction of travel, so that a pack sitting on a threshold does not change band every
// wake - each change writes a journal entry and changes the interval, and a unit that oscillates
// between a quarter of an hour and six hours is a unit nobody can predict. 60 mV is three times
// the sag a modest load step makes across a tired cell's internal resistance, and is what
// host_test/core/power/test_interval_policy.cpp wanders +-20 mV inside.
constexpr std::uint16_t kHysteresisMv = 60;
static_assert(kLowerMv - kFloorMv > 2 * kHysteresisMv, "a band is wider than its own hysteresis");

// Charging *steadily*: this much current into the pack, averaged over the samples we have. 50 mA
// into a 1S pack is a panel doing real work rather than a cloud edge; below it the charger is
// idling and the pack is not actually gaining.
constexpr std::int16_t kChargingMa = 50;

// -- the intervals, in minutes -----------------------------------------------------------------
//
// "A full battery under a sunny panel can afford a check-in every quarter of an hour; a sinking
// one in December gets one a day" (the task). The four follow from that sentence and from
// nothing measured yet.
constexpr std::uint32_t kPlentyMinutes = 15;
constexpr std::uint32_t kNormalMinutes = 60;
constexpr std::uint32_t kSavingMinutes = 360;     // six hours
constexpr std::uint32_t kCriticalMinutes = 1440;  // a day
static_assert(kPlentyMinutes < kNormalMinutes && kNormalMinutes < kSavingMinutes &&
                  kSavingMinutes < kCriticalMinutes,
              "a worse band never wakes more often");

// `kUnknown`'s own interval: the medium one. Explicitly *not* `plenty` - "no reading at all (the
// radio is silent) is its own case: a medium interval and the supervisor's rules, not plenty".
constexpr std::uint32_t kUnknownMinutes = kNormalMinutes;

// In `saving` the modem is used only every Nth wake: four six-hourly wakes is one attach a day,
// which is about what a pack at a quarter of a charge in a Danish December can pay for.
constexpr std::uint32_t kSavingModemEveryNth = 4;

// A wake that ended in a crash or a watchdog lengthens the next sleep rather than shortening it
// (the task: "too many in a row lengthen the sleep rather than shorten it"). Each consecutive
// fault doubles the interval, up to this many doublings and never past kCriticalMinutes: a
// Walter that panics on every wake must not panic sixty times an hour, and a panic caused by
// something the next wake would do again is best met with time.
constexpr std::uint32_t kMaxFaultDoublings = 4;

// -- the readings ------------------------------------------------------------------------------

// How many samples are kept. Five: enough for a median of three to mean something and for a mean
// to see past one burst, small enough to be a handful of bytes in a firmware with no heap.
constexpr std::size_t kSamplesKept = 5;

// How many of them the median is taken over. Three: the smallest median there is.
constexpr std::size_t kMedianOf = 3;
static_assert(kMedianOf <= kSamplesKept && (kMedianOf % 2) == 1, "the median is of an odd few");

struct Sample {
  std::uint16_t millivolts = 0;
  std::int16_t current_ma = 0;  // positive = charging (LINK-PROTOCOL §5.8)
  bool current_known = false;
};

// What a wake in this band may do. Read by whoever runs the wake (task p3-05) and by the
// console. Nothing here enforces it: the machines that could spend the charge have floors of
// their own (core/update/radio_update.h, core/supervisor/supervisor.h), and this is the band's
// answer on top of them.
struct Allowance {
  bool modem = false;        // may the modem be brought up at all in this wake
  bool update = false;       // may an update of either MCU be *started*
  bool flash = false;        // may either MCU be flashed
  bool power_cycle = false;  // may the radio's rail be taken away
};

// What `decide()` needs that is not a reading: everything that crossed the sleep.
struct Inputs {
  Band was = Band::kUnknown;            // the band the last wake ended in
  // `saving`'s every-Nth, counted in wakes as the uplink's own backoff is. **0 means the modem
  // has not been used since this band was entered**, which is a wake it may be used in; 1 means
  // it was used in the last wake; the Nth after that is the next one that may
  // (core::cycle::WakeReason::modemWasUsed() sets it to 1 and modemWasSkipped() counts up).
  std::uint32_t wakes_in_saving = 0;
  std::uint32_t faults_in_a_row = 0;    // consecutive crash or watchdog wakes
  // §8's attention from the radio, this wake: reason 2 (*battery low*) floors the band at
  // `saving` and reason 3 (*battery critical*) at `critical`, at once and without waiting for a
  // reading of our own - the radio has the INA3221 on its own I2C and it has already looked.
  // 0 for none. `radio_detail` is the millivolts the event carried (0 when it carried none),
  // which is used as a sample when we have none of our own.
  std::uint8_t radio_attention = 0;
  std::uint16_t radio_detail = 0;
  // `wake.interval` (core/settings/keys.h): 0 is `auto` - the battery chooses. Set to minutes it
  // overrides the first three bands' interval and **never** `critical`.
  std::uint32_t wake_interval_minutes = 0;
};

struct Decision {
  Band band = Band::kUnknown;
  Band was = Band::kUnknown;
  bool changed = false;
  std::uint32_t interval_minutes = kUnknownMinutes;
  std::uint32_t interval_seconds = kUnknownMinutes * 60;
  Allowance may{};
  // `saving`'s every-Nth: whether *this* wake is one the modem may be used in. In the other
  // bands it is exactly `may.modem`.
  bool modem_this_wake = false;
  std::uint32_t wakes_until_modem = 0;  // in `saving`: how many wakes until the next modem wake
  // The reading behind it.
  bool reading_known = false;
  std::uint16_t median_mv = 0;
  std::int16_t mean_ma = 0;
  bool current_known = false;
  bool charging = false;
  std::size_t samples = 0;
  bool from_radio = false;  // the only reading was the attention's own millivolts
  // Why this band and not another, in words, for the console and the journal.
  const char* why = "";
  std::uint32_t band_minutes = kUnknownMinutes;  // before the two things that may change it
  bool overridden = false;                       // `wake.interval` is in force
  std::uint32_t fault_doublings = 0;             // faults that lengthened it
};

// The interval and the allowance a band gives, on their own: what docs/power.md tabulates and
// what the table test walks, so that the document and the code cannot drift.
std::uint32_t minutesFor(Band band);
Allowance allowanceFor(Band band);

// The band the voltage alone says, with hysteresis against the band we were in. Public because
// the no-oscillation test walks it directly as well as through decide().
Band bandFor(std::uint16_t millivolts, Band was);

class IntervalPolicy {
 public:
  IntervalPolicy() = default;

  // A POWER_GET reading of this wake. Kept in the ring; the oldest is forgotten.
  void addSample(std::uint16_t millivolts, std::int16_t current_ma, bool current_known = true);
  // This wake could not read the battery at all (the link is down, the channel's bit was clear).
  // Nothing is added, and `decide()` answers `kUnknown` when the ring is empty.
  void noReading();

  // The decision. `const`: nothing here is remembered.
  Decision decide(const Inputs& in) const;

  // The samples, newest first, for the console's `power`.
  std::size_t sampleCount() const { return count_; }
  const Sample& sampleAt(std::size_t index) const;
  std::uint32_t misses() const { return misses_; }

  // The ring emptied: a new wake, or a test starting over.
  void forget();

 private:
  bool median(std::uint16_t* out) const;
  bool mean(std::int16_t* out) const;

  Sample samples_[kSamplesKept]{};
  std::size_t count_ = 0;  // of kSamplesKept
  std::size_t next_ = 0;   // where the next sample goes
  std::uint32_t misses_ = 0;
};

}  // namespace power
}  // namespace core
