// The duty cycle on a bench of simulators (task p3-04), built the way task p2-03's update bench
// is: one **world** that outlives a Walter - the clock, the retained map that is RTC memory (a
// sleep keeps it, a power cut does not), the journal's flash, the scripted radio and the fake
// modem - and a **Walter** over it that can be destroyed and built again, which is what a wake
// from deep sleep *is*. `boot()` is a wake.
//
// What a test turns: which source woke the board (`world.woke`), the reset reason the chip
// reports (`world.reset`), whether a USB host is there (`world.usb`), what the radio answers
// POWER_GET with (`world.rak.battery_mv`, `battery_ma`, `power_valid`), and whatever §8
// attention the radio raises.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/cycle/duty_cycle.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/power/interval_policy.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/journal.h"
#include "core/update/last_good_battery.h"
#include "core/update/link_battery.h"
#include "core/uplink/policy.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_sleeper.h"
#include "fakes/fake_wall_clock.h"
#include "fakes/sim_key_value.h"
#include "sim/fake_modem.h"
#include "sim/scripted_radio.h"
#include "sim/sim_flash.h"

namespace bench {

namespace cyc = core::cycle;
namespace pwr = core::power;

// **The Walter's own monotonic clock.** ESP-IDF's `esp_timer` restarts at zero at every wake
// from deep sleep - "Upon wakeup from deep sleep, the initialization timer restarts from zero",
// docs/en/api-reference/system/esp_timer.rst at v5.5.5, and `interfaces/clock.h` says so - so a
// Walter's `nowMs()` is the world's minus the moment its own boot began. The delays go through
// to the world, because that is where the radio and the attention's 300 ms live.
//
// Without this the bench would be a Walter whose clock had been running for a month, and every
// "how long was this wake" would be "a month".
class BootClock final : public interfaces::Clock {
 public:
  explicit BootClock(fakes::FakeClock& world) : world_(world), base_(world.nowMs()) {}
  std::uint64_t nowMs() override { return world_.nowMs() - base_; }
  void delayMs(std::uint32_t ms) override { world_.delayMs(ms); }
  void delayUs(std::uint32_t us) override { world_.delayUs(us); }

 private:
  fakes::FakeClock& world_;
  const std::uint64_t base_;
};

struct CycleWorld {
  fakes::FakeClock clock;
  // RTC memory: it survives a deep sleep, a reset, a panic and a watchdog, and a power cut
  // clears it (interfaces/retained.h). The world holds it, so a `boot()` keeps it and
  // `powerCut()` does not.
  fakes::FakeRetained retained;
  sim::SimFlash journal_flash{core::update::kJournalPartitionBytes};
  sim::ScriptedRadio rak{clock};
  fakes::FakeWallClock wall;
  sim::FakeModem modem{clock};
  fakes::SimKeyValue kv;

  // What the next boot looks like.
  interfaces::WokeBy woke = interfaces::WokeBy::kNotFromSleep;
  core::health::ResetReason reset = core::health::ResetReason::kPowerOn;
  bool usb = false;
  int boots = 0;

  CycleWorld() {
    // The radio answers POWER_GET with numbers a test chose, not the fixture's.
    rak.override_power = true;
    // A time of day, so that a sleep can say when it expects to be back (the journal's
    // `at_epoch`). A Walter without one is the other half of the tests below.
    wall.give(1760000000);
  }

