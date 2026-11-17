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
  // not two: platform/retained.cpp gave every slot kRetainedRecordMax bytes whatever its record
  // held, and RTC SLOW memory had 316 bytes free before that task (tasks/phase-3/reports/04.md,
  // *What RTC memory costs*). Since task p15-06 a slot is sized to its record (kRetainedSlotBytes
  // below); this one's record is kRetainedRecordMax exactly, so nothing of it changed.
  kDutyCycle = 6,
  // core/cycle/telemetry_ring.h (task p3-05): the readings of each wake's step 4 kept while
  // there is nobody to give them to, and the asks this firmware refused and has not yet been
  // able to tell anybody about. **One record for both**, for the reason the slot above says.
  kWakeCycle = 7,
  // core/cycle/wake_progress.h (task p15-06): **where a wake was when it was ended** - the step,
  // the call that was out or the job that was being run, and the second of the wake each began
  // at. Written as the wake goes and read by the boot after, which is the only reader there can
  // be: a wake ended by the bound, a crash or the chip's own watchdog leaves its log, its claim
  // and its letters in RAM, and the reset takes them.
  kWakeProgress = 8,
  kCount
};

// The most any record may be. Small on purpose: RTC memory is 8 KB and shared.
constexpr std::size_t kRetainedRecordMax = 128;

// How much room each slot has, in the order platform/retained.cpp lays them out.
//
// **Why a table and not one number** (task p15-06). Until that task every slot was given
// kRetainedRecordMax bytes whatever its record held - 128 bytes of RTC memory for a record of
// eight - and that is the reason three earlier tasks put two unrelated things in one record (the
// two slots above say so). There was no room to spend that way for a ninth: RTC SLOW memory is
// 8 192 bytes, the pinned modem component's own `.rtc.data` is 6 996 of them (`_socketCtxSetRTC`,
// `_pdpCtxSetRTC`, `blueCherryRTC` and two more, read off the build's linker map), the eight
// slots were 1 120, and what was left was **36**.
//
// A slot's capacity is part of **the layout**: the slots are laid out in this order, so changing
// an entry moves every slot after it, and a record whose slot moved is not read back - which is
// the rule above working rather than failing, and costs the one wake every change of this
// record's shape has cost.
//
//   0..6  **128, unchanged**, which is why the records of a unit in service survive this task:
//         their offsets and their rooms are what they were, so their checksums still hold.
//   7     **116**, which is `core::cycle::kRecordBytes` exactly. It is the last of the eight, so
//         giving its twelve unused bytes back moves no other slot; what it costs is that one
//         wake, because the checksum is taken over the slot's whole room and that room is now
//         smaller. The telemetry ring's ten readings and its refusals start again at the first
//         boot of the release that carries this.
//   8     **24**, `core::cycle::kProgressBytes`: the new record. 36 free plus the 12 given back
//         is 48; a slot costs 12 bytes of magic, size and checksum on top of its room, so 24 is
//         what fits with twelve bytes of RTC SLOW left for whoever needs one next.
//
// A record longer than its slot is a **failed build** and not a quiet refusal at run time: every
// owner of a slot static_asserts its record against `retainedSlotBytes()`.
constexpr std::size_t kRetainedSlotBytes[static_cast<std::size_t>(RetainedSlot::kCount)] = {
    kRetainedRecordMax,  // kResetRecord
    kRetainedRecordMax,  // kHealth
    kRetainedRecordMax,  // kSupervisor
    kRetainedRecordMax,  // kSupervisorIncidents
    kRetainedRecordMax,  // kUplink
    kRetainedRecordMax,  // kTlsTrust
    kRetainedRecordMax,  // kDutyCycle
    116,                 // kWakeCycle: core::cycle::kRecordBytes
    24,                  // kWakeProgress: core::cycle::kProgressBytes
};

// How much room a slot has, for the static_assert of whoever owns one.
constexpr std::size_t retainedSlotBytes(RetainedSlot slot) {
  return kRetainedSlotBytes[static_cast<std::size_t>(slot)];
}

class Retained {
 public:
  virtual ~Retained() = default;

  // Copies the record last stored in the slot into `out`, exactly `size` bytes. False, and `out`
  // untouched, when there is none: the first boot, a power cut, a corrupt checksum, a record of
  // another size.
  virtual bool load(RetainedSlot slot, void* out, std::size_t size) = 0;

  // Stores `size` bytes as the slot's record. False when the size is over the room the slot has
  // (`retainedSlotBytes()`).
  virtual bool store(RetainedSlot slot, const void* data, std::size_t size) = 0;
};

}  // namespace interfaces
