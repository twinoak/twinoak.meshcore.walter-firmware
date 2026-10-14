// interfaces::OwnUpdate on the host (task p2-04): two application slots, their otadata states,
// and the bootloader's rules at a reboot - so that the Walter's own update can be run through a
// switch, a trial, a confirmation and a rollback on the host, with the power going where a test
// says. The rules are ESP-IDF v5.5.5's, read in components/bootloader_support/src/
// bootloader_utility.c (bootloader_utility_get_selected_boot_partition) and
// components/app_update/esp_ota_ops.c, and cited where they are applied:
//
//   - reboot(): every slot PENDING_VERIFY becomes ABORTED (bootloader_utility.c: "otadata[i] is
//     marking as ABORTED"); the slot with the highest valid otadata sequence - an entry whose
//     state is neither INVALID nor ABORTED (bootloader_common_ota_select_invalid) - is selected;
//     a selected slot in state NEW becomes PENDING_VERIFY; with no valid entry at all the
//     bootloader boots ota_0 (no factory partition here) and writes it an entry of its own -
//     sequence 1, state VALID (set_actual_ota_seq, "Set actual ota_seq=1 in otadata[0]") - so a
//     unit flashed over USB runs with a VALID slot from its first boot on, never a trial;
//   - setNextBoot(): the other slot's entry gets a sequence one above the highest and the state
//     NEW (esp_rewrite_ota_data, set_new_state_otadata with rollback enabled); refused when the
//     slot holds no image (esp_ota_set_boot_partition: image_validate);
//   - beginWrite(): refused while the running image is PENDING_VERIFY (esp_ota_begin:
//     ESP_ERR_OTA_ROLLBACK_INVALID_STATE); the other slot's bytes are erased and its otadata entry
//     invalidated (esp_ota_invalidate_inactive_ota_data_slot);
//   - confirm(): the running entry becomes VALID (esp_ota_current_ota_is_workable(true));
//   - rejectAndRestart(): refused when the other slot holds nothing bootable
//     (esp_ota_check_rollback_is_possible); otherwise the running entry becomes INVALID and a
//     restart is asked for - here recorded in `restart_asked`, and the bench then reboots.
//
// A slot's "image" is whatever bytes were written; endWrite() accepts any non-empty image unless
// a test says otherwise (`reject_image_at_end`) - the real esp_ota_end checks the ESP image
// format, which the fixture images do not have and which is not what these tests are about. The
// version of a slot is what a test flashed there (`flashOverUsb`) or what a write's descriptor
// said, read out of the bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/image/descriptor.h"
#include "interfaces/own_update.h"

namespace fakes {

class FakeOwnUpdate final : public interfaces::OwnUpdate {
 public:
  struct Slot {
    std::vector<std::uint8_t> bytes;  // the slot's flash, `size` bytes, 0xFF where erased
    std::uint32_t seq = 0xFFFFFFFFu;  // the otadata entry's ota_seq; 0xFFFFFFFF = blank
    interfaces::OwnImageState state = interfaces::OwnImageState::kUndefined;
    std::string version;
    bool has_image = false;  // something endWrite() or flashOverUsb() put there
  };

  // Two slots of `slot_size` bytes at the addresses partitions.csv gives ota_0 and ota_1
  // (scaled: the tests' images are small, and a 3 MB vector per slot is not what a unit test
  // needs). The addresses are the real ones, because the self-update compares a manifest's
  // load_address with them.
  explicit FakeOwnUpdate(std::uint32_t slot_size = 64 * 1024) : slot_size_(slot_size) {
    for (Slot& s : slots_) s.bytes.assign(slot_size, 0xFF);
    // A unit out of the box: the factory image in ota_0, blank otadata as esptool left it, and
    // the first boot - which gives ota_0 its entry (the rule above).
    flashOverUsb(0, {}, "0.0.0-dev");
    reboot();
    boots = 0;
  }

  // ---- what a test arranges ------------------------------------------------------------------

