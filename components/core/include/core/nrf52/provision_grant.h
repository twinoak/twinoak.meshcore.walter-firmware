// The grant that opens the one door (task p5-02; spec/SYSTEM.md S-5a). A value with no public
// constructor, made by exactly one function - grantFromTypedCommand() - from what a person typed
// after `rak provision`, in S-5a's grammar:
//
//   <DEVICEID>         16 hex digits, read without regard to case: "%08X%08X" of FICR.DEVICEID[1]
//                      then DEVICEID[0], as `rak swd id` prints it - for a radio whose access
//                      port is open;
//   locked             for a radio whose port is closed and which runs no application (the link
//                      answers no HELLO);
//   locked:<key16>     for a radio whose port is closed and whose application answers: the first
//                      16 hex digits of the public key `rak id` prints, so that a radio with an
//                      identity is named before it loses it.
//
// Anything else is a grant of no kind, which the door refuses before it touches a register. The
// door (provision_door.h) checks the grant against the chip itself - the port's state from
// APPROTECTSTATUS, the DEVICEID from FICR - so that a later caller cannot skip what the console
// checks; which of `locked` and `locked:<key16>` is right is the sequence's to decide from the
// link (task p5-03). A grant is consumed when the door opens: one grant, one opening; a grant
// moved from is spent, and a spent grant is refused.
//
// WHO MAY INCLUDE THIS: components/core/provision/ (the sequence, task p5-03), the file where a
// parsed console verb runs (components/core/console/console.cpp; the parser only recognises the
// words), and the door's own two files. Rail R3b (tools/check_rails.py) holds that list; nothing
// under supervisor/, update/, flasher/, cycle/ or a future agent/ includes it. No ESP-IDF
// include, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace nrf52 {

enum class GrantKind : std::uint8_t {
  kNone,           // not S-5a's grammar: refused by the door
  kDeviceId,       // 16 hex digits
  kLocked,         // the word `locked`
  kLockedWithKey,  // `locked:<key16>`
};
const char* describe(GrantKind kind);

constexpr std::size_t kKey16Digits = 16;

class ProvisionGrant;
ProvisionGrant grantFromTypedCommand(const char* text);

class ProvisionGrant {
 public:
  // A grant moves, and the source is spent by the move: there is one of each grant.
  ProvisionGrant(ProvisionGrant&& other) noexcept;
  ProvisionGrant& operator=(ProvisionGrant&& other) noexcept;
  ProvisionGrant(const ProvisionGrant&) = delete;
  ProvisionGrant& operator=(const ProvisionGrant&) = delete;

  GrantKind kind() const { return kind_; }
  bool valid() const { return kind_ != GrantKind::kNone; }
  bool spent() const { return spent_; }
  // kDeviceId: what was typed, as DEVICEID[0] and DEVICEID[1] (FICR's order).
  std::uint32_t deviceId(unsigned i) const { return device_id_[i & 1u]; }
  // kLockedWithKey: the 16 digits as typed, upper-cased, NUL-terminated.
  const char* key16() const { return key16_; }

 private:
  friend ProvisionGrant grantFromTypedCommand(const char* text);
  friend class ProvisionDoor;
  ProvisionGrant() = default;
  void spend() { spent_ = true; }

  GrantKind kind_ = GrantKind::kNone;
  bool spent_ = false;
  std::uint32_t device_id_[2] = {0, 0};
  char key16_[kKey16Digits + 1] = {};
};

}  // namespace nrf52
}  // namespace core
