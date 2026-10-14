// The radio's update on a bench of simulators (task p2-03): one *world* that outlives a Walter -
// the image store's flash, the journal's flash, the simulated nRF52840 the images are flashed
// into, the scripted radio that answers over the UART, and one clock for all of them - and a
// *Walter* that can be destroyed and built again over it, which is what a reset in the middle of
// an update is. The radio on the bench runs whatever the chip's flash holds: when the flasher
// resets the chip, the bench reads the descriptor out of the application region and, if the
// bootloader would start it, the scripted radio comes up with that version, board and role
// (or as NoiseScope, for a scanner image) after a boot delay; with no startable application it
// stays silent, as a radio in its bootloader does.
//
// Everything a test wants to go wrong is a knob here: how the radio behaves when it boots, a
// restart or a silence at a chosen moment of the proving period, the battery's reading, and the
// Walter dying right after the Nth journal entry.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/flasher/bootloader_settings.h"
#include "core/flasher/flasher.h"
#include "core/health/health.h"
#include "core/image/descriptor.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/wire.h"
#include "core/update/battery.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/image/store_bench.h"
#include "fakes/capture.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "sim/nrf52_chip.h"
#include "sim/scripted_radio.h"
#include "sim/sim_flash.h"
#include "sim/swd_target.h"

namespace bench {

namespace upd = core::update;
using upd::Kind;
using upd::Reason;
using upd::State;

// A battery a test sets: known or not, its millivolts, its trend.
class FakeBattery final : public upd::BatterySource {
 public:
  bool known = true;
  std::uint16_t millivolts = 4000;
  bool trend_known = false;
  std::int16_t mv_per_hour = 0;
  int reads = 0;

  bool read(upd::BatteryReading* out) override {
    ++reads;
    if (!known) return false;
    out->millivolts = millivolts;
    out->trend_known = trend_known;
    out->mv_per_hour = mv_per_hour;
    return true;
  }
};

// How the radio behaves when it boots into an image.
enum class Boot {
  kNormal,          // what the descriptor says, answering everything
  kSilent,          // never answers (an image whose link never comes up)
  kOldVersion,      // HELLO says the version in `old_version` (the flash did not take)
  kWrongRole,       // HELLO says bridge
  kWrongBoard,      // HELLO says another board
  kRefuseTime,      // TIME_SET refused with ERR_RANGE
  kNotInitialised,  // STATUS flags without bit 4
  kNoPower,         // POWER_GET never answers
  kRefuseHello,     // every HELLO refused ERR_UNSUPPORTED: §5.1, a radio that does not speak this major
};

struct UpdateWorld {
  StoreBench store;  // the image store over a simulated flash, its clock, its wall clock
  fakes::FakeClock& clock = store.clock;
  sim::SimFlash journal_flash{upd::kJournalPartitionBytes};
  sim::Nrf52Chip chip{clock};
  sim::ScriptedRadio radio{clock};
  const core::flasher::BoardProfile& profile = core::flasher::kRak4630Single;

  // The knobs.
  Boot boot = Boot::kNormal;
  std::string old_version;
  std::uint32_t boot_delay_ms = 1500;
  std::uint64_t restart_at_ms = 0;  // the radio restarts itself at this time (0 = never)
  std::uint64_t silence_at_ms = 0;  // the radio goes silent at this time (0 = never)
  bool dead = false;                // the Walter has died: nothing of it works until it is rebuilt

  // What the bench saw.
  unsigned seen_resets = 0;
  bool boot_pending = false;
  std::uint64_t boot_due_ms = 0;
  bool radio_runs = false;
  std::string running_version;
  int boots = 0;

  UpdateWorld() {
    chip.installSoftDevice();
    chip.onlyTouch(profile.app.begin, profile.app.end);
    chip.alsoTouch(profile.settings_page, profile.settings_page + 4096);
    seen_resets = chip.counters().resets;
    // A time to give: the health check's TIME_SET is a real one.
    store.wall.give(1760000000);
    // The world settles whenever a Walter waits, and whenever it touches the UART: the radio
    // boots, restarts or goes silent on cue, so that a caller that only steps the machine and
    // delays sees a live radio - and so that a HELLO sent right after the flasher's reset, before
    // any delay, is not answered by the firmware the chip no longer holds (the scripted radio
    // answers inside write(); a real radio is dark for a second after its reset).
    clock.on_delay = [this](std::uint32_t) { settle(); };
    radio.on_access = [this] { settle(); };
  }

