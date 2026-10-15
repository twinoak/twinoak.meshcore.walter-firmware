// The battery's last good reading (task p2-05): a BatterySource that remembers what the one
// under it last answered, and answers that when the live reading cannot be had.
//
// Why it exists. The Walter has no gauge of its own - the INA3221 is on the radio's I2C and its
// battery channel comes over the link's POWER_GET (spec/LINK-PROTOCOL.md §5.8,
// core/update/link_battery.h) - so the one moment the firmware most needs a reading is the one
// moment it cannot take one: the radio has gone quiet. Without this, the supervisor's rung 4
// could never put the golden image back, because the update machine refuses to start on a
// battery it cannot read (core/update/radio_update.h, Reason::kBatteryUnknown) and a silent
// radio reads none.
//
// The task puts it plainly: "a radio that browned out because the battery is empty needs sun,
// not a reflash; POWER_GET's **last good reading** and the reset reason are what tell the two
// apart". So the last good reading is kept, with the time it was taken, and offered for
// kStaleAfterMs - after which it is no reading at all, because a cell that was full six hours
// ago says nothing about a cell that has been driving two MCUs since.
//
// A trend is never invented: the reading is passed on exactly as it came, trend and all (phase
// 3's power component keeps a history and will answer with one).
#pragma once

#include <cstdint>

#include "core/update/battery.h"
#include "interfaces/clock.h"

namespace core {
namespace update {

// How old a reading may be and still be used. Six hours: long enough that a radio which went
// quiet an hour ago is still judged on what it reported, short enough that a reflash is never
// authorised by yesterday's sunshine. A PLACEHOLDER until the pack's own curve is measured
// (docs/bench.md §11).
constexpr std::uint32_t kBatteryStaleAfterMs = 6u * 60 * 60 * 1000;

class LastGoodBattery final : public BatterySource {
 public:
  LastGoodBattery(BatterySource& under, interfaces::Clock& clock) : under_(under), clock_(clock) {}

  // The live reading when there is one, else the last good one while it is younger than
  // kBatteryStaleAfterMs. False when there has never been one, or the last is too old.
  bool read(BatteryReading* out) override;

  bool known() const { return known_; }
  // True when the last answer came from the cache and not from the radio.
  bool wasStale() const { return stale_; }
  // When the kept reading was taken. 0 when there is none.
  std::uint64_t takenAtMs() const { return taken_at_ms_; }
  std::uint32_t ageMs() const;
  // The kept reading itself, by value: a caller that holds one holds a copy.
  BatteryReading kept() const { return kept_; }

  std::uint32_t liveReads() const { return live_reads_; }
  std::uint32_t staleReads() const { return stale_reads_; }
  std::uint32_t misses() const { return misses_; }

  // A power cut: nothing is remembered (the host tests' way of starting over).
  void forget();

 private:
  BatterySource& under_;
  interfaces::Clock& clock_;
  BatteryReading kept_;
  bool known_ = false;
  bool stale_ = false;
  std::uint64_t taken_at_ms_ = 0;
  std::uint32_t live_reads_ = 0;
  std::uint32_t stale_reads_ = 0;
  std::uint32_t misses_ = 0;
};

}  // namespace update
}  // namespace core