  // Bytes into a slot as esptool would put them: the entry stays as it was (UNDEFINED on a fresh
  // chip). With no bytes, a stand-in image: the version alone.
  void flashOverUsb(std::uint8_t index, const std::vector<std::uint8_t>& bytes, const std::string& version) {
    Slot& s = slots_[index];
    s.bytes.assign(slot_size_, 0xFF);
    for (std::size_t i = 0; i < bytes.size() && i < s.bytes.size(); ++i) s.bytes[i] = bytes[i];
    s.version = version;
    s.has_image = true;
  }

  // The bootloader, at a reset: the rules above. Which slot runs afterwards is `running()`.
  void reboot() {
    writing_ = false;
    for (Slot& s : slots_) {
      if (s.state == interfaces::OwnImageState::kPendingVerify) s.state = interfaces::OwnImageState::kAborted;
    }
    const int active = activeEntry();
    if (active < 0) {
      // "No factory image, trying OTA 0", then set_actual_ota_seq: ota_0 gets a valid entry.
      running_ = 0;
      slots_[0].seq = highestSeq() + 1;
      slots_[0].state = interfaces::OwnImageState::kValid;
    } else {
      running_ = static_cast<std::uint8_t>(active);
      if (slots_[running_].state == interfaces::OwnImageState::kNew) {
        slots_[running_].state = interfaces::OwnImageState::kPendingVerify;
      }
    }
    restart_asked = false;
    ++boots;
  }

  // The power goes at the Nth write or erase of a slot (reads do not count); the operation is
  // half done and everything after fails until powerOn().
  std::uint32_t lose_power_at_op = 0;
  std::function<void()> on_power_loss;
  void powerOn() {
    powered_ = true;
    lose_power_at_op = 0;
    ops_ = 0;
    writing_ = false;
  }
  bool powered() const { return powered_; }

  bool reject_image_at_end = false;  // esp_ota_end's own verification fails
  bool restart_asked = false;        // rejectAndRestart() asked for a restart
  int boots = 0;
  int confirms = 0;
  int erases = 0;

  const Slot& slot(std::uint8_t index) const { return slots_[index]; }
  Slot& slot(std::uint8_t index) { return slots_[index]; }
  std::uint8_t runningIndex() const { return running_; }
  std::uint8_t otherIndex() const { return static_cast<std::uint8_t>(1 - running_); }
  std::uint32_t slotSize() const { return slot_size_; }

  // partitions.csv: ota_0 at 0x10000, ota_1 at 0x310000.
  static constexpr std::uint32_t kAddress[2] = {0x10000, 0x310000};

  // ---- the interface ------------------------------------------------------------------------

  bool running(interfaces::OwnSlot* out) override { return fill(running_, out); }
  bool other(interfaces::OwnSlot* out) override { return fill(otherIndex(), out); }

  bool beginWrite(std::uint32_t size) override {
    if (writing_ || !powered_) return false;
    if (slots_[running_].state == interfaces::OwnImageState::kPendingVerify) return false;
    if (size > slot_size_) return false;
    Slot& s = slots_[otherIndex()];
    if (!countdown()) return false;
    ++erases;
    s.bytes.assign(slot_size_, 0xFF);
    s.has_image = false;
    s.version.clear();
    // esp_ota_invalidate_inactive_ota_data_slot: the other entry is blank from here on.
    s.seq = 0xFFFFFFFFu;
    s.state = interfaces::OwnImageState::kUndefined;
    written_ = 0;
    writing_ = true;
    return true;
  }

  bool write(const void* data, std::size_t size) override {
    if (!writing_ || !powered_) return false;
    Slot& s = slots_[otherIndex()];
    if (written_ + size > slot_size_) return false;
    const bool cut = !countdown();
    const std::size_t n = cut ? size / 2 : size;
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) s.bytes[written_ + i] &= p[i];
    written_ += static_cast<std::uint32_t>(n);
    if (cut) return false;
    return true;
  }

