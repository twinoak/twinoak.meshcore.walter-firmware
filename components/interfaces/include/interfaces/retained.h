// Retained storage: a few small records that survive a reset, a panic, a watchdog and a deep sleep
// but not a power cut. On the ESP32-S3 that is RTC memory, with a checksum (platform/retained);
// on the host, a map the test can clear to simulate a power cut.
//
// A record is identified by its slot and its size: a record of another size in the same slot is
// not the same record, so a firmware whose layout changed does not read the old bytes as the new
// ones. What goes in is plain data - no pointers, no objects with constructors - because what
// comes out is the bytes, copied.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

enum class RetainedSlot : std::uint8_t {
  kResetRecord = 0,  // core/board: when the last reset pulse ended
  kHealth = 1,     // core/health: the boot counter and the last reasons
  kSupervisor = 2,           // core/supervisor: the day's counts, their window, a stop in force
  kSupervisorIncidents = 3,  // core/supervisor: the incidents kept for the next check-in
  kUplink = 4,               // core/uplink: the backoff's wakes, and a PIN that was refused
  kTlsTrust = 5,             // core/net/trust: what was written into the modem's certificate slots
  // core/cycle and core/power: the last ten wakes with their reasons and durations, the spurious
  // count and the disarm, and the band the interval policy was last in. **One slot for both**,
  // not two: platform/retained.cpp gives every slot kRetainedRecordMax bytes whatever its record
  // holds, and RTC SLOW memory had 316 bytes free before this task (tasks/phase-3/reports/04.md,
  // *What RTC memory costs*).
  kDutyCycle = 6,
  // core/cycle/telemetry_ring.h (task p3-05): the readings of each wake's step 4 kept while
  // there is nobody to give them to, and the asks this firmware refused and has not yet been
  // able to tell anybody about. **One record for both**, for the reason the slot above says: a
  // slot costs kRetainedRecordMax bytes whatever it holds, and RTC SLOW memory had 176 bytes
  // free when task p3-05 began. The next task that wants a slot does not have one, and
  // tasks/phase-3/reports/05.md says what to do about it.
  kWakeCycle = 7,
  kCount
};

// The most a record may be. Small on purpose: RTC memory is 8 KB and shared.
constexpr std::size_t kRetainedRecordMax = 128;

class Retained {
 public:
  virtual ~Retained() = default;

  // Copies the record last stored in the slot into `out`, exactly `size` bytes. False, and `out`
  // untouched, when there is none: the first boot, a power cut, a corrupt checksum, a record of
  // another size.
  virtual bool load(RetainedSlot slot, void* out, std::size_t size) = 0;

  // Stores `size` bytes as the slot's record. False when the size is over kRetainedRecordMax.
  virtual bool store(RetainedSlot slot, const void* data, std::size_t size) = 0;
};

}  // namespace interfaces
