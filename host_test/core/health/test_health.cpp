// Why the last boot happened (task p0-03): the mapping from ESP-IDF's codes, the boot counter,
// the five remembered boots with the previous run's uptime, a planned restart's reason, a power
// cut and a corrupt record.
#include <doctest.h>

#include <string>

#include "core/health/health.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"

using core::health::Health;
using core::health::ResetReason;
using core::health::Restart;
using core::health::WakeCause;
namespace codes = core::health::esp_codes;

TEST_CASE("every ESP-IDF reset reason maps to one of ours, and unknown codes to unknown") {
  struct Case {
    int code;
    ResetReason expected;
  };
  const Case cases[] = {
      {codes::kRstUnknown, ResetReason::kUnknown},    {codes::kRstPowerOn, ResetReason::kPowerOn},
      {codes::kRstExt, ResetReason::kResetPin},       {codes::kRstSw, ResetReason::kSoftware},
      {codes::kRstPanic, ResetReason::kPanic},        {codes::kRstIntWdt, ResetReason::kWatchdog},
      {codes::kRstTaskWdt, ResetReason::kWatchdog},   {codes::kRstWdt, ResetReason::kWatchdog},
      {codes::kRstDeepSleep, ResetReason::kDeepSleepWake},
      {codes::kRstBrownout, ResetReason::kBrownout},  {codes::kRstSdio, ResetReason::kOther},
      {codes::kRstUsb, ResetReason::kUsb},            {codes::kRstJtag, ResetReason::kOther},
      {codes::kRstEfuse, ResetReason::kOther},        {codes::kRstPwrGlitch, ResetReason::kBrownout},
      {codes::kRstCpuLockup, ResetReason::kPanic},    {99, ResetReason::kUnknown},
      {-1, ResetReason::kUnknown},
  };
  for (const Case& c : cases) {
    CHECK_MESSAGE(core::health::fromEspResetReason(c.code) == c.expected, "code " << c.code);
  }
  // The copied codes are 0..15 without a gap, as the enum is.
  CHECK(codes::kRstUnknown == 0);
  CHECK(codes::kRstCpuLockup == 15);
}

TEST_CASE("every wake cause that matters maps: timer, a pin, not from sleep, other") {
  CHECK(core::health::fromEspWakeCause(codes::kWakeUndefined) == WakeCause::kNotFromSleep);
  CHECK(core::health::fromEspWakeCause(codes::kWakeTimer) == WakeCause::kTimer);
  CHECK(core::health::fromEspWakeCause(codes::kWakeExt0) == WakeCause::kPin);
  CHECK(core::health::fromEspWakeCause(codes::kWakeExt1) == WakeCause::kPin);
  CHECK(core::health::fromEspWakeCause(codes::kWakeGpio) == WakeCause::kPin);
  CHECK(core::health::fromEspWakeCause(codes::kWakeUart) == WakeCause::kPin);
  CHECK(core::health::fromEspWakeCause(codes::kWakeTouchpad) == WakeCause::kOther);
  CHECK(core::health::fromEspWakeCause(codes::kWakeUlp) == WakeCause::kOther);
  CHECK(core::health::fromEspWakeCause(99) == WakeCause::kOther);
  CHECK(core::health::fromEspWakeCause(-1) == WakeCause::kNotFromSleep);
}

TEST_CASE("every reason and cause has a name") {
  for (int i = 0; i <= static_cast<int>(ResetReason::kOther); ++i) {
    CHECK(std::string(core::health::name(static_cast<ResetReason>(i))) != "?");
  }
  for (int i = 0; i <= static_cast<int>(WakeCause::kOther); ++i) {
    CHECK(std::string(core::health::name(static_cast<WakeCause>(i))) != "?");
  }
  CHECK(std::string(core::health::name(Restart::kConsole)) == "asked for on the console");
  CHECK(std::string(core::health::name(static_cast<ResetReason>(200))) == "?");
}

TEST_CASE("the first boot: the record starts here") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  Health h(retained, clock);
  h.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
  CHECK(!h.recordSurvived());
  CHECK(h.boots() == 1);
  CHECK(h.count() == 1);
  CHECK(h.thisBoot().reason == ResetReason::kPowerOn);
  CHECK(h.thisBoot().wake == WakeCause::kNotFromSleep);
  CHECK(h.thisBoot().requested == Restart::kNotRequested);
  CHECK(h.thisBoot().previous_uptime_s == 0);
  CHECK(h.uptimeS() == 0);
  clock.advance(2500);
  CHECK(h.uptimeS() == 2);
  CHECK(retained.has(interfaces::RetainedSlot::kHealth));
  CHECK(h.at(1).reason == ResetReason::kUnknown);  // out of range: an empty record
}