  void powerCut() {
    retained.powerCut();
    // A power cut takes the radio down with the Walter: they share the battery.
    rak.forgetPending();
  }
};

// One Walter, from a boot to a sleep.
struct CycleWalter {
  CycleWorld& w;
  // Everything of this Walter runs on *its own* clock, which starts at zero as a boot's does.
  BootClock clock{w.clock};
  fakes::FakePins pins{w.clock};  // the pin rules are about the wires, and the wires do not reboot
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::board::RadioPins board{pins, clock, w.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{board, w.rak};
  core::link::client::Client link{port, clock, w.wall, slots};
  core::health::Health health{w.retained, clock};
  core::update::Journal journal{w.journal_flash};
  core::update::LinkBattery link_battery{link};
  core::update::LastGoodBattery battery{link_battery, clock};
  core::settings::Settings settings{w.kv};
  core::uplink::Settings modem_settings{};
  core::uplink::UplinkPolicy uplink{w.modem, clock, w.retained, modem_settings};
  fakes::FakeSleeper sleeper{w.clock, pins};
  std::unique_ptr<core::supervisor::Supervisor> supervisor;
  std::unique_ptr<cyc::DutyCycle> duty;

  explicit CycleWalter(CycleWorld& world) : w(world) {
    ++w.boots;
    pins.record_log = false;
    w.journal_flash.powerOn();
    // The modem is handed deadlines on *this* Walter's clock, which began now (sim/fake_modem.h).
    w.modem.walterBooted(w.clock.nowMs());
    // The order main/ uses: the pins safe, the holds released, the wake cause read, safe again.
    // Nothing here has to release a hold - the fake pins keep theirs across a `boot()` the way
    // real pads keep theirs across a sleep - but doing it in this order is what the firmware
    // does, and the fake pins check the transitions.
    (void)board.releaseHolds();
    board.safe();
    sleeper.wake_as = w.woke;
    sleeper.usb = w.usb;
    health.begin(w.reset, w.woke == interfaces::WokeBy::kTimer
                              ? core::health::WakeCause::kTimer
                              : w.woke == interfaces::WokeBy::kRadioLine
                                    ? core::health::WakeCause::kPin
                                    : core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    settings.begin();
    REQUIRE(journal.begin() == core::update::JournalError::kNone);
    uplink.begin();
    REQUIRE(port.open() == core::board::Outcome::kOk);
    supervisor.reset(new core::supervisor::Supervisor(link, board, slots, health, battery, clock,
                                                      w.wall, w.retained, nullptr, nullptr,
                                                      nullptr, &journal));
    supervisor->begin();
    duty.reset(new cyc::DutyCycle(sleeper, board, link, uplink, health, clock, w.wall, restart,
                                  w.retained, supervisor.get(), nullptr, nullptr, &journal,
                                  &settings));
  }
  ~CycleWalter() { CHECK(pins.violations.empty()); }

  // What main/ does after the link's port is open: work out why we are awake. For a wake on the
  // radio's line this listens for up to §8's whole repeat window.
  void begin() { duty->begin(); }

  const cyc::WakeInfo& wake() const { return duty->wake(); }
};

// A world and the Walter on it, with `boot()` for a wake.
struct CycleBench {
  CycleWorld world;
  std::unique_ptr<CycleWalter> walter;

  CycleBench() { walter = std::make_unique<CycleWalter>(world); }

  // A wake: the Walter is destroyed and built again over the same world, exactly as a wake from
  // deep sleep destroys and rebuilds one. `woke` is what the chip will say woke it;
  // `requested` is who asked for a software reset, which the *previous* life records before it
  // restarts (core/health/health.h, noteRestart) - which is why it is said here and not in the
  // new Walter's constructor.
  void boot(interfaces::WokeBy woke = interfaces::WokeBy::kTimer,
            core::health::ResetReason reset = core::health::ResetReason::kDeepSleepWake,
            core::health::Restart requested = core::health::Restart::kNotRequested) {
    if (walter && requested != core::health::Restart::kNotRequested) {
      walter->health.noteRestart(requested);
    }
    world.woke = woke;
    world.reset = reset;
    walter.reset();
    walter = std::make_unique<CycleWalter>(world);
  }

  CycleWalter& v() { return *walter; }
  cyc::DutyCycle& duty() { return *walter->duty; }
  const cyc::WakeInfo& wake() { return walter->duty->wake(); }

  // A whole wake, as a test walks one: boot, work out why, read the battery, decide, sleep.
  cyc::SleepResult oneWake(interfaces::WokeBy woke = interfaces::WokeBy::kTimer) {
    boot(woke);
    walter->begin();
    (void)duty().readPower();
    (void)duty().decide();
    return duty().sleepNow();
  }

  std::string runAndCapture(const std::function<void()>& what) {
    walter->out.clear();
    what();
    return walter->out.text;
  }

  // How many journal entries of the duty cycle's kind are in flash, newest first.
  std::vector<cyc::Payload> sleepEntries() {
    std::vector<cyc::Payload> found;
    core::update::Cursor cursor;
    core::update::Entry e;
    bool more = walter->journal.first(&cursor, &e);
    while (more && found.size() < 64) {
      cyc::Payload p;
      if (e.kind == core::update::kind::kDutyCycle && cyc::decode(e, &p)) found.push_back(p);
      more = walter->journal.previous(&cursor, &e);
    }
    return found;
  }
};

}  // namespace bench
