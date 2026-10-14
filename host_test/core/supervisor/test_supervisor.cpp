// The supervisor (task p2-05): the heartbeat, the four kinds of silence, and the ladder - each
// rung reached only after the one below it failed, each followed by its wait, and the climb
// stopping the moment the radio comes back. Everything here is about something going wrong, so
// everything here is a radio that misbehaves on cue (host_test/core/supervisor/supervisor_bench.h).
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/link/codec/wire.h"
#include "core/supervisor/supervisor.h"
#include "supervisor_bench.h"

using bench::Outcome;
using bench::Rung;
using bench::RungOutcome;
using bench::Silence;
using bench::Slot;
using bench::SupWalter;
using bench::WatchedUnit;
using bench::Which;
namespace sup = core::supervisor;
namespace upd = core::update;

namespace {

// Erases and writes on the chip: whether anything at all was flashed.
unsigned flashOps(const sim::Nrf52Chip& chip) {
  return chip.countLog(sim::FlashEvent::Kind::kErasePage) + chip.countLog(sim::FlashEvent::Kind::kWrite);
}

}  // namespace

TEST_CASE("a healthy radio for a simulated week asks and does nothing else") {
  WatchedUnit unit;
  SupWalter& walter = *unit.walter;

  // A heartbeat a minute for seven days.
  const int beats = 7 * 24 * 60;
  for (int i = 0; i < beats; ++i) {
    const sup::Step s = walter.beat();
    CHECK(s.after == sup::State::kIdle);
    CHECK_FALSE(s.changed);
  }
  CHECK(unit.status().health == sup::Health::kHealthy);
  CHECK(unit.status().pings == static_cast<std::uint32_t>(beats));
  CHECK(unit.status().uptime_s > 0);

  // No action and no journal entry: the heartbeat writes nothing, and nothing was done to the
  // radio but asking.
  CHECK(walter.pins.reset_pulses == 0);
  CHECK(walter.pins.kill_cycles == 0);
  CHECK(flashOps(unit.world.u.chip) == 0);
  CHECK(walter.supervisorEntries() == 0);
  CHECK(walter.supervisor.incidentCount() == 0);
  CHECK(unit.status().counts.resets == 0);
  CHECK(unit.status().counts.power_cycles == 0);
  CHECK(unit.status().counts.flashes == 0);
  CHECK(walter.pins.violations.empty());
}

