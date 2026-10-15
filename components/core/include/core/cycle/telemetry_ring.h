// The readings kept while there is nobody to give them to (task p3-05): step 4's POWER_GET and
// ENV_GET with their times, in a bounded ring that survives a sleep and a reset, oldest dropped
// first, with a count of what was dropped - and emptied **only** when the backend says it took
// them (`interfaces::BackendAnswer::took_readings`).
//
// -- where it lives, and the numbers that decided it -------------------------------------------
//
// In retained storage (`interfaces/retained.h`, `RetainedSlot::kWakeCycle`): RTC memory, which
// survives a deep sleep, a reset, a panic and a watchdog, and which a power cut clears. The task
// offers the journal's partition as the alternative "if that is too small for a useful history -
// decide with the numbers", and the numbers say no:
//
//   the journal is 1024 entries of 64 bytes in one 64 KB partition, and it exists so that an
//   *update* is never guessed at (`core/update/journal.h`: "it wraps without ever losing the
//   entries of the update in progress"). A unit checking in every quarter of an hour takes 96
//   readings a day; four to an entry is 24 entries a day, which wraps the whole log in six weeks
//   and erases a sector every two and a half days for the life of the unit - to carry readings
//   that are a gloss, in the partition whose whole purpose is the one thing that must not be
//   lost. Task p3-04 refused to journal every *sleep* for the same reason and with the same
//   arithmetic (`tasks/phase-3/reports/04.md`, *Deviations* 2).
//
// So: retained storage, `kReadingsKept` of them, and a dropped count. A ring that overflows is
// not a failure here - it is the design the task asks for ("a week without uplink fills it,
// drops the oldest, counts") - and `readings_dropped` is what tells a backend what it missed.
// What survives is **the recent history and an honest count of the rest**, which is what a
// battery that cannot pay for an attach can afford to keep.
//
// -- one record, and why it also holds the refusals --------------------------------------------
//
// `platform/retained.cpp` gives every slot `kRetainedRecordMax` bytes whatever its record holds,
// and RTC SLOW memory had 176 bytes free when this task began (`tasks/phase-3/reports/04.md`,
// *What RTC memory costs*) - one slot and no more. So this one record carries both things the
// wake cycle must not forget across a sleep: the readings, and the asks it refused and has not
// yet been able to tell anybody about (`interfaces::BackendRefusal`; "refused, recorded, and
// told at the next exchange"). A unit in `critical` does not use the modem at all, so "the next
// exchange" can be days away, and a refusal that did not survive the sleep would be a refusal
// nobody ever hears about.
//
// -- the times ---------------------------------------------------------------------------------
//
// "Timestamps taken before the clock was known are marked as such, not guessed." A reading taken
// by a Walter that has never attached has `time_known` false and `at_epoch` 0; its position in
// the ring is the only ordering there is, and that is what is sent. Nothing here invents a time
// from an uptime, because an uptime is reset by every wake (`interfaces/clock.h`).
//
// Logic: no ESP-IDF include, no heap, every loop bounded. The record is encoded to bytes by hand
// (as the journal's payloads are), so that no compiler's padding is part of the layout.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/backend.h"
#include "interfaces/retained.h"