  // Puts an image's bytes on the chip as an earlier flash would have left them: the bytes, and a
  // settings record the bootloader accepts (bank 0 valid, CRC check off, the image's size).
  void installOnChip(const Candidate& image) {
    const std::uint32_t load = profile.app.begin;
    for (std::size_t i = 0; i < image.file.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.file.size(); ++k) w |= static_cast<std::uint32_t>(image.file[i + k]) << (8 * k);
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
    // The descriptors of the fixture images sit near the start; 64 KB covers every image this
    // bench flashes, and scanning the whole region at every boot would be slow for nothing.
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

  // Whether the chip holds exactly this image's bytes at the application's start.
  bool chipHolds(const Candidate& image) const {
    for (std::size_t i = 0; i < image.file.size(); i += 4) {
      std::uint32_t w = 0;
      for (unsigned k = 0; k < 4 && i + k < image.file.size(); ++k) w |= static_cast<std::uint32_t>(image.file[i + k]) << (8 * k);
      for (std::size_t k = image.file.size() - i; k < 4; ++k) w |= 0xFFu << (8 * k);
      if (chip.flashWord(static_cast<std::uint32_t>(profile.app.begin + i)) != w) return false;
    }
    return true;
  }

  // The radio boots into whatever the chip holds, with the knob applied.
  void radioBoots() {
    ++boots;
    // A new boot has no memory of the last firmware's quirks: the per-type refusals and
    // silences a knob set are cleared, and the knob applied again below.
    radio.override_status_flags = false;
    radio.refuseType(0, 0, 0);
    radio.ignoreType(0, 0);
    radio.scanner = false;
    core::image::Fields f;
    if (!chipApplication(&f)) {
      radio.deaf = true;  // in its bootloader: nothing answers on the link
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
      radio.scanner = true;  // NoiseScope: lines, no frames (§9)
      if (boot == Boot::kSilent) radio.deaf = true;  // a scanner build whose console never comes up
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
    radio.restart(!radio.deaf);  // EV_BOOT on the wire, unless nothing is
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

// One life of the Walter over the world: fresh pins, a fresh debug port, a fresh flasher, link
// client, journal object, update machine and console - the flashes under them are the world's.
struct Walter {
  UpdateWorld& w;
  fakes::FakePins pins{w.clock};
  sim::SwdTarget target{pins};
  fakes::FakeRetained retained;
  core::board::RadioPins board{target, w.clock, retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::flasher::Flasher flasher{board, w.clock, slots};
  core::link::client::LinkPort port{board, w.radio};
  core::link::client::Client client{port, w.clock, w.store.wall, slots};
  upd::Journal journal{w.journal_flash};
  FakeBattery battery;
  upd::RadioUpdate update{w.store.store, flasher, client, journal, w.clock, battery};
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::health::Health health{retained, w.clock};
  core::image::Receiver receiver{w.store.store, w.store.verdict, out, w.clock};
  core::console::Console console{out, board, health, w.clock, restart, client, slots,
                                 &w.store.store, &receiver, &update, &journal};
  std::vector<upd::StepResult> transitions;

  explicit Walter(UpdateWorld& world) : w(world) {
    w.dead = false;
    w.radio.refuse_open = false;
    w.journal_flash.powerOn();
    w.store.flash.powerOn();
    REQUIRE(w.store.store.begin() == StoreError::kNone);
    w.chip.attachTo(target);
    w.chip.on_power_loss = [this] { target.silent = true; };
    pins.record_log = false;
    health.begin(core::health::ResetReason::kPowerOn, core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
    REQUIRE(journal.begin() == upd::JournalError::kNone);
    port.open();  // as main does at start-up, so that an EV_BOOT is heard
  }
  ~Walter() { CHECK(pins.violations.empty()); }

  // The Walter dies now: its flashes stop answering, its debug port and its UART go dead. The
  // machine's current step fails on whatever it touches next and the loop below sees `dead`.
  void die() {
    w.dead = true;
    w.journal_flash.powerOff();
    w.store.flash.powerOff();
    target.silent = true;
    w.radio.refuse_open = true;
    port.close();
  }

  // Steps the machine until it is terminal (or the Walter is dead), delaying between steps that
  // only waited - the world settles inside each delay. `on_step` sees every result.
  State run(const std::function<void(const upd::StepResult&)>& on_step = nullptr, int max_steps = 50000) {
    int steps = 0;
    while (update.active() && !w.dead && steps++ < max_steps) {
      const upd::StepResult s = update.step();
      if (s.changed) transitions.push_back(s);
      if (on_step) on_step(s);
      if (!s.changed && s.reason == Reason::kJournal) break;
      if (s.waited) w.clock.delayMs(100);
    }
    CHECK(steps < max_steps);
    return update.state();
  }

  std::string transitionText() const {
    std::string text;
    for (const upd::StepResult& s : transitions) {
      text += std::string(upd::describe(s.before)) + " -> " + upd::describe(s.after) + ": " + s.note + "\n";
    }
    return text;
  }

  std::string runLine(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }
};

// A unit as the update finds it: the golden image in the store and on the chip, running; the
// candidate in the store.
struct ReadyUnit {
  UpdateWorld world;
  Candidate golden{"update-golden"};
  Candidate candidate{"update-candidate"};
  std::unique_ptr<Walter> walter;

  ReadyUnit(bool with_candidate = true, bool golden_present = true) {
    if (golden_present) REQUIRE(world.store.put(Which::kGolden, Slot{0}, golden) == StoreError::kNone);
    if (with_candidate) REQUIRE(world.store.put(Which::kCandidate, Slot{0}, candidate) == StoreError::kNone);
    if (golden_present) world.installOnChip(golden);
    world.radioBoots();
    walter = std::make_unique<Walter>(world);
  }

  // The Walter reset: destroyed and built again over the same world.
  void reboot() {
    walter.reset();
    walter = std::make_unique<Walter>(world);
  }

  upd::Request request(Kind kind = Kind::kUpdate) const {
    upd::Request r;
    r.kind = kind;
    r.slot = Slot{0};
    return r;
  }

  Record read(Which which) const { return world.store.read(which, Slot{0}); }
  std::string goldenVersion() const {
    const Record r = read(Which::kGolden);
    return r.present ? std::string(r.version) : std::string("(none)");
  }
};

}  // namespace bench
