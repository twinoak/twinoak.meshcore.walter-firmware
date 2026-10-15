// A simulated month of random radio behaviour over the supervisor's bench, as a function of a
// seed and a number of days (task p2-05's property; task p3-06's rail R11 runs it over a longer
// horizon). For random sequences of radio behaviour the daily counts never exceed the caps - and
// what the radio actually got, counted at the pins, never exceeds them either. A count that is
// right while the wires say otherwise would be the bug this exists to catch.
//
// The Walter is restarted, slept and power-cut at random inside the month, because every one of
// those is a way the day's record can be lost, and the caps have to hold across all of them.
// (A Walter that was built again has fresh pin counters, so the per-window comparison against
// the wires begins again there; the counts themselves are checked after every single step.)
//
// It lives in a header because two tests walk it: host_test/core/supervisor/
// test_supervisor_property.cpp (a month and a fortnight, two seeds) and
// host_test/rails/test_rail_caps.cpp (a quarter, and the caps as the rail).
#pragma once

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/supervisor/supervisor.h"
#include "supervisor_bench.h"

namespace bench {

namespace sup = core::supervisor;

// A small deterministic generator, so that a failure is a seed and not a mystery.
struct Random {
  std::uint32_t state;
  explicit Random(std::uint32_t seed) : state(seed) {}
  std::uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  std::uint32_t upTo(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

struct Month {
  std::uint32_t climbs = 0;
  std::uint32_t windows = 0;
  std::uint32_t incidents = 0;
  std::uint32_t flashes = 0;
};

inline Month runMonth(std::uint32_t seed, std::uint64_t days) {
  Random random(seed);
  Month seen;
  WatchedUnit unit;
  unit.walter->pins.record_log = false;  // a month of transitions is millions; the rules still hold
  unit.walter->beat();

  std::uint64_t window = unit.status().window_began_ms;
  int resets_at_window = unit.walter->pins.reset_pulses;
  int kills_at_window = unit.walter->pins.kill_cycles;
  // A wake from deep sleep puts the monotonic clock back to zero (interfaces/clock.h), so the
  // simulated month is counted here and not read off the clock - what was on it before each
  // sleep is added to `slept`.
  std::uint64_t slept = 0;
  auto simulated = [&]() { return slept + unit.world.u.clock.nowMs(); };
  const std::uint64_t until = simulated() + days * 24 * 60 * 60 * 1000;
  // A climb inside a loop like this one steps with a coarse idle: the waits are measured by the
  // clock, so a tenth of a second and five seconds prove the same thing and one of them is fifty
  // times the work.
  const std::uint32_t idle_ms = 5000;

  // The caps hold at every step; inside a window the wires agree with the counts.
  auto check = [&]() {
    const sup::Status& w = unit.status();
    REQUIRE(w.counts.resets <= w.caps.resets);
    REQUIRE(w.counts.power_cycles <= w.caps.power_cycles);
    REQUIRE(w.counts.flashes <= w.caps.flashes);
    if (w.window_began_ms != window) {
      ++seen.windows;
      window = w.window_began_ms;
      resets_at_window = unit.walter->pins.reset_pulses;
      kills_at_window = unit.walter->pins.kill_cycles;
      return;
    }
    if (unit.walter->pins.reset_pulses < resets_at_window ||
        unit.walter->pins.kill_cycles < kills_at_window) {
      resets_at_window = unit.walter->pins.reset_pulses;  // a Walter built again: fresh counters
      kills_at_window = unit.walter->pins.kill_cycles;
      return;
    }
    REQUIRE(unit.walter->pins.reset_pulses - resets_at_window <=
            static_cast<int>(w.caps.resets));
    REQUIRE(unit.walter->pins.kill_cycles - kills_at_window <=
            static_cast<int>(w.caps.power_cycles));
  };

  // The radio answering again, as a reset of it puts the client back to "I do not know what is
  // over there" (§9: a text session ends at a reset of the RAK and nowhere else).
  auto healthy = [&unit]() {
    unit.world.u.radio.scanner = false;
    unit.world.u.radio.deaf = false;
    unit.world.u.boot = Boot::kNormal;
    unit.world.cured = true;
    unit.walter->client.radioWasReset();
  };

  while (simulated() < until) {
    switch (random.upTo(10)) {
      case 0:
      case 1:
      case 2:
        healthy();
        unit.walter->beat();
        break;
      case 3:
      case 4:
        unit.world.cured = false;
        unit.world.cure = Rung::kReset;  // a reset cures it
        unit.world.goSilent();
        ++seen.climbs;
        unit.walter->ladder(idle_ms);
        break;
      case 5:
        unit.world.cured = false;
        unit.world.cure = Rung::kPowerCycle;  // only the rail cures it
        unit.world.goSilent();
        ++seen.climbs;
        unit.walter->ladder(idle_ms);
        break;
      case 6:
        unit.world.cured = false;
        unit.world.cure = Rung::kNone;  // nothing cures it: the ladder stops at the top
        unit.world.goSilent();
        ++seen.climbs;
        unit.walter->ladder(idle_ms);
        break;
      case 7:
        // The scanner is running: not a fault, and nothing is done.
        unit.world.u.radio.scanner = true;
        unit.world.u.radio.sendLine("{\"t\":\"stat\"}");
        unit.walter->beat();
        unit.walter->supervisor.step();
        healthy();
        break;
      case 8:
        // It restarts by itself: a boot loop, which climbs from rung 3.
        healthy();
        for (std::uint32_t i = 0; i < sup::kBootLoopBoots; ++i) {
          unit.world.u.radio.restart(true);
          unit.world.u.clock.delayMs(1000);
          unit.walter->supervisor.step();
          check();
        }
        ++seen.climbs;
        unit.walter->beat();
        unit.walter->climb(200000, idle_ms);
        break;
      default: {
        // Its chip loses its application: only rung 4 can mend that, and only once a day.
        unit.world.cured = true;
        unit.world.cure = Rung::kNone;
        unit.world.u.boot = Boot::kNormal;
        unit.world.wipeApplication();
        const std::uint32_t before = unit.status().counts.flashes;
        ++seen.climbs;
        unit.walter->ladder(idle_ms);
        if (unit.status().counts.flashes > before) ++seen.flashes;
        break;
      }
    }
    check();
    seen.incidents = static_cast<std::uint32_t>(unit.walter->supervisor.incidentCount());

    // Time between events: a few minutes to four hours.
    unit.world.u.clock.advance(60000ull * (1 + random.upTo(240)));

    // And now and then the Walter itself goes away.
    switch (random.upTo(24)) {
      case 0:
        unit.reboot();
        break;
      case 1:
        slept += unit.world.u.clock.nowMs();
        unit.world.deepSleepWake();
        unit.reboot(core::health::ResetReason::kDeepSleepWake);
        unit.world.u.clock.advance(sup::kWalterPowerUpGraceMs + 1);
        window = unit.status().window_began_ms;
        break;
      case 2:
        unit.powerCut();
        break;
      default:
        break;
    }
    unit.walter->pins.record_log = false;
    unit.walter->steps.clear();
    unit.walter->rung_ms.clear();
    unit.walter->step_ms.clear();
    check();
  }

  CHECK(unit.walter->pins.violations.empty());
  CHECK(unit.status().counts.resets <= sup::kResetsPerDay);
  CHECK(unit.status().counts.power_cycles <= sup::kPowerCyclesPerDay);
  CHECK(unit.status().counts.flashes <= sup::kFlashesPerDay);
  return seen;
}


}  // namespace bench
