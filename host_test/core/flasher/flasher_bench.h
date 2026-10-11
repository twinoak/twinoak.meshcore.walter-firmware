// A Walter with the flasher under test and a provisioned simulated RAK4630 (task p1-05): task
// p1-04's bench with the SoftDevice information structure installed, the fake pins' transition
// log off (a flash is millions of transitions), and the chip told that only the image's pages and
// the settings page may be touched. Every flasher test starts from one of these; its destructor
// checks that the pin rules held, the lines were released, the log stayed inside the two ranges,
// and nothing overwrote a word more often than nWRITE allows.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/flasher/flasher.h"
#include "sim/nrf52_bench.h"

namespace sim {

namespace fl = core::flasher;

// An image for the tests: `pages` pages, the first with a plausible vector table (the stack
// pointer at the top of RAM, the reset vector odd and inside the image), the rest sparse (a few
// non-blank words a page, so that the power-loss loop stays short) or dense (every word set).
inline std::vector<std::uint8_t> makeImage(std::uint32_t pages, bool dense, std::uint32_t seed = 1,
                                           std::uint32_t load = 0x26000) {
  std::vector<std::uint8_t> bytes(pages * 4096, 0xFF);
  auto put = [&](std::uint32_t offset, std::uint32_t w) {
    bytes[offset] = static_cast<std::uint8_t>(w);
    bytes[offset + 1] = static_cast<std::uint8_t>(w >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(w >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(w >> 24);
  };
  put(0, 0x20040000);          // the initial stack pointer: the top of 256 KB of RAM
  put(4, load + 0x100 + 1);    // the reset vector, Thumb, inside the image
  for (std::uint32_t p = 0; p < pages; ++p) {
    const std::uint32_t words = dense ? 1024 : 8;
    for (std::uint32_t i = 0; i < words; ++i) {
      const std::uint32_t offset = p * 4096 + (dense ? i * 4 : 8 + i * 4 + (p * 16));
      if (offset < 8) continue;
      std::uint32_t w = 0x9E3779B9u * (offset / 4 + 1) ^ (seed * 0x01000193u);
      if (w == 0xFFFFFFFFu) w ^= 1u;
      put(offset, w);
    }
  }
  return bytes;
}

struct FlasherBench : Nrf52Bench {
  const fl::BoardProfile& profile = fl::kRak4630Single;
  fl::Flasher flasher{board, clock};
  std::uint32_t allowed_begin = 0;
  std::uint32_t allowed_end = 0;

  FlasherBench() {
    pins.record_log = false;
    chip.installSoftDevice();
  }
  ~FlasherBench() {
    CHECK(pins.violations.empty());
    CHECK(!board.swdHeld());
    CHECK(chip.counters().overwrites == 0);
    CHECK(chip.counters().tripwire == 0);
    if (allowed_end != 0) {
      // Every erase and write in the log is inside the image's pages or the settings page.
      for (const FlashEvent& e : chip.log()) {
        if (e.kind != FlashEvent::Kind::kErasePage && e.kind != FlashEvent::Kind::kWrite) continue;
        const std::uint32_t len = e.kind == FlashEvent::Kind::kErasePage ? 4096 : 4;
        const bool in_image = e.addr >= allowed_begin && e.addr + len <= allowed_end;
        const bool in_settings = e.addr >= profile.settings_page && e.addr + len <= profile.settings_page + 4096;
        const bool inside = in_image || in_settings;
        CHECK_MESSAGE(inside, "a flash operation outside the image's pages and the settings page");
      }
    }
  }

  // Tell the chip, and the destructor, which pages the image covers.
  void allow(std::uint32_t load, std::uint32_t size) {
    allowed_begin = load;
    allowed_end = load + ((size + 4095) / 4096) * 4096;
    chip.onlyTouch(allowed_begin, allowed_end);
    chip.alsoTouch(profile.settings_page, profile.settings_page + 4096);
  }

  // The image as the chip holds it, byte for byte.
  bool chipHolds(const std::vector<std::uint8_t>& image, std::uint32_t load) const {
    for (std::size_t i = 0; i < image.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.size(); ++k) w |= static_cast<std::uint32_t>(image[i + k]) << (8 * k);
      for (std::size_t k = image.size() - i; k < 4; ++k) w |= 0xFFu << (8 * k);
      if (chip.flashWord(static_cast<std::uint32_t>(load + i)) != w) return false;
    }
    return true;
  }

  // The settings page as the chip holds it.
  fl::bootloader_settings::Record settings() const {
    std::uint32_t words[fl::bootloader_settings::kRecordWords];
    for (std::size_t i = 0; i < fl::bootloader_settings::kRecordWords; ++i) {
      words[i] = chip.flashWord(profile.settings_page + 4 * static_cast<std::uint32_t>(i));
    }
    return fl::bootloader_settings::decode(words);
  }
  void setSettings(const fl::bootloader_settings::Record& r) {
    std::uint32_t words[fl::bootloader_settings::kRecordWords];
    fl::bootloader_settings::encode(r, words);
    for (std::size_t i = 0; i < fl::bootloader_settings::kRecordWords; ++i) {
      chip.setFlashWord(profile.settings_page + 4 * static_cast<std::uint32_t>(i), words[i]);
    }
  }
  // A page of the chip written as if an earlier firmware had been installed there.
  void install(const std::vector<std::uint8_t>& image, std::uint32_t load) {
    for (std::size_t i = 0; i < image.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.size(); ++k) w |= static_cast<std::uint32_t>(image[i + k]) << (8 * k);
      for (std::size_t k = image.size() - i; k < 4; ++k) w |= 0xFFu << (8 * k);
      chip.setFlashWord(static_cast<std::uint32_t>(load + i), w);
    }
  }
};

// Nothing was erased or written on the chip - READY reads (a session waits for the flash
// controller before its first flash read) are the one kind of log entry a refusal may leave.
inline bool nothingWritten(const Nrf52Chip& chip) {
  return chip.countLog(FlashEvent::Kind::kErasePage) == 0 && chip.countLog(FlashEvent::Kind::kWrite) == 0 &&
         chip.countLog(FlashEvent::Kind::kConfig) == 0 && chip.countLog(FlashEvent::Kind::kRejectedWrite) == 0 &&
         chip.countLog(FlashEvent::Kind::kRejectedErase) == 0 && chip.countLog(FlashEvent::Kind::kPowerLost) == 0;
}

inline std::vector<std::uint8_t> digestOf(const std::vector<std::uint8_t>& image) {
  std::vector<std::uint8_t> d(core::image::kSha256Size);
  core::image::sha256(image.data(), image.size(), d.data());
  return d;
}

struct CountingProgress final : fl::ProgressSink {
  std::uint32_t calls = 0;
  std::uint32_t scan = 0, pages = 0, read_back = 0;
  std::uint32_t last_total = 0;
  void onProgress(const fl::Progress& p) override {
    calls++;
    last_total = p.pages_total;
    if (p.step == fl::Step::kScan) scan++;
    if (p.step == fl::Step::kPages || p.step == fl::Step::kFirstPage) pages++;
    if (p.step == fl::Step::kReadBack) read_back++;
  }
};

}  // namespace sim