TEST_CASE("silence cured at each rung in turn: only the rungs below it were used") {
  SUBCASE("rung 1: it answers again after the pause") {
    WatchedUnit unit;
    unit.walter->beat();  // healthy once, so there is a battery reading to remember
    unit.world.goSilent();
    unit.world.cure = Rung::kAskAgain;
    // Inside rung 1's pause: after the first asking has run out of attempts (§3's three, twice
    // over, which is seconds of the simulated clock) and before the second asking begins.
    unit.world.cure_at_ms = unit.world.u.clock.nowMs() + sup::kAskAgainPauseMs + 5000;

    CHECK(unit.walter->ladder() == sup::State::kIdle);
    const auto rungs = unit.walter->rungs();
    REQUIRE(rungs.size() >= 1);
    CHECK(rungs[0].first == Rung::kAskAgain);
    CHECK(unit.incident().outcome == Outcome::kCured);
    CHECK(unit.incident().cured_at == Rung::kAskAgain);
    CHECK(unit.incident().rung[0] == RungOutcome::kCured);
    CHECK(unit.incident().rung[1] == RungOutcome::kNotTried);
    // Nothing was done to the radio: no pin moved, nothing was flashed.
    CHECK(unit.walter->pins.reset_pulses == 0);
    CHECK(unit.walter->pins.kill_cycles == 0);
    CHECK(flashOps(unit.world.u.chip) == 0);
    CHECK(unit.status().counts.resets == 0);
  }

  SUBCASE("rung 2: a reset pulse cures it, and the pause came first") {
    WatchedUnit unit;
    unit.walter->beat();
    const std::uint64_t began = unit.world.u.clock.nowMs();
    unit.world.goSilent();
    unit.world.cure = Rung::kReset;

    CHECK(unit.walter->ladder() == sup::State::kIdle);
    CHECK(unit.incident().outcome == Outcome::kCured);
    CHECK(unit.incident().cured_at == Rung::kReset);
    CHECK(unit.walter->pins.reset_pulses == 1);
    CHECK(unit.walter->pins.kill_cycles == 0);
    CHECK(flashOps(unit.world.u.chip) == 0);
    CHECK(unit.status().counts.resets == 1);
    CHECK(unit.status().counts.power_cycles == 0);
    // The waits: rung 1's pause before rung 2, and rung 2's wait before the radio was judged.
    const std::uint64_t ask = unit.walter->whenRung(Rung::kAskAgain);
    const std::uint64_t reset = unit.walter->whenRung(Rung::kReset);
    REQUIRE(ask >= began);
    CHECK(reset - ask >= sup::kAskAgainPauseMs);
    CHECK(unit.status().last_answer_ms - reset >= sup::kAfterResetMs);
  }

  SUBCASE("rung 3: a power cycle cures it, after a reset did not") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();
    unit.world.cure = Rung::kPowerCycle;

    CHECK(unit.walter->ladder() == sup::State::kIdle);
    CHECK(unit.incident().outcome == Outcome::kCured);
    CHECK(unit.incident().cured_at == Rung::kPowerCycle);
    CHECK(unit.incident().rung[0] == RungOutcome::kDone);  // rung 1 was tried and did not cure it
    CHECK(unit.incident().rung[1] == RungOutcome::kDone);  // nor the reset
    CHECK(unit.walter->pins.reset_pulses == 1);
    CHECK(unit.walter->pins.kill_cycles == 1);
    CHECK(flashOps(unit.world.u.chip) == 0);  // the chip was never touched
    CHECK(unit.status().counts.resets == 1);
    CHECK(unit.status().counts.power_cycles == 1);
    CHECK(unit.status().counts.flashes == 0);
    const std::uint64_t reset = unit.walter->whenRung(Rung::kReset);
    const std::uint64_t kill = unit.walter->whenRung(Rung::kPowerCycle);
    CHECK(kill - reset >= sup::kAfterResetMs);
    CHECK(unit.status().last_answer_ms - kill >= sup::kAfterPowerCycleMs);
    // The waits grow with the rungs (docs/supervisor.md's table).
    CHECK(sup::kAfterPowerCycleMs > sup::kAfterResetMs);
  }

  SUBCASE("rung 4: the chip has nothing on it, so the golden image goes back and cures it") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.wipeApplication();  // no startable application: nothing but a reflash can mend it

    CHECK(unit.walter->ladder() == sup::State::kIdle);
    CHECK(unit.incident().outcome == Outcome::kCured);
    CHECK(unit.incident().cured_at == Rung::kGoldenImage);
    CHECK(unit.walter->pins.reset_pulses == 1);
    CHECK(unit.walter->pins.kill_cycles == 1);
    CHECK(unit.status().counts.flashes == 1);
    // The golden image is on the chip again and the radio is running it.
    CHECK(unit.world.u.chipHolds(unit.golden));
    CHECK(unit.world.u.radio_runs);
    CHECK(unit.world.u.running_version == std::string(unit.golden.version));
    // And the store is as it was: a rung is not an update (golden stays golden, nothing is
    // promoted, nothing is on the bad list).
    CHECK(unit.world.u.store.read(Which::kGolden, Slot{0}).present);
    CHECK(unit.world.u.store.store.badCount() == 0);
  }
}

TEST_CASE("the radio coming back mid-climb stops the climb there") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  // It comes back by itself during rung 2's wait - the bench's "remove the jumper" item.
  unit.world.cure = Rung::kAskAgain;
  unit.world.cure_at_ms =
      unit.world.u.clock.nowMs() + sup::kAskAgainPauseMs + sup::kAfterResetMs - 2000;

  CHECK(unit.walter->ladder() == sup::State::kIdle);
  CHECK(unit.incident().outcome == Outcome::kCured);
  CHECK(unit.incident().cured_at == Rung::kReset);
  CHECK(unit.walter->pins.kill_cycles == 0);  // rung 3 was never reached
  CHECK(unit.incident().rung[2] == RungOutcome::kNotTried);
}

