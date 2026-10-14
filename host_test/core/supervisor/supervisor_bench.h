// The supervisor's bench (task p2-05): task p2-03's world - the store's flash, the journal's
// flash, the simulated nRF52840, the scripted radio and one clock for all of them - with two
// things added that the ladder needs and an update does not:
//
//   - **retained storage that outlives a Walter.** The day's caps are kept in RTC memory, which
//     a reset and a deep sleep survive and a power cut does not (interfaces/retained.h), so the
//     FakeRetained lives in the world here and not in the Walter. `powerCut()` is the power cut.
//   - **a world that reacts to the pins.** A reset pulse and a kill cycle are what rungs 2 and 3
//     do, and neither touches the chip, so task p2-03's `settle()` - which watches the chip's
//     reset counter - cannot see them. The fake pins call back as each ends (fakes/fake_pins.h:
//     on_reset_pulse, on_kill_cycle) and the radio here goes dark and boots again, as a real one
//     does. Whether it *answers* when it boots is the `Boot` knob p2-03 already has, and the
//     `cure` below is how a test says which rung brings it back.
//
// The radio on this bench still runs whatever the chip's flash holds, so rung 4 needs no knob at
// all: wipe the application region and the radio is silent because its firmware is gone, which no
// reset and no power cycle can cure and a reflash of the golden image can.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/supervisor/supervisor.h"
#include "core/update/last_good_battery.h"
#include "core/update/update_bench.h"

namespace bench {

namespace sup = core::supervisor;
using sup::Outcome;
using sup::Rung;
using sup::RungOutcome;
using sup::Silence;

// Which rung brings the radio back. kNone: nothing does.
struct SupWorld {
  UpdateWorld u;
  // RTC memory: one of these for the unit, not one per Walter.
  fakes::FakeRetained retained;

  // What cures the radio, and when rung 1's "it came back by itself" happens.
  Rung cure = Rung::kNone;
  std::uint64_t cure_at_ms = 0;  // for Rung::kAskAgain: the time it starts answering again
  bool cured = false;
  // The fake pins of the Walter that is alive now, so that a reset or a kill reaches the world.
  fakes::FakePins* pins = nullptr;

  unsigned reset_pulses_seen = 0;
  unsigned kill_cycles_seen = 0;

  SupWorld() {
    // Everything of p2-03's world passes through its settle(); the cure is applied on both sides
    // of it, so that a boot which falls due inside settle() already knows whether the firmware
    // answers, and a radio that is running when the cure arrives stops being deaf at once.
    u.clock.on_delay = [this](std::uint32_t) { settle(); };
    u.radio.on_access = [this] { settle(); };
  }

  void settle() {
    applyCure();
    u.settle();
    applyCure();
  }

  void applyCure() {
    if (!cured && cure == Rung::kAskAgain && cure_at_ms != 0 && u.clock.nowMs() >= cure_at_ms) {
      cured = true;
    }
    if (cured && relapse_after_ms != 0 && relapse_at_ms == 0) {
      relapse_at_ms = u.clock.nowMs() + relapse_after_ms;
    }
    if (cured && relapse_at_ms != 0 && u.clock.nowMs() >= relapse_at_ms) {
      cured = false;
      relapse_at_ms = 0;
      goSilent();
      return;
    }
    if (scanner_on_chip) {
      // A boot of the radio into a scanner image says nothing on the wire: NoiseScope speaks
      // NDJSON and has no EV_BOOT to send (§9), so whatever p2-03's radioBoots() queued for a
      // repeater is dropped here - and that is what lets this Walter's next session find text.
      if (u.boots != scanner_boots_seen) {
        scanner_boots_seen = u.boots;
        u.radio.forgetPending();
      }
      if (u.radio_runs) {
        u.radio.scanner = true;
        u.radio.deaf = false;
      }
      return;
    }
    scanner_boots_seen = u.boots;
    if (!cured) return;
    u.boot = Boot::kNormal;
    if (u.radio_runs) u.radio.deaf = false;
  }

