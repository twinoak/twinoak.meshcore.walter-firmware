// The uplink's bench (task p3-01): a fake modem, a clock whose delays take no time, retained
// storage that outlives the Walter built over it, and the policy on top - plus one wake of the
// duty cycle as a method, because almost everything this task has to prove is about what happens
// over many wakes rather than inside one.
//
// A wake here is: the interval passes, the modem forgets everything a deep sleep takes with it,
// the wake is counted for the backoff, and the backoff decides whether anything is tried at all.
// That last part is the point - a wake in which nothing is tried still has to be a wake that was
// counted, or "every fourth wake" means nothing.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <memory>

#include "core/uplink/policy.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_retained.h"
#include "sim/fake_modem.h"

namespace bench {

namespace upl = core::uplink;

struct UplinkUnit {
  fakes::FakeClock clock;
  fakes::FakeRetained retained;
  sim::FakeModem modem{clock};
  upl::Settings settings{};
  std::unique_ptr<upl::UplinkPolicy> policy;

  // Half an hour between wakes: a plausible middle of the duty cycle's table, which does not
  // exist yet (task p3-04). Nothing here depends on the number except how long "a month" is.
  std::uint32_t wake_interval_ms = 30u * 60u * 1000u;
  int wakes = 0;
  int attempts = 0;

  UplinkUnit() { boot(); }

  // A restart of the Walter: a new policy over the same retained storage, as a reset or a wake
  // from deep sleep gives it.
  void boot() {
    policy.reset(new upl::UplinkPolicy(modem, clock, retained, settings));
    policy->begin();
  }

  // A power cut: RTC memory goes with it (interfaces/retained.h).
  void powerCut() {
    retained.powerCut();
    boot();
  }

  const upl::Status& status() const { return policy->status(); }

  // One wake. True when an attempt was made; `out`, when given, is its result.
  bool oneWake(upl::Result* out = nullptr, std::uint32_t budget_ms = upl::kWakeBudgetMs) {
    clock.advance(wake_interval_ms);
    modem.forgetEverything();
    ++wakes;
    policy->beginWake();
    if (!policy->mayTry()) return false;
    ++attempts;
    const upl::Result r = policy->bringUp(budget_ms);
    if (out != nullptr) *out = r;
    return true;
  }

  // One wake in which the modem is brought up whatever the backoff says: what `lte up` from a
  // person does, and what a test that only cares about one bring-up wants.
  upl::Result forceWake(std::uint32_t budget_ms = upl::kWakeBudgetMs) {
    clock.advance(wake_interval_ms);
    modem.forgetEverything();
    ++wakes;
    ++attempts;
    policy->beginWake();
    return policy->bringUp(budget_ms);
  }

  // Wake until an attempt is actually made, at most `limit` wakes. The number of wakes it took,
  // or 0 when the backoff never allowed one.
  int wakeUntilTried(upl::Result* out, int limit = 64) {
    for (int i = 1; i <= limit; ++i) {
      if (oneWake(out)) return i;
    }
    return 0;
  }
};

}  // namespace bench
