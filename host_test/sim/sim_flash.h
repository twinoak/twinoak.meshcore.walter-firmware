// A simulated flash region (task p2-02), behind interfaces::FlashRegion: what the image store and
// - from task p2-03 - the update journal are developed against. It is the shape of NOR flash and
// nothing else:
//
//   - **a write only clears bits** (`old & new`), so writing over what is already there is
//     allowed and is not what the caller meant;
//   - **an erase sets a whole sector to 0xFF**, and nothing else sets a bit;
//   - **the power can go at the Nth operation**, leaving that one half done: half a sector
//     erased, half the bytes of a write written. Which half is a choice this simulator makes and
//     documents - real flash is less tidy - and the tests are written so that no caller may
//     depend on it: what they check is that what is left either validates over its own bytes or
//     does not validate at all.
//
// It counts the erases of every sector (the wear test) and records every operation (the
// power-loss loops walk the list and cut the power at each one in turn).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "interfaces/flash_region.h"

namespace sim {

struct FlashOp {
  enum class Kind : std::uint8_t { kRead, kWrite, kErase };
  Kind kind;
  std::uint32_t offset;
  std::uint32_t count;
  bool powered_off = false;  // the power went at this one
};

class SimFlash final : public interfaces::FlashRegion {
 public:
  // A region of `size` bytes in sectors of `sector`, erased (0xFF everywhere), as a partition a
  // factory flash left behind.
  SimFlash(std::uint32_t size, std::uint32_t sector = 4096)
      : bytes_(size, 0xFF), sector_(sector), erases_(size / sector, 0) {}

  // ---- what it can be told ------------------------------------------------------------------

  // The 1-based number of the write or erase at which the power goes (reads do not count); 0 is
  // never. When it goes, that operation is half done, every operation afterwards fails, and
  // powered() is false until powerOn().
  std::uint32_t lose_power_at_op = 0;
  std::function<void()> on_power_loss;

  // The power back: the contents as they were when it went, the countdown cleared. What a Walter
  // that reset in the middle sees.
  void powerOn() {
    powered_ = true;
    lose_power_at_op = 0;
    ops_ = 0;
  }
  bool powered() const { return powered_; }

  // How many writes and erases there have been (what lose_power_at_op counts against).
  std::uint32_t operations() const { return ops_; }

  // ---- the interface -------------------------------------------------------------------------

  std::uint32_t size() const override { return static_cast<std::uint32_t>(bytes_.size()); }
  std::uint32_t sectorSize() const override { return sector_; }

  bool read(std::uint32_t offset, void* out, std::uint32_t count) const override {
    if (!powered_ || !inside(offset, count) || out == nullptr) return false;
    std::uint8_t* p = static_cast<std::uint8_t*>(out);
    for (std::uint32_t i = 0; i < count; ++i) p[i] = bytes_[offset + i];
    return true;
  }

  bool write(std::uint32_t offset, const void* data, std::uint32_t count) override {
    if (!powered_ || !inside(offset, count) || data == nullptr) return false;
    const bool cut = countdown();
    // Half the bytes when the power goes in the middle of a write, and the rest never arrive.
    const std::uint32_t n = cut ? count / 2 : count;
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    for (std::uint32_t i = 0; i < n; ++i) bytes_[offset + i] &= p[i];  // a write only clears bits
    log.push_back(FlashOp{FlashOp::Kind::kWrite, offset, count, cut});
    if (cut) {
      cutPower();
      return false;
    }
    return true;
  }

  bool erase(std::uint32_t offset, std::uint32_t count) override {
    if (!powered_ || !inside(offset, count)) return false;
    if (offset % sector_ != 0 || count % sector_ != 0) return false;
    const bool cut = countdown();
    // Half of the first sector when the power goes in the middle of an erase; the sectors after
    // it are untouched.
    std::uint32_t done = count;
    if (cut) done = sector_;
    for (std::uint32_t at = offset; at < offset + done; at += sector_) {
      const std::uint32_t half = cut ? sector_ / 2 : sector_;
      for (std::uint32_t i = 0; i < half; ++i) bytes_[at + i] = 0xFF;
      ++erases_[at / sector_];
    }
    log.push_back(FlashOp{FlashOp::Kind::kErase, offset, count, cut});
    if (cut) {
      cutPower();
      return false;
    }
    return true;
  }

  // ---- what the tests read -------------------------------------------------------------------

  std::vector<FlashOp> log;  // every write and erase, in order

  std::uint32_t erasesOfSector(std::uint32_t sector) const {
    return sector < erases_.size() ? erases_[sector] : 0;
  }
  std::uint32_t sectors() const { return static_cast<std::uint32_t>(erases_.size()); }
  void forgetErases() {
    for (std::uint32_t& e : erases_) e = 0;
  }

  std::uint8_t at(std::uint32_t offset) const { return bytes_[offset]; }
  // A bit flipped where no write put it: flash rot, which `image verify` must notice.
  void rot(std::uint32_t offset, std::uint8_t mask = 0x01) { bytes_[offset] ^= mask; }
  // Bytes in, as if they had been programmed earlier (the tests' way of arranging a region).
  void poke(std::uint32_t offset, const std::uint8_t* data, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) bytes_[offset + i] = data[i];
  }

 private:
  bool inside(std::uint32_t offset, std::uint32_t count) const {
    return offset <= bytes_.size() && count <= bytes_.size() - offset;
  }

  bool countdown() {
    ++ops_;
    return lose_power_at_op != 0 && ops_ == lose_power_at_op;
  }

  void cutPower() {
    powered_ = false;
    if (on_power_loss) on_power_loss();
  }

  std::vector<std::uint8_t> bytes_;
  std::uint32_t sector_;
  std::vector<std::uint32_t> erases_;
  std::uint32_t ops_ = 0;
  bool powered_ = true;
};

}  // namespace sim