  bool endWrite() override {
    if (!writing_) return false;
    writing_ = false;
    if (!powered_ || written_ == 0 || reject_image_at_end) return false;
    Slot& s = slots_[otherIndex()];
    s.has_image = true;
    // The version, as the real platform reads it from the image's own description: here the
    // descriptor in the bytes.
    const core::image::Found found = core::image::find(s.bytes.data(), written_);
    s.version = found.count == 1 ? std::string(found.fields.version) : std::string();
    return true;
  }

  void abortWrite() override { writing_ = false; }
  bool writing() const override { return writing_; }

  bool readSlot(std::uint8_t index, std::uint32_t offset, void* out, std::uint32_t count) const override {
    if (!powered_ || index > 1 || out == nullptr) return false;
    if (offset > slot_size_ || count > slot_size_ - offset) return false;
    std::memcpy(out, slots_[index].bytes.data() + offset, count);
    return true;
  }

  bool setNextBoot() override {
    if (!powered_) return false;
    Slot& s = slots_[otherIndex()];
    if (!s.has_image) return false;  // image_validate fails: nothing there
    if (!countdown()) return false;
    s.seq = highestSeq() + 1;
    s.state = interfaces::OwnImageState::kNew;
    return true;
  }

  bool confirm() override {
    if (!powered_) return false;
    Slot& s = slots_[running_];
    // Running with no entry of its own cannot happen after a boot (the rule above gives ota_0
    // one), but a test may arrange it; esp_ota_current_ota_is_workable would answer ESP_FAIL.
    if (s.seq == 0xFFFFFFFFu) return false;
    if (s.state != interfaces::OwnImageState::kValid) {
      if (!countdown()) return false;
      s.state = interfaces::OwnImageState::kValid;
    }
    ++confirms;
    return true;
  }

  bool rejectAndRestart() override {
    if (!powered_) return false;
    // esp_ota_check_rollback_is_possible: the other entry valid and its slot holding an image.
    const Slot& o = slots_[otherIndex()];
    const bool other_valid = o.seq != 0xFFFFFFFFu && o.state != interfaces::OwnImageState::kInvalid &&
                             o.state != interfaces::OwnImageState::kAborted;
    if (!(other_valid && o.has_image)) return false;
    if (!countdown()) return false;
    slots_[running_].state = interfaces::OwnImageState::kInvalid;
    restart_asked = true;
    return true;
  }

 private:
  bool fill(std::uint8_t index, interfaces::OwnSlot* out) const {
    if (out == nullptr) return false;
    const Slot& s = slots_[index];
    *out = interfaces::OwnSlot{};
    out->known = true;
    out->index = index;
    out->address = kAddress[index];
    out->size = slot_size_;
    out->state = s.state;
    std::strncpy(out->version, s.version.c_str(), sizeof out->version - 1);
    return true;
  }

  // bootloader_common_get_active_otadata: the valid entry with the higher sequence, or -1.
  int activeEntry() const {
    int best = -1;
    for (int i = 0; i < 2; ++i) {
      const Slot& s = slots_[i];
      const bool valid = s.seq != 0xFFFFFFFFu && s.state != interfaces::OwnImageState::kInvalid &&
                         s.state != interfaces::OwnImageState::kAborted;
      if (!valid) continue;
      if (best < 0 || s.seq > slots_[best].seq) best = i;
    }
    return best;
  }

  std::uint32_t highestSeq() const {
    std::uint32_t top = 0;
    for (const Slot& s : slots_) {
      if (s.seq != 0xFFFFFFFFu && s.seq > top) top = s.seq;
    }
    return top;
  }

  // One write or erase: false when the power goes at this one.
  bool countdown() {
    ++ops_;
    if (lose_power_at_op != 0 && ops_ == lose_power_at_op) {
      powered_ = false;
      if (on_power_loss) on_power_loss();
      return false;
    }
    return true;
  }

  std::uint32_t slot_size_;
  Slot slots_[2];
  std::uint8_t running_ = 0;
  bool writing_ = false;
  std::uint32_t written_ = 0;
  bool powered_ = true;
  std::uint32_t ops_ = 0;
};

}  // namespace fakes
