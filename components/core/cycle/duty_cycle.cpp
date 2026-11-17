#include "core/cycle/duty_cycle.h"

namespace core {
namespace cycle {

DutyCycle::DutyCycle(interfaces::Sleeper& sleeper, board::RadioPins& pins,
                     link::client::Client& radio, uplink::UplinkPolicy& uplink,
                     health::Health& health, interfaces::Clock& clock, interfaces::WallClock& wall,
                     interfaces::Restart& restart, interfaces::Retained& retained,
                     supervisor::Supervisor* supervisor, update::RadioUpdate* update,
                     update::SelfUpdate* self, update::Journal* journal,
                     settings::Settings* settings)
    : sleeper_(sleeper),
      radio_(radio),
      health_(health),
      settings_(settings),
      reason_(retained, clock),
      progress_(retained, clock),
      guard_(clock, restart, health),
      sleep_(sleeper, pins, radio, uplink, reason_, progress_, clock, wall, update, self, journal),
      attentions_(supervisor) {}

void DutyCycle::begin() {
  // The USB host is read before anything else: it is what turns every wake into "a person is
  // here", and the bound on a wake is held off by it.
  const bool host = sleeper_.usbHost();
  // Where the wake before this one was, out of the record, and this wake's own begun - **before**
  // the wake reason, because reading why we woke can block for the whole of §8's repeat window
  // and a wake that is reset inside that window should still leave word (task p15-06).
  progress_.begin();
  reason_.begin(health_.thisBoot(), sleeper_.wokeBy(), host,
                attentions_.there() ? &attentions_ : nullptr);
  guard_.usbHostIs(host);
  measureTimeAway();
}

// **How long this board was away, for a wake that is not the timer's** (task p14-06). The radio
// never sleeps and counts its own seconds, so the `uptime_s` written down before the sleep and
// the one read now are the two ends of it (`core/cycle/wake_reason.h`).
//
// Here, in `begin()`, because the figure has to be known **before the agent's state is restored**
// (`main/main.cpp`: `duty.begin()` is a hundred and fifty lines above `backend.begin()`, which is
// where `mm::AgentLoop::restore(state, awayMs)` is handed it). Step 4 of a wake reads a STATUS
// with the same `uptime_s` in it, and that is far too late: the wait would already have been put
// back whole and the wake would already be sending nothing.
//
// Two frames, and only when both are worth sending:
//
//   a timer wake      keeps the figure it has - the seconds the last sleep asked its timer for -
//                     so the radio is not asked at all, and the overwhelming majority of this
//                     unit's wakes cost nothing.
//   no reading        a power cut, a first boot, a wake that never reached the sleep, a radio
//                     that did not answer last time: there is nothing to measure against, so
//                     there is nothing to ask.
//
// A HELLO of this boot's own is not a cost either way: step 2 of the wake sends one a moment
// later, and the second one finds the session already up. It is needed here because `boot_id`
// comes from HELLO and nothing else (§5.1), and `boot_id` is how a radio that restarted during
// the sleep - whose seconds are not the same seconds - is told from one that did not.
void DutyCycle::measureTimeAway() {
  if (reason_.wake() == Wake::kTimer) return;
  if (!reason_.hasRadioReading()) return;
  const link::client::Answer<link::HelloResponse> hello = radio_.hello();
  if (!hello.ok()) return;
  // The nonce is this boot's own wake count: it moves every wake, and a PING answered with
  // another one is a stale frame whose `uptime_s` belongs to some other moment (§5.2).
  const std::uint32_t nonce = reason_.info().wakes ^ hello.message.body.boot_id;
  const link::client::Answer<link::PingResponse> ping = radio_.ping(nonce);
  if (!ping.ok() || ping.message.nonce != nonce) return;
  reason_.noteRadioAtBoot(hello.message.body.boot_id, ping.message.uptime_s);
}

bool DutyCycle::readPower() {
  // A reading needs a session: POWER is a capability HELLO advertises, and the link client
  // refuses a call whose capability nothing has advertised without putting a byte on the wire
  // (§4, §5.1). So HELLO first when there is none - which is what the first step of a wake does
  // anyway (the supervisor's heartbeat), and what makes `power` on the console work on its own.
  if (!radio_.session().up) (void)radio_.hello();
  // §5.8: `valid` bit 1 is channel 2, which on the standard B-board is the battery - "positive =
  // charging, negative = discharging", and the radio has already applied the shunt.
  const link::client::Answer<link::PowerGetResponse> a = radio_.powerGet();
  if (!a.ok() || (a.message.valid & 0x02u) == 0) {
    battery_.noReading();
    return false;
  }
  const link::PowerChannel& ch = a.message.channels[1];
  battery_.addSample(ch.bus_mv, ch.current_ma, true);
  return true;
}

std::uint32_t DutyCycle::intervalSetting() const {
  return settings_ != nullptr ? settings_->wakeIntervalMinutes() : 0;
}

bool DutyCycle::lineWillArm() const {
  const bool setting = settings_ == nullptr || settings_->wakeOnRadio();
  return reason_.lineMayArm(setting);
}

power::Decision DutyCycle::decide() {
  power::Inputs in{};
  in.was = reason_.band();
  in.wakes_in_saving = reason_.wakesInSaving();
  in.faults_in_a_row = wake().faults_in_a_row;
  in.radio_attention = wake().attention.reason;
  in.radio_detail = wake().attention.detail;
  in.wake_interval_minutes = intervalSetting();
  decision_ = battery_.decide(in);
  decided_ = true;
  reason_.bandDecided(decision_.band);
  return decision_;
}

SleepResult DutyCycle::sleepNow(bool by_hand) {
  if (!decided_) (void)decide();
  return sleep_.go(decision_, lineWillArm(), by_hand);
}

SleepResult DutyCycle::sleepFor(std::uint32_t seconds, bool by_hand) {
  if (!decided_) (void)decide();
  return sleep_.goFor(seconds, decision_, lineWillArm(), by_hand);
}

bool DutyCycle::tick() {
  guard_.usbHostIs(sleeper_.usbHost());
  // The wake is still going, at this second of it (task p15-06): the one place that is asked as
  // the loop turns, so *for how long* has an answer for a wake nothing finer wrote down again.
  progress_.stillAwake();
  return guard_.tick();
}

void DutyCycle::settingsChanged() {
  // Nothing to recompute: `wake.interval` and `wake.radio` are read where they are used, at the
  // end of a wake (core/settings/keys.h's own `effect` column says so). A decision already taken
  // is stale, though, so the next sleep takes a new one.
  decided_ = false;
}

}  // namespace cycle
}  // namespace core
