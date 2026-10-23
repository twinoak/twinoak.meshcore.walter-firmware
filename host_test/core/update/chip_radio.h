// A simulated nRF52840 and the radio that runs whatever its flash holds (task p4-05): the
// coupling task p2-03's update bench made (host_test/core/update/update_bench.h, UpdateWorld),
// as one piece a bench that already has a radio and a clock can add - the wake bench, whose
// agent applies a candidate through the real flasher and has to see the radio come up with the
// flashed version, or not. The rules are the update bench's, line for line: when the flasher
// resets the chip the radio goes dark, boots after `boot_delay_ms` into the application the
// bootloader would start - its descriptor's version, board and role, or NoiseScope for a scanner
// image - and stays silent with no startable application; the `boot` knob applies a fault to the
// boot; the restart and silence knobs fire at their times. update_bench.h predates this header
// and keeps its own copy of the same rules.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/flasher/board_profile.h"
#include "core/flasher/bootloader_settings.h"
#include "core/image/descriptor.h"
#include "core/image/store_bench.h"
#include "core/link/codec/wire.h"
#include "core/update/update_bench.h"
#include "fakes/fake_clock.h"
#include "sim/nrf52_chip.h"
#include "sim/scripted_radio.h"

namespace bench {

struct ChipRadio {
  fakes::FakeClock& clock;
  sim::ScriptedRadio& radio;
  sim::Nrf52Chip chip{clock};
  const core::flasher::BoardProfile& profile = core::flasher::kRak4630Single;

  // The knobs, as the update bench has them.
  Boot boot = Boot::kNormal;
  std::string old_version;
  std::uint32_t boot_delay_ms = 1500;
  std::uint64_t restart_at_ms = 0;  // the radio restarts itself at this time (0 = never)
  std::uint64_t silence_at_ms = 0;  // the radio goes silent at this time (0 = never)

  // What the bench saw.
  unsigned seen_resets = 0;
  bool boot_pending = false;
  std::uint64_t boot_due_ms = 0;
  bool radio_runs = false;
  std::string running_version;
  int boots = 0;

  ChipRadio(fakes::FakeClock& the_clock, sim::ScriptedRadio& the_radio) : clock(the_clock), radio(the_radio) {
    chip.installSoftDevice();
    chip.onlyTouch(profile.app.begin, profile.app.end);
    chip.alsoTouch(profile.settings_page, profile.settings_page + 4096);
    seen_resets = chip.counters().resets;
  }

  // Puts an image's bytes on the chip as an earlier flash would have left them: the bytes, and a
  // settings record the bootloader accepts (bank 0 valid, CRC check off, the image's size).
  void installOnChip(const Candidate& image) {
    const std::uint32_t load = profile.app.begin;
    for (std::size_t i = 0; i < image.file.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.file.size(); ++k) {
        w |= static_cast<std::uint32_t>(image.file[i + k]) << (8 * k);
      }
      for (std::size_t k = image.file.size() - i; k < 4; ++k) w |= 0xFFu << (8 * k);
      chip.setFlashWord(static_cast<std::uint32_t>(load + i), w);
    }
    namespace bs = core::flasher::bootloader_settings;
    std::uint32_t words[bs::kRecordWords];
    bs::encode(bs::validRecord(static_cast<std::uint32_t>(image.file.size())), words);
    for (std::size_t i = 0; i < bs::kRecordWords; ++i) {
      chip.setFlashWord(profile.settings_page + 4 * static_cast<std::uint32_t>(i), words[i]);
    }
  }

  // The application on the chip, by its descriptor, if the bootloader would start it.
  bool chipApplication(core::image::Fields* out) const {
    if (!chip.bootloaderAccepts(sim::Nrf52Chip::Bootloader::kStock)) return false;
    const std::uint32_t span = 64 * 1024;
    std::vector<std::uint8_t> bytes(span);
    for (std::uint32_t at = 0; at < span; at += 4) {
      const std::uint32_t w = chip.flashWord(profile.app.begin + at);
      bytes[at] = static_cast<std::uint8_t>(w);
      bytes[at + 1] = static_cast<std::uint8_t>(w >> 8);
      bytes[at + 2] = static_cast<std::uint8_t>(w >> 16);
      bytes[at + 3] = static_cast<std::uint8_t>(w >> 24);
    }
    const core::image::Found found = core::image::find(bytes.data(), bytes.size());
    if (found.count != 1) return false;
    if (out != nullptr) *out = found.fields;
    return true;
  }

  bool chipHolds(const Candidate& image) const {
    for (std::size_t i = 0; i < image.file.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.file.size(); ++k) {
        w |= static_cast<std::uint32_t>(image.file[i + k]) << (8 * k);
      }
      for (std::size_t k = image.file.size() - i; k < 4; ++k) w |= 0xFFu << (8 * k);
      if (chip.flashWord(static_cast<std::uint32_t>(profile.app.begin + i)) != w) return false;
    }
    return true;
  }

  // The radio boots into whatever the chip holds, with the knob applied.
  void radioBoots() {
    ++boots;
    radio.override_status_flags = false;
    radio.refuseType(0, 0, 0);
    radio.ignoreType(0, 0);
    radio.scanner = false;
    core::image::Fields f;
    if (!chipApplication(&f)) {
      radio.deaf = true;
      radio_runs = false;
      running_version.clear();
      return;
    }
    radio_runs = true;
    running_version = f.version;
    radio.fw_version = f.version;
    radio.board = f.board;
    radio.role = static_cast<std::uint8_t>(f.role);
    radio.deaf = false;
    if (f.role == core::image::Role::kScanner) {
      radio.scanner = true;
      if (boot == Boot::kSilent) radio.deaf = true;
      radio.restart(false);
      return;
    }
    switch (boot) {
      case Boot::kNormal: break;
      case Boot::kSilent: radio.deaf = true; break;
      case Boot::kOldVersion: radio.fw_version = old_version; break;
      case Boot::kWrongRole: radio.role = core::link::role::kBridge; break;
      case Boot::kWrongBoard: radio.board = "rak4630-dual-a"; break;
      case Boot::kRefuseTime: radio.refuseType(core::link::type::kTimeSet, core::link::status::kRange, 1); break;
      case Boot::kNotInitialised:
        radio.override_status_flags = true;
        radio.status_flags = 0;
        break;
      case Boot::kNoPower: radio.ignoreType(core::link::type::kPowerGet, 100); break;
      case Boot::kRefuseHello: radio.refuseType(core::link::type::kHello, core::link::status::kUnsupported, 100); break;
    }
    radio.restart(!radio.deaf);
  }

  // The world's own time passing: a reset of the chip is a radio that boots after a delay; the
  // restart and silence knobs fire at their times.
  void settle() {
    const std::uint64_t now = clock.nowMs();
    if (chip.counters().resets != seen_resets) {
      seen_resets = chip.counters().resets;
      radio.deaf = true;
      radio_runs = false;
      boot_pending = true;
      boot_due_ms = now + boot_delay_ms;
    }
    if (boot_pending && now >= boot_due_ms) {
      boot_pending = false;
      radioBoots();
    }
    if (restart_at_ms != 0 && now >= restart_at_ms) {
      restart_at_ms = 0;
      if (radio_runs) radio.restart(true);
    }
    if (silence_at_ms != 0 && now >= silence_at_ms) {
      silence_at_ms = 0;
      radio.deaf = true;
    }
  }
};

}  // namespace bench