TEST_CASE("silence that nothing cures: the ladder runs once, stops, and stays stopped") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();        // every boot of it from here is deaf
  unit.world.wipeApplication();  // ... and its chip is empty, so rung 4 flashes

  CHECK(unit.walter->ladder() == sup::State::kStopped);
  CHECK(unit.status().stopped);
  // Everything was tried: the golden image went back and the radio still does not answer, which
  // docs/ARCHITECTURE.md §8 calls a hardware fault.
  CHECK(unit.status().stop_reason == sup::StopReason::kEverythingTried);
  CHECK(unit.incident().outcome == Outcome::kStopped);
  CHECK(unit.incident().rungs_tried == (sup::rungBit(Rung::kAskAgain) | sup::rungBit(Rung::kReset) |
                                        sup::rungBit(Rung::kPowerCycle) |
                                        sup::rungBit(Rung::kGoldenImage) | sup::rungBit(Rung::kStop)));
  CHECK(unit.status().counts.resets == 1);
  CHECK(unit.status().counts.power_cycles == 1);
  CHECK(unit.status().counts.flashes == 1);
  const unsigned resets_after = unit.world.reset_pulses_seen;
  const unsigned kills_after = unit.world.kill_cycles_seen;
  const unsigned ops_after = flashOps(unit.world.u.chip);

  // Stopped: more steps do nothing at all, however long they go on for.
  for (int i = 0; i < 50; ++i) {
    const sup::Step s = unit.walter->supervisor.step();
    CHECK(s.waited);
    CHECK(s.after == sup::State::kStopped);
    unit.world.u.clock.delayMs(60000);
  }
  CHECK(unit.world.reset_pulses_seen == resets_after);
  CHECK(unit.world.kill_cycles_seen == kills_after);
  CHECK(flashOps(unit.world.u.chip) == ops_after);
  CHECK(unit.status().stopped);
  CHECK(unit.status().quiet_left_ms > 0);

  // And a restart of the Walter does not lift it: the stop is in retained storage.
  unit.reboot();
  CHECK(unit.status().stopped);
  CHECK(unit.status().stop_reason == sup::StopReason::kEverythingTried);

  // The quiet period over, the ladder may be tried once more - and is.
  unit.world.u.clock.advance(sup::kQuietPeriodMs);
  const sup::Step over = unit.walter->supervisor.step();
  CHECK(over.changed);
  CHECK(over.after == sup::State::kIdle);
  CHECK_FALSE(unit.status().stopped);
  unit.walter->steps.clear();
  unit.walter->rung_ms.clear();
  CHECK(unit.walter->climb() == sup::State::kStopped);
  CHECK(unit.world.reset_pulses_seen > resets_after);  // a second climb, three days later
  CHECK(unit.walter->supervisor.incidentCount() == 2);
}