  // The radio as a reset pulse and a kill cycle leave it: dark, and booting again after the boot
  // delay into whatever the chip holds.
  void radioRestarts() {
    u.radio.deaf = true;
    u.radio_runs = false;
    u.boot_pending = true;
    u.boot_due_ms = u.clock.nowMs() + u.boot_delay_ms;
  }

  void onResetPulse() {
    ++reset_pulses_seen;
    if (cure == Rung::kReset) cured = true;
    radioRestarts();
  }

  void onKillCycle() {
    ++kill_cycles_seen;
    if (cure == Rung::kPowerCycle) cured = true;
    radioRestarts();
  }

  // The radio relapses: this long after it was cured it goes quiet again (0 = never). What a
  // radio that is dying rather than hung does, and what makes the caps matter.
  std::uint32_t relapse_after_ms = 0;
  std::uint64_t relapse_at_ms = 0;

  // The radio stops answering: it runs, but nothing comes back - and every boot from here is the
  // same, until a cure.
  void goSilent() {
    u.boot = Boot::kSilent;
    u.radio.deaf = true;
  }

  // The radio is running NoiseScope (§9): it answers no frame, and nothing of the firmware it
  // was running is still in flight - a module whose application was replaced has no EV_BOOT of
  // the old one's left to send, which is what makes §9's text detection possible at all.
  void runScanner() {
    scanner_on_chip = true;
    u.radio.deaf = false;
    u.radio_runs = true;
    u.radio.forgetPending();
    applyCure();
  }
  // Kept on across a boot of the radio, because what a radio runs is what its chip holds: p2-03's
  // radioBoots() reads the descriptor out of the application region and would make it a repeater
  // again, and a reset must not un-NoiseScope a module.
  bool scanner_on_chip = false;
  int scanner_boots_seen = 0;

  // The application region wiped: the bootloader starts nothing, so the radio is dark for a
  // reason only a reflash can mend.
  void wipeApplication() {
    u.chip.fillFlash(u.profile.app.begin, u.profile.app.begin + 0x10000, 0xFFFFFFFFu);
    u.radio.deaf = true;
    u.radio_runs = false;
  }

  // A power cut: RTC memory is gone, the flashes are not.
  void powerCut() { retained.powerCut(); }

