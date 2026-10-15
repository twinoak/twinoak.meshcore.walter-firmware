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
      guard_(clock, restart, health),
      sleep_(sleeper, pins, radio, uplink, reason_, clock, wall, update, self, journal),
      attentions_(supervisor) {}

void DutyCycle::begin() {
  // The USB host is read before anything else: it is what turns every wake into "a person is
  // here", and the bound on a wake is held off by it.
  const bool host = sleeper_.usbHost();
  reason_.begin(health_.thisBoot(), sleeper_.wokeBy(), host,
                attentions_.there() ? &attentions_ : nullptr);
  guard_.usbHostIs(host);
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