TEST_CASE("rung 4 looks at the chip before it writes to it") {
  SUBCASE("the chip holds the golden image: nothing is flashed") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();  // the firmware is there and the radio still will not answer
    const unsigned before = flashOps(unit.world.u.chip);

    CHECK(unit.walter->ladder() == sup::State::kStopped);
    CHECK(unit.status().stop_reason == sup::StopReason::kFirmwareIntact);
    CHECK(unit.incident().rung[3] == RungOutcome::kIntact);
    CHECK(flashOps(unit.world.u.chip) == before);  // read, not written
    CHECK(unit.status().counts.flashes == 0);      // and it did not spend the day's flash
  }

  SUBCASE("the chip is protected: it stops, and nothing erases it") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();
    unit.world.u.chip.approtect = true;  // a module whose debug port is shut
    const unsigned before = flashOps(unit.world.u.chip);

    CHECK(unit.walter->ladder() == sup::State::kStopped);
    CHECK(unit.status().stop_reason == sup::StopReason::kChipProtected);
    CHECK(unit.incident().rung[3] == RungOutcome::kFailed);
    CHECK(flashOps(unit.world.u.chip) == before);
    CHECK(unit.status().counts.flashes == 0);
  }

  SUBCASE("the chip cannot be read at all: it stops with that reason") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();
    unit.walter->target.silent = true;  // nothing answers on the debug port
    const unsigned before = flashOps(unit.world.u.chip);

    CHECK(unit.walter->ladder() == sup::State::kStopped);
    CHECK(unit.status().stop_reason == sup::StopReason::kChipUnreadable);
    CHECK(flashOps(unit.world.u.chip) == before);
    CHECK(unit.status().counts.flashes == 0);
  }

  SUBCASE("there is no golden image to put back") {
    WatchedUnit unit(true, false);  // nothing in the store, nothing on the chip
    unit.walter->raw.millivolts = 4000;
    unit.world.goSilent();

    CHECK(unit.walter->ladder() == sup::State::kStopped);
    CHECK(unit.status().stop_reason == sup::StopReason::kNoGoldenImage);
    CHECK(unit.status().counts.flashes == 0);
  }
}

TEST_CASE("a boot loop takes the ladder from rung 3") {
  WatchedUnit unit;
  unit.walter->beat();
  // The radio restarts by itself, three times inside the window (§5.1's EV_BOOT each time).
  for (std::uint32_t i = 0; i < sup::kBootLoopBoots; ++i) {
    unit.world.u.radio.restart(true);
    unit.world.u.clock.delayMs(1000);
    unit.walter->supervisor.step();  // the client reads the EV_BOOT inside the heartbeat's poll
    unit.world.u.clock.advance(sup::kHeartbeatIntervalMs);
  }
  CHECK(unit.status().restarts >= sup::kBootLoopBoots);
  CHECK(unit.status().boot_loop);

  // A radio that answers and keeps restarting is an incident all the same, begun by the
  // heartbeat that noticed the third boot.
  REQUIRE(unit.walter->supervisor.incidentCount() == 1);
  CHECK(unit.incident().cause == sup::Cause::kBootLoop);
  CHECK(unit.incident().rung[0] == RungOutcome::kSkipped);  // asking again cures nothing
  CHECK(unit.incident().rung[1] == RungOutcome::kSkipped);  // nor does a reset: it is resetting
  unit.walter->climb();
  CHECK(unit.walter->pins.reset_pulses == 0);
  CHECK(unit.walter->pins.kill_cycles == 1);
  CHECK(unit.status().counts.resets == 0);
  CHECK(unit.status().counts.power_cycles == 1);
  // The reset reasons it reported are kept for the report.
  CHECK(unit.walter->supervisor.incidentAt(0).reset_reason != 0);
  CHECK(unit.walter->supervisor.incidentAt(0).restarts >= 1);
}

TEST_CASE("a radio whose uptime never gets past the threshold is a boot loop too") {
  WatchedUnit unit;
  // The other way a boot loop shows (software-goals must-have 6): a radio that answers, and
  // whose uptime is always a minute or two, is starting over and over without saying so.
  unit.world.u.radio.override_uptime = true;
  unit.world.u.radio.uptime_s = 30;
  unit.walter->beat();
  CHECK(unit.status().uptime_s == 30);
  CHECK_FALSE(unit.status().boot_loop);  // one short uptime is a radio that has just booted

  // A second reading, a boot-loop window later, still short.
  unit.world.u.clock.advance(static_cast<std::uint64_t>(sup::kBootLoopUptimeS) * 1000u + 1000u);
  unit.walter->beat();
  CHECK(unit.status().boot_loop);
  // And it is treated as one: an incident whose ladder starts at rung 3.
  REQUIRE(unit.walter->supervisor.incidentCount() == 1);
  CHECK(unit.incident().cause == sup::Cause::kBootLoop);
  CHECK(unit.incident().rung[1] == RungOutcome::kSkipped);
  CHECK(unit.incident().last_uptime_s == 30);

  // An uptime that grows past the threshold is not a loop.
  WatchedUnit well;
  well.world.u.radio.override_uptime = true;
  well.world.u.radio.uptime_s = 30;
  well.walter->beat();
  well.world.u.radio.uptime_s = sup::kBootLoopUptimeS * 10;
  well.world.u.clock.advance(static_cast<std::uint64_t>(sup::kBootLoopUptimeS) * 1000u + 1000u);
  well.walter->beat();
  CHECK_FALSE(well.status().boot_loop);
}

