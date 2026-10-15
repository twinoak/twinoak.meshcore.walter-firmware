// The duty cycle, in one object (task p3-04): why this wake happened
// (`core/cycle/wake_reason.h`), what the battery allows (`core/power/interval_policy.h`), the
// bound on a wake (`core/cycle/wake_guard.h`) and the one way out of it
// (`core/cycle/sleep.h`). It composes those four and nothing more - it does **not** run a wake:
// what a wake does, in what order, is task p3-05's, and this is the thing that task will drive.
//
// Why it exists at all: the four parts have nine collaborators between them, and a console
// constructor with nine more arguments would be a worse seam than one. `main/` constructs this,
// the console is handed this, and p3-05 will step this.
//
// **In this firmware today nothing sleeps by itself.** `begin()` works out the wake, `readPower()`
// and `decide()` answer, and the console's `sleep now`, `sleep <seconds>`, `wake`, `power` and
// `stay` drive it; the bound on a wake is constructed and left *disarmed*, because a bound that
// reset a Walter which has no way to reach sleep would be a boot loop and not a safety rail
// (tasks/phase-3/reports/04.md, *Decisions*). `arm()` is the one line task p3-05 adds.
#pragma once

#include <cstdint>

#include "core/board/radio_pins.h"
#include "core/cycle/attention_source.h"
#include "core/cycle/sleep.h"
#include "core/cycle/wake_guard.h"
#include "core/cycle/wake_reason.h"
#include "core/health/health.h"
#include "core/link/client/client.h"
#include "core/power/interval_policy.h"
#include "core/settings/settings.h"
#include "core/supervisor/supervisor.h"
#include "core/update/journal.h"
#include "core/update/radio_update.h"
#include "core/update/self_update.h"
#include "core/uplink/policy.h"
#include "interfaces/clock.h"
#include "interfaces/restart.h"
#include "interfaces/retained.h"
#include "interfaces/sleeper.h"
#include "interfaces/wall_clock.h"

namespace core {
namespace cycle {

class DutyCycle {
 public:
  // The supervisor, the two update machines, the journal and the settings are pointers because a
  // build can be without them (main/main.cpp hands over nothing rather than a machine over flash
  // that is not there). Without a supervisor a wake on the radio's line is spurious by
  // definition - nothing could have decoded the event - and `wake` says so.
  DutyCycle(interfaces::Sleeper& sleeper, board::RadioPins& pins, link::client::Client& radio,
            uplink::UplinkPolicy& uplink, health::Health& health, interfaces::Clock& clock,
            interfaces::WallClock& wall, interfaces::Restart& restart,
            interfaces::Retained& retained, supervisor::Supervisor* supervisor = nullptr,
            update::RadioUpdate* update = nullptr, update::SelfUpdate* self = nullptr,
            update::Journal* journal = nullptr, settings::Settings* settings = nullptr);

  // At boot, after `health.begin()` and after the pins are safe: what this wake means. For a
  // wake on the radio's line this listens for up to `kAttentionWindowMs` (§8's whole repeat
  // window), so it is the one call of this class that takes time.
  void begin();
  bool begun() const { return reason_.begun(); }

  const WakeInfo& wake() const { return reason_.info(); }
  WakeReason& reason() { return reason_; }
  const WakeReason& reason() const { return reason_; }
  power::IntervalPolicy& battery() { return battery_; }
  const power::IntervalPolicy& battery() const { return battery_; }
  WakeGuard& guard() { return guard_; }
  const WakeGuard& guard() const { return guard_; }
  Sleep& sleeper() { return sleep_; }

  // A POWER_GET over the link, channel 2, into the policy's ring (§5.8). True when a reading was
  // had; false records a miss, which is what makes the band `no-reading`.
  bool readPower();

  // The band and the interval now, from the readings this wake has and everything that crossed
  // the sleep. Remembers the band for the next wake's hysteresis.
  power::Decision decide();
  const power::Decision& decision() const { return decision_; }
  // The last decision without taking a new one: what `status` prints.
  bool decided() const { return decided_; }

  // Whether the radio's line will arm the next sleep: `wake.radio`, and the spurious-wake
  // disarm.
  bool lineWillArm() const;
  // `wake.interval` in minutes, 0 for `auto`.
  std::uint32_t intervalSetting() const;

  // The one way out of a wake: decide(), then sleep for the interval it chose. **On the board
  // this does not return.**
  SleepResult sleepNow(bool by_hand = false);
  // `sleep <seconds>`: a person's number, the band's allowance unchanged. `by_hand` false is the
  // wake cycle's shortened interval (task p3-05, step 10), which is nobody's hand.
  SleepResult sleepFor(std::uint32_t seconds, bool by_hand = true);

  // The modem was used in this wake, or was not and the band is `saving`: the every-Nth counter.
  // Whoever runs the wake calls one of them, so that a wake the uplink's backoff skipped does
  // not count as a use.
  void modemWasUsed() { reason_.modemWasUsed(); }
  void modemWasSkipped() { reason_.modemWasSkipped(); }

  // As the loop turns: the sleeper is asked whether a host is there and the bound is checked.
  // True when the bound ended the wake (the restart was asked for).
  bool tick();

  // `wake.interval` or `wake.radio` changed at the console.
  void settingsChanged();

 private:
  interfaces::Sleeper& sleeper_;
  link::client::Client& radio_;
  health::Health& health_;
  settings::Settings* settings_ = nullptr;
  WakeReason reason_;
  power::IntervalPolicy battery_;
  WakeGuard guard_;
  Sleep sleep_;
  // The supervisor as an attention source. A member and not a local, because `begin()` hands a
  // pointer to it to the wake reason and core/ has no heap.
  SupervisorAttentions attentions_;
  power::Decision decision_{};
  bool decided_ = false;
};

}  // namespace cycle
}  // namespace core