TEST_CASE("a restart is counted and the run before it is remembered with its uptime") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  {
    Health first(retained, clock);
    first.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(5400);
    first.tick();
    const int stores = retained.stores;
    first.tick();  // the same second: nothing new to store
    CHECK(retained.stores == stores);
  }
  clock.advance(100);
  Health second(retained, clock);
  second.begin(ResetReason::kPanic, WakeCause::kNotFromSleep);
  CHECK(second.recordSurvived());
  CHECK(second.boots() == 2);
  CHECK(second.count() == 2);
  CHECK(second.thisBoot().reason == ResetReason::kPanic);
  CHECK(second.thisBoot().previous_uptime_s == 5);
  CHECK(second.at(1).reason == ResetReason::kPowerOn);
  CHECK(second.at(1).previous_uptime_s == 0);
  CHECK(second.uptimeS() == 0);
}

TEST_CASE("a planned restart says who asked; an unplanned software reset does not") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  {
    Health h(retained, clock);
    h.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(3000);
    h.noteRestart(Restart::kConsole);
  }
  {
    Health h(retained, clock);
    h.begin(ResetReason::kSoftware, WakeCause::kNotFromSleep);
    CHECK(h.thisBoot().reason == ResetReason::kSoftware);
    CHECK(h.thisBoot().requested == Restart::kConsole);
    CHECK(h.thisBoot().previous_uptime_s == 3);  // noteRestart kept the uptime
  }
  {
    // The request was consumed: the next software reset is nobody's.
    Health h(retained, clock);
    h.begin(ResetReason::kSoftware, WakeCause::kNotFromSleep);
    CHECK(h.thisBoot().requested == Restart::kNotRequested);
  }
  {
    // A request followed by a reset that is not a software one is not carried either.
    Health h(retained, clock);
    h.noteRestart(Restart::kConsole);
  }
  {
    Health h(retained, clock);
    h.begin(ResetReason::kWatchdog, WakeCause::kNotFromSleep);
    CHECK(h.thisBoot().requested == Restart::kNotRequested);
  }
}

TEST_CASE("five boots are kept, newest first, and the counter keeps counting") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  const ResetReason reasons[] = {ResetReason::kPowerOn, ResetReason::kSoftware, ResetReason::kPanic,
                                 ResetReason::kWatchdog, ResetReason::kBrownout, ResetReason::kResetPin,
                                 ResetReason::kUsb};
  for (int i = 0; i < 7; ++i) {
    Health h(retained, clock);
    h.begin(reasons[i], WakeCause::kNotFromSleep);
    clock.advance(1000u * static_cast<std::uint32_t>(i + 1));
    h.tick();
    CHECK(h.boots() == static_cast<std::uint32_t>(i + 1));
  }
  Health h(retained, clock);
  h.begin(ResetReason::kDeepSleepWake, WakeCause::kTimer);
  CHECK(h.boots() == 8);
  CHECK(h.count() == core::health::kRemembered);
  CHECK(h.at(0).reason == ResetReason::kDeepSleepWake);
  CHECK(h.at(0).wake == WakeCause::kTimer);
  CHECK(h.at(0).previous_uptime_s == 7);
  CHECK(h.at(1).reason == ResetReason::kUsb);
  CHECK(h.at(1).previous_uptime_s == 6);
  CHECK(h.at(4).reason == ResetReason::kWatchdog);
  CHECK(h.at(4).previous_uptime_s == 3);
}

TEST_CASE("a wake cause is only kept for a wake from deep sleep") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  Health h(retained, clock);
  h.begin(ResetReason::kPowerOn, WakeCause::kTimer);  // nonsense from the chip: a timer on a power-on
  CHECK(h.thisBoot().wake == WakeCause::kNotFromSleep);
}

TEST_CASE("a power cut and a corrupt record both start the record afresh") {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  {
    Health h(retained, clock);
    h.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
  }
  {
    Health h(retained, clock);
    h.begin(ResetReason::kSoftware, WakeCause::kNotFromSleep);
    CHECK(h.boots() == 2);
  }
  retained.powerCut();
  {
    Health h(retained, clock);
    h.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    CHECK(!h.recordSurvived());
    CHECK(h.boots() == 1);
    CHECK(h.count() == 1);
  }
  retained.truncate(interfaces::RetainedSlot::kHealth);
  {
    Health h(retained, clock);
    h.begin(ResetReason::kSoftware, WakeCause::kNotFromSleep);
    CHECK(!h.recordSurvived());
    CHECK(h.boots() == 1);
  }
}