TEST_CASE("the three silences that are not faults: nothing is done") {
  SUBCASE("the scanner is running, and this session has never heard a frame") {
    // §9: a Walter that gets no HELLO and sees lines beginning with `{` is talking to NoiseScope.
    // It is decided per session, and a session begins at a boot of the Walter or a reset of the
    // radio - frames and text are never mixed in one (task p1-02's client holds that).
    WatchedUnit unit;
    unit.world.runScanner();

    unit.walter->supervisor.step();  // the first ask: three HELLOs and §9's `info` probe
    REQUIRE(unit.walter->client.mode() == core::link::client::Mode::kText);
    const sup::Step told = unit.walter->supervisor.step();
    CHECK(told.silence == Silence::kScanner);
    CHECK(told.after == sup::State::kIdle);
    CHECK(unit.status().health == sup::Health::kScanner);
    CHECK(unit.walter->supervisor.incidentCount() == 0);
    CHECK(unit.walter->pins.reset_pulses == 0);
    CHECK(unit.walter->pins.kill_cycles == 0);
    CHECK(unit.walter->supervisorEntries() == 0);
  }

  SUBCASE("the scanner appeared after a session of frames: the reset of rung 2 reveals it") {
    // The radio was answering frames and is now running the scanner. §9 is explicit that the
    // decision holds "until the next reset of the RAK", so this unit cannot know yet - and the
    // ladder's second rung is a reset, which is what ends the old session and finds out.
    WatchedUnit unit;
    unit.walter->beat();  // a session of frames
    unit.world.runScanner();

    CHECK(unit.walter->ladder() == sup::State::kIdle);
    CHECK(unit.walter->pins.reset_pulses == 1);  // the reset is what ended the old session
    CHECK(unit.walter->pins.kill_cycles == 0);   // rung 3 was never reached
    CHECK(unit.status().health == sup::Health::kScanner);
    CHECK(unit.incident().outcome == Outcome::kCured);  // something is running over there
    CHECK(flashOps(unit.world.u.chip) == 0);
  }

  SUBCASE("this Walter has only just powered up") {
    // ARCHITECTURE §2 rule 5: a power-up of the ESP32-S3 glitches GPIO1-14 low for about 60 us,
    // and that reaches nRESET - our own power-up reset the radio, and it is still booting.
    WatchedUnit unit(false);
    unit.reboot(core::health::ResetReason::kPowerOn);
    unit.world.goSilent();
    REQUIRE(unit.world.u.clock.nowMs() < sup::kWalterPowerUpGraceMs);

    unit.walter->supervisor.step();  // asks, finds silence
    const sup::Step told = unit.walter->supervisor.step();
    CHECK(told.silence == Silence::kWalterPoweredUp);
    CHECK(told.after == sup::State::kIdle);
    CHECK(unit.walter->supervisor.incidentCount() == 0);
    CHECK(unit.walter->pins.reset_pulses == 0);
    CHECK(unit.walter->pins.kill_cycles == 0);
  }

  SUBCASE("an update is in progress") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();
    // The journal says a radio update was in progress and has not ended (task p2-03's states).
    upd::Payload p;
    p.state = upd::State::kBooting;
    p.kind = upd::Kind::kUpdate;
    std::uint8_t bytes[upd::kPayloadBytes];
    upd::encode(p, bytes);
    REQUIRE(unit.walter->journal.append(upd::kind::kRadioUpdate, bytes, sizeof bytes) ==
            upd::JournalError::kNone);

    unit.walter->beat();
    const sup::Step told = unit.walter->supervisor.step();
    CHECK(told.silence == Silence::kUpdateRunning);
    CHECK(told.after == sup::State::kIdle);
    CHECK(unit.walter->supervisor.incidentCount() == 0);
    CHECK(unit.walter->pins.reset_pulses == 0);
  }

  SUBCASE("the Walter's own update is unconfirmed (S-7)") {
    WatchedUnit unit;
    unit.walter->beat();
    unit.world.goSilent();
    std::uint8_t self[8] = {upd::self_update::kOnTrial, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(unit.walter->journal.append(upd::kind::kSelfUpdate, self, sizeof self) ==
            upd::JournalError::kNone);

    unit.walter->beat();
    const sup::Step told = unit.walter->supervisor.step();
    CHECK(told.silence == Silence::kUpdateRunning);
    CHECK(unit.walter->pins.reset_pulses == 0);
  }
}