namespace core {
namespace cycle {

// One reading, as it lies in the record. Ten bytes:
//
//    0  at_epoch   u32  0 when the clock was not known
//    4  mv         u16  the battery's bus voltage (§5.8 channel 2)
//    6  ma         i16  its current, positive = charging
//    8  flags      u8
//    9  temp_c     i8   whole degrees Celsius; a mast's temperature to the degree is all a
//                       check-in needs of it, and ENV_GET's centidegrees would cost two bytes
//                       in a record that has ten readings' worth of room in all
constexpr std::size_t kReadingBytes = 10;

constexpr std::uint8_t kReadingTimeKnown = 0x01;
constexpr std::uint8_t kReadingBatteryKnown = 0x02;
constexpr std::uint8_t kReadingCurrentKnown = 0x04;
constexpr std::uint8_t kReadingTempKnown = 0x08;

// How many are kept. Ten, which is what is left of a 128-byte retained record once the header
// and the refusals are in it - and the same ten wakes `wake` prints, so that the two halves of
// "what has this unit been doing" are the same depth. A PLACEHOLDER only in the sense that the
// record's size is: if a later task makes `platform/retained.cpp` size each slot to its record
// (the note task p3-04 left), this is the number that grows.
constexpr std::size_t kReadingsKept = 10;
static_assert(kReadingsKept <= interfaces::kBackendReadings,
              "every kept reading has to fit what crosses the backend seam");

// How many refusals are remembered. Four, the same bound as the settings an answer may ask to
// change, and more than the closed set of asks has members minus one.
constexpr std::size_t kRefusalsKept = 4;
static_assert(kRefusalsKept <= interfaces::kBackendSettingsMax,
              "every kept refusal has to fit what crosses the backend seam");

// The record's layout. Written down because it is what the size-keyed retained slot is keyed on
// (`interfaces/retained.h`: a record of another size is not the same record).
//
//    0  version    u8
//    1  count      u8   how many readings are held
//    2  next       u8   where the next one goes
//    3  refusals   u8   how many refusals are held
//    4  dropped    u16  readings dropped to make room, saturating
//    6  taken      u16  readings handed to a backend, saturating: a gloss for the console
//    8  refusal[4]      two bytes each: the ask and why
//   16  reading[10]     ten bytes each
constexpr std::size_t kRecordHeaderBytes = 8;
constexpr std::size_t kRecordBytes = kRecordHeaderBytes + kRefusalsKept * 2 +
                                     kReadingsKept * kReadingBytes;
static_assert(kRecordBytes <= interfaces::kRetainedRecordMax,
              "the wake cycle's record has to fit a retained slot");
constexpr std::uint8_t kRecordVersion = 1;

// What a reading is made of when it is taken: whatever step 4 managed to read.
struct Taken {
  bool time_known = false;
  std::uint32_t at_epoch = 0;
  bool battery_known = false;
  std::uint16_t millivolts = 0;
  bool current_known = false;
  std::int16_t current_ma = 0;
  bool temp_known = false;
  std::int16_t temp_centi = 0;  // ENV_GET's own units; kept to the degree in the record
  std::uint8_t wake = 0;        // core::cycle::Wake, as a byte: the ring knows nothing of it
};

class TelemetryRing {
 public:
  explicit TelemetryRing(interfaces::Retained& retained) : retained_(retained) {}

  // At boot, before anything else: the record, or an empty ring when there is none (a first boot
  // or a power cut, which `survived()` says).
  void begin();
  bool begun() const { return begun_; }
  bool survived() const { return survived_; }

  // One reading. The oldest is dropped when the ring is full, and `dropped()` counts it. Stored
  // at once: a wake that is reset after reading the battery and before sleeping has still read
  // the battery.
  void add(const Taken& reading);

  // What is held, oldest first.
  std::size_t count() const { return count_; }
  std::uint32_t dropped() const { return dropped_; }
  std::uint32_t taken() const { return taken_; }
  // `index` 0 is the oldest. Out of range: a zeroed reading.
  Taken at(std::size_t index) const;

  // The backend took them: the `offered` oldest readings are forgotten. Readings added *after*
  // they were offered stay, which is why the count and not "all of them" - a reading taken
  // between filling the structure and the answer coming back has not been given to anybody.
  void wereTaken(std::size_t offered);

  // -- the refusals ----------------------------------------------------------------------------

  // An ask this firmware would not carry out. The oldest is dropped when there are too many; a
  // repeat of the same (ask, why) is not added twice, because a backend that asks every wake
  // would otherwise push everything else out.
  void refused(interfaces::Ask ask, interfaces::Refusal why);
  std::size_t refusals() const { return refusals_; }
  interfaces::BackendRefusal refusalAt(std::size_t index) const;
  // They have been told: forgotten. Only an exchange that *worked* calls this.
  void refusalsTold();

  // -- filling the seam's structure ------------------------------------------------------------

  // The readings and the refusals into `known`, oldest first. Answers how many readings were
  // offered, which is what `wereTaken()` wants.
  std::size_t offerTo(interfaces::BackendKnowledge* known) const;

  // The bench and the tests: everything forgotten, as a power cut leaves it.
  void forget();

 private:
  void load();
  void save();

  interfaces::Retained& retained_;
  Taken items_[kReadingsKept]{};
  interfaces::BackendRefusal refusal_[kRefusalsKept]{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::size_t refusals_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t taken_ = 0;
  bool begun_ = false;
  bool survived_ = false;
};

}  // namespace cycle
}  // namespace core