  // A wake from deep sleep: ESP-IDF's monotonic clock restarts at zero (interfaces/clock.h),
  // which is the case everything that keeps a time in retained storage has to be ready for.
  // Nothing of the world may be waiting on an absolute time across it.
  void deepSleepWake() {
    u.boot_pending = false;
    u.restart_at_ms = 0;
    u.silence_at_ms = 0;
    relapse_at_ms = 0;
    cure_at_ms = 0;
    u.clock.now_ms = 0;
  }
};

// One life of the Walter over that world: the same objects task p2-03's bench builds, plus the
// battery's cache and the supervisor itself.
struct SupWalter {
  SupWorld& s;
  UpdateWorld& w;
  fakes::FakePins pins{s.u.clock};
  sim::SwdTarget target{pins};
  core::board::RadioPins board{target, s.u.clock, s.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::flasher::Flasher flasher{board, s.u.clock, slots};
  core::link::client::LinkPort port{board, s.u.radio};
  core::link::client::Client client{port, s.u.clock, s.u.store.wall, slots};
  upd::Journal journal{s.u.journal_flash};
  FakeBattery raw;
  upd::LastGoodBattery battery{raw, s.u.clock};
  upd::RadioUpdate update{s.u.store.store, flasher, client, journal, s.u.clock, battery};
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::health::Health health{s.retained, s.u.clock};
  core::image::Receiver receiver{s.u.store.store, s.u.store.verdict, out, s.u.clock};
  sup::Supervisor supervisor{client,        board,     slots,       health,   battery,
                             s.u.clock,     s.u.store.wall, s.retained, &s.u.store.store,
                             &flasher,      &update,   &journal};
  core::console::Console console{out,  board,  health,   s.u.clock, restart, client,
                                 slots, &s.u.store.store, &receiver, &update, &journal,
                                 nullptr, &supervisor};
  std::vector<sup::Step> steps;
  std::vector<std::uint64_t> step_ms;  // the clock at each step: what the waits are asserted on
  // When each rung was done or refused, in order. The waits between the rungs are the gaps here.
  std::vector<std::pair<Rung, std::uint64_t>> rung_ms;

  // `reason` is how this Walter came up: a power-on is the one the glitch of ARCHITECTURE §2
  // rule 5 belongs to, and the supervisor reads it to tell "our own power-up reset the radio"
  // from "the radio is at fault".
  explicit SupWalter(SupWorld& world,
                     core::health::ResetReason reason = core::health::ResetReason::kSoftware)
      : s(world), w(world.u) {
    w.dead = false;
    w.radio.refuse_open = false;
    w.journal_flash.powerOn();
    w.store.flash.powerOn();
    REQUIRE(w.store.store.begin() == StoreError::kNone);
    w.chip.attachTo(target);
    w.chip.on_power_loss = [this] { target.silent = true; };
    pins.on_reset_pulse = [this] { s.onResetPulse(); };
    pins.on_kill_cycle = [this] { s.onKillCycle(); };
    s.pins = &pins;
    health.begin(reason, core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
    REQUIRE(journal.begin() == upd::JournalError::kNone);
    port.open();
    // What main/ does at start-up, in the order it does it: the console's begin() is where the
    // boot lines are printed, a journalled update is resumed and the supervisor reads its
    // retained record (core/console/console.h).
    console.begin();
  }
  ~SupWalter() {
    CHECK(pins.violations.empty());
    if (s.pins == &pins) s.pins = nullptr;
  }

  // Steps the supervisor until it is not climbing any more, delaying between steps that only
  // waited - the world settles inside each delay, as it does for an update.
  // `idle_ms` is what a step that only waited sleeps for. The console uses kStepIdleMs (a tenth
  // of a second, which is what a person at a bench wants); a test that runs a simulated month
  // passes something coarser, because the waits are measured by the clock and a coarser step
  // changes nothing about them - only how many times the loop turns.
  sup::State climb(int max_steps = 200000, std::uint32_t idle_ms = sup::kStepIdleMs) {
    int n = 0;
    for (;;) {
      const sup::Step step = supervisor.step();
      steps.push_back(step);
      step_ms.push_back(w.clock.nowMs());
      if (step.rung != Rung::kNone && step.rung_outcome != RungOutcome::kNotTried &&
          (rung_ms.empty() || rung_ms.back().first != step.rung)) {
        rung_ms.emplace_back(step.rung, w.clock.nowMs());
      }
      if (!supervisor.climbing()) break;
      if (step.waited) w.clock.delayMs(idle_ms);
      if (++n >= max_steps) {
        FAIL("the supervisor did not stop climbing");
        break;
      }
    }
    return supervisor.state();
  }

  // `rak ladder`, from the outside: begin a climb and run it to its end. A climb already in
  // hand (a heartbeat that found a boot loop, say) is simply carried on with.
  sup::State ladder(std::uint32_t idle_ms = sup::kStepIdleMs) {
    if (!supervisor.climbing()) REQUIRE(supervisor.runLadder());
    return climb(200000, idle_ms);
  }

  // One heartbeat, with the clock moved past the interval so that it is due.
  sup::Step beat() {
    w.clock.advance(sup::kHeartbeatIntervalMs);
    return supervisor.step();
  }

  // When a rung was done, or 0 when it was not reached.
  std::uint64_t whenRung(Rung rung) const {
    for (const auto& at : rung_ms) {
      if (at.first == rung) return at.second;
    }
    return 0;
  }

  // Which rungs the recorded steps tried, in order, and what came of each.
  std::vector<std::pair<Rung, RungOutcome>> rungs() const {
    std::vector<std::pair<Rung, RungOutcome>> out;
    for (const sup::Step& st : steps) {
      if (st.rung == Rung::kNone || st.rung_outcome == RungOutcome::kNotTried) continue;
      if (!out.empty() && out.back().first == st.rung && out.back().second == st.rung_outcome) continue;
      out.emplace_back(st.rung, st.rung_outcome);
    }
    return out;
  }

  std::string stepText() const {
    std::string text;
    for (const sup::Step& st : steps) {
      if (st.rung != Rung::kNone && st.rung_outcome != RungOutcome::kNotTried) {
        text += std::string(sup::describe(st.rung)) + ": " + sup::describe(st.rung_outcome) + " - " +
                st.note + "\n";
      } else if (st.changed) {
        text += std::string(sup::describe(st.before)) + " -> " + sup::describe(st.after) + ": " +
                st.note + "\n";
      }
    }
    return text;
  }

  std::string runLine(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }

  // How many supervisor entries the journal holds.
  std::size_t supervisorEntries() {
    std::size_t n = 0;
    upd::Cursor cursor;
    upd::Entry e;
    bool more = journal.first(&cursor, &e);
    while (more) {
      sup::Payload p;
      if (sup::decode(e, &p)) ++n;
      more = journal.previous(&cursor, &e);
    }
    return n;
  }

  // The supervisor entries, oldest first.
  std::vector<sup::Payload> story() {
    std::vector<sup::Payload> out_entries;
    upd::Cursor cursor;
    upd::Entry e;
    bool more = journal.first(&cursor, &e);
    while (more) {
      sup::Payload p;
      if (sup::decode(e, &p)) out_entries.push_back(p);
      more = journal.previous(&cursor, &e);
    }
    std::vector<sup::Payload> ordered;
    for (std::size_t i = out_entries.size(); i > 0; --i) ordered.push_back(out_entries[i - 1]);
    return ordered;
  }
};

// A unit the supervisor finds as task p2-03's good path leaves it: the golden image in the store
// and on the chip, the radio running it and answering, the candidate slot empty.
struct WatchedUnit {
  SupWorld world;
  Candidate golden{"update-golden"};
  std::unique_ptr<SupWalter> walter;

  // `past_power_up`: the clock moved past kWalterPowerUpGraceMs, so that a silence is not read as
  // "this Walter has only just powered up" (ARCHITECTURE §2 rule 5). Every test but the one about
  // that rule wants it.
  explicit WatchedUnit(bool past_power_up = true, bool golden_present = true) {
    if (golden_present) {
      REQUIRE(world.u.store.put(Which::kGolden, Slot{0}, golden) == StoreError::kNone);
      world.u.installOnChip(golden);
    }
    world.u.radioBoots();
    if (past_power_up) world.u.clock.advance(sup::kWalterPowerUpGraceMs + 1);
    walter = std::make_unique<SupWalter>(world);
  }

  // The Walter reset: destroyed and built again over the same world and the same RTC memory.
  void reboot(core::health::ResetReason reason = core::health::ResetReason::kSoftware) {
    walter.reset();
    walter = std::make_unique<SupWalter>(world, reason);
  }

  // The Walter's power pulled: RTC memory gone, the journal and the store still there.
  void powerCut() {
    walter.reset();
    world.powerCut();
    walter = std::make_unique<SupWalter>(world, core::health::ResetReason::kPowerOn);
    world.u.clock.advance(sup::kWalterPowerUpGraceMs + 1);
  }

  const sup::Status& status() const { return walter->supervisor.status(); }
  const sup::Incident& incident(std::size_t i = 0) const {
    return walter->supervisor.incidentAt(i);
  }
};

}  // namespace bench