TEST_CASE("a low battery refuses the power cycle and the flash, and the incident says why") {
  WatchedUnit unit;
  unit.walter->raw.millivolts = sup::kPowerCycleMinMv - 100;
  unit.walter->beat();  // the reading is taken while the link is up, and remembered
  REQUIRE(unit.status().battery_known);
  unit.world.goSilent();
  unit.world.wipeApplication();  // a reflash would cure it - and is refused all the same

  CHECK(unit.walter->ladder() == sup::State::kStopped);
  CHECK(unit.status().stop_reason == sup::StopReason::kBatteryTooLow);
  CHECK(unit.incident().rung[2] == RungOutcome::kRefusedBattery);
  CHECK(unit.incident().battery_mv == sup::kPowerCycleMinMv - 100);
  // A reset costs the battery nothing and was tried; the rail and the flash were not.
  CHECK(unit.walter->pins.reset_pulses == 1);
  CHECK(unit.walter->pins.kill_cycles == 0);
  CHECK(unit.status().counts.power_cycles == 0);
  CHECK(unit.status().counts.flashes == 0);
  // And the refusal is in the journal, with the reading.
  bool found = false;
  for (const sup::Payload& p : unit.walter->story()) {
    if (p.what == sup::Wrote::kRung && p.rung == Rung::kPowerCycle &&
        p.outcome == static_cast<std::uint8_t>(RungOutcome::kRefusedBattery)) {
      found = true;
      CHECK(p.detail == sup::kPowerCycleMinMv - 100);
    }
  }
  CHECK(found);
}

TEST_CASE("the battery between the two thresholds allows the power cycle and refuses the flash") {
  WatchedUnit unit;
  unit.walter->raw.millivolts = (sup::kPowerCycleMinMv + sup::kFlashMinMv) / 2;
  unit.walter->beat();
  unit.world.goSilent();
  unit.world.wipeApplication();

  CHECK(unit.walter->ladder() == sup::State::kStopped);
  CHECK(unit.status().stop_reason == sup::StopReason::kBatteryTooLow);
  CHECK(unit.walter->pins.kill_cycles == 1);  // the rail was allowed
  CHECK(unit.status().counts.power_cycles == 1);
  CHECK(unit.incident().rung[3] == RungOutcome::kRefusedBattery);
  CHECK(unit.status().counts.flashes == 0);
}

TEST_CASE("the battery behind a dead radio is the last good reading") {
  WatchedUnit unit;
  unit.walter->raw.millivolts = 4000;
  unit.walter->beat();
  REQUIRE(unit.walter->battery.liveReads() > 0);
  // The radio is gone, so POWER_GET answers nothing: what the rungs judge by is what it said
  // while it could (core/update/last_good_battery.h).
  unit.walter->raw.known = false;  // POWER_GET answers nothing from here on
  unit.world.wipeApplication();

  CHECK(unit.walter->ladder() == sup::State::kIdle);  // rung 4 cured it
  CHECK(unit.walter->battery.staleReads() > 0);
  CHECK(unit.status().counts.flashes == 1);

  // A reading that is older than kBatteryStaleAfterMs is no reading at all.
  unit.world.u.clock.advance(upd::kBatteryStaleAfterMs + 1);
  upd::BatteryReading reading;
  CHECK_FALSE(unit.walter->battery.read(&reading));
}

TEST_CASE("after a restart of the radio the clock is set and STATUS read again") {
  WatchedUnit unit;
  unit.walter->beat();
  const std::size_t before = unit.world.u.radio.requests.size();
  unit.world.u.radio.restart(true);  // EV_BOOT: §5.1, every piece of per-boot state is gone
  unit.world.u.clock.delayMs(2000);
  unit.walter->beat();

  bool time_set = false;
  bool status_read = false;
  for (std::size_t i = before; i < unit.world.u.radio.requests.size(); ++i) {
    const std::uint8_t type = unit.world.u.radio.requests[i].type;
    if (type == core::link::type::kTimeSet) time_set = true;
    if (type == core::link::type::kStatus) status_read = true;
  }
  CHECK(time_set);     // §5.4: before anything else it asks the radio to do
  CHECK(status_read);  // everything the old boot said belonged to the boot before
  CHECK(unit.status().time_set);
  CHECK(unit.status().health == sup::Health::kHealthy);
}

TEST_CASE("an attention is recorded, acknowledged and handed on (section 8)") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.u.radio.raiseAttention(core::link::attention::kBatteryLow, 3350, 3);
  // §8: 0x00 0x00, then the event 300 ms later. Nothing of ours asks in between, or the radio
  // would stop repeating.
  unit.world.u.clock.advance(500);
  unit.walter->beat();

  // Three repeats of one attention are one attention (§8: it repeats until a request arrives).
  REQUIRE(unit.walter->supervisor.attentionCount() == 1);
  const sup::Attention& kept = unit.walter->supervisor.attentionAt(0);
  CHECK(kept.reason == core::link::attention::kBatteryLow);
  CHECK(kept.detail == 3350);
  CHECK(kept.repeat >= 1);
  CHECK(kept.acknowledged);  // a request of ours went out after it was read
  CHECK(unit.status().attentions == 1);

  // It is journalled with its reason, and handed to whoever runs the wake exactly once.
  bool journalled = false;
  for (const sup::Payload& p : unit.walter->story()) {
    if (p.what == sup::Wrote::kAttention &&
        p.outcome == core::link::attention::kBatteryLow && (p.detail & 0xFFFFu) == 3350) {
      journalled = true;
    }
  }
  CHECK(journalled);
  sup::Attention taken;
  CHECK(unit.walter->supervisor.takeAttention(&taken));
  CHECK(taken.reason == core::link::attention::kBatteryLow);
  CHECK_FALSE(unit.walter->supervisor.takeAttention(&taken));
  CHECK(unit.walter->supervisor.attentionCount() == 0);
}

TEST_CASE("two resets are never less than a second apart, however the ladder is driven") {
  WatchedUnit unit;
  unit.walter->beat();
  unit.world.goSilent();
  unit.world.cure = Rung::kNone;

  // Climb, lift the stop by hand at once, climb again, five times over: every reset pulse goes
  // through RadioPins, which waits out the spacing rather than violating it (ARCHITECTURE §2
  // rule 2, and the fake pins' `reset-spacing` rule fails the test if it ever does not).
  for (int i = 0; i < 5; ++i) {
    unit.walter->ladder();
    CHECK(unit.walter->pins.countViolations("reset-spacing") == 0);
    CHECK(unit.walter->pins.countViolations("reset-pulse-length") == 0);
    CHECK(unit.walter->pins.countViolations("driven-while-killed") == 0);
    CHECK(unit.walter->pins.countViolations("reset-driven-high") == 0);
    CHECK(unit.walter->pins.countViolations("kill-driven-low") == 0);
  }
  CHECK(unit.walter->pins.violations.empty());
  // And across a restart of the Walter, where the record of the last pulse is in retained
  // storage and the clock is all there is to hold it against.
  unit.reboot();
  unit.walter->ladder();
  CHECK(unit.walter->pins.violations.empty());
}
