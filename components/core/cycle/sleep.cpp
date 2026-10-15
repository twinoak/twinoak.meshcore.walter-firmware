#include "core/cycle/sleep.h"

namespace core {
namespace cycle {

namespace {

void put32(std::uint8_t* at, std::uint32_t value) {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
  at[2] = static_cast<std::uint8_t>(value >> 16);
  at[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t get32(const std::uint8_t* at) {
  return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
         (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

std::uint8_t cap8(std::uint32_t value) {
  return value > 0xFF ? static_cast<std::uint8_t>(0xFF) : static_cast<std::uint8_t>(value);
}

}  // namespace

const char* describe(SleepRefusal refusal) {
  switch (refusal) {
    case SleepRefusal::kNone: return "asleep";
    case SleepRefusal::kNoSeconds:
      return "neither a time nor a wake source: only a reset would end that sleep";
    case SleepRefusal::kUpdateRunning:
      return "the radio's update has a step in flight: finish it or let it journal first";
    case SleepRefusal::kTransferOpen:
      return "the other application slot is open for writing";
    case SleepRefusal::kOnTrial:
      return "this firmware is on trial: a deep sleep would roll it back "
             "('self confirm' or 'self reject' first)";
    case SleepRefusal::kSleeperRefused:
      return "the chip would not take the sleep plan";
  }
  return "?";
}

const char* describe(Wrote what) {
  switch (what) {
    case Wrote::kNone: return "nothing";
    case Wrote::kSleeping: return "sleeping";
    case Wrote::kRefused: return "a sleep refused";
    case Wrote::kLineDisarmed: return "the radio's line disarmed as a wake source";
    case Wrote::kLineArmed: return "the radio's line armed again";
  }
  return "?";
}

void encode(const Payload& p, std::uint8_t out[kPayloadBytes]) {
  for (std::size_t i = 0; i < kPayloadBytes; ++i) out[i] = 0;
  out[0] = static_cast<std::uint8_t>(p.what);
  out[1] = static_cast<std::uint8_t>(p.wake);
  out[2] = static_cast<std::uint8_t>(p.band);
  out[3] = p.flags;
  put32(out + 4, p.interval_s);
  out[8] = static_cast<std::uint8_t>(p.median_mv);
  out[9] = static_cast<std::uint8_t>(p.median_mv >> 8);
  out[10] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(p.mean_ma));
  out[11] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(p.mean_ma) >> 8);
  put32(out + 12, p.at_epoch);
  put32(out + 16, p.at_ms);
  put32(out + 20, p.awake_s);
  out[24] = p.attention;
  out[25] = p.spurious;
  out[26] = p.faults;
  out[27] = static_cast<std::uint8_t>(p.refusal);
  put32(out + 28, p.wakes);
}

bool decode(const update::Entry& e, Payload* out) {
  if (e.kind != update::kind::kDutyCycle || e.size < kPayloadBytes || out == nullptr) return false;
  if (e.payload[0] > static_cast<std::uint8_t>(Wrote::kLineArmed)) return false;
  if (e.payload[1] >= static_cast<std::uint8_t>(Wake::kCount)) return false;
  if (e.payload[2] >= static_cast<std::uint8_t>(power::Band::kCount)) return false;
  if (e.payload[27] > static_cast<std::uint8_t>(SleepRefusal::kSleeperRefused)) return false;
  out->what = static_cast<Wrote>(e.payload[0]);
  out->wake = static_cast<Wake>(e.payload[1]);
  out->band = static_cast<power::Band>(e.payload[2]);
  out->flags = e.payload[3];
  out->interval_s = get32(e.payload + 4);
  out->median_mv = static_cast<std::uint16_t>(e.payload[8] | (e.payload[9] << 8));
  out->mean_ma = static_cast<std::int16_t>(
      static_cast<std::uint16_t>(e.payload[10] | (e.payload[11] << 8)));
  out->at_epoch = get32(e.payload + 12);
  out->at_ms = get32(e.payload + 16);
  out->awake_s = get32(e.payload + 20);
  out->attention = e.payload[24];
  out->spurious = e.payload[25];
  out->faults = e.payload[26];
  out->refusal = static_cast<SleepRefusal>(e.payload[27]);
  out->wakes = get32(e.payload + 28);
  return true;
}

Sleep::Sleep(interfaces::Sleeper& sleeper, board::RadioPins& pins, link::client::Client& radio,
             uplink::UplinkPolicy& uplink, WakeReason& wake, interfaces::Clock& clock,
             interfaces::WallClock& wall, update::RadioUpdate* update, update::SelfUpdate* self,
             update::Journal* journal)
    : sleeper_(sleeper),
      pins_(pins),
      radio_(radio),
      uplink_(uplink),
      wake_(wake),
      clock_(clock),
      wall_(wall),
      update_(update),
      self_(self),
      journal_(journal) {}

SleepRefusal Sleep::wouldRefuse() const {
  if (update_ != nullptr && update_->active()) return SleepRefusal::kUpdateRunning;
  if (self_ != nullptr && self_->writing()) return SleepRefusal::kTransferOpen;
  if (self_ != nullptr && self_->onTrial()) return SleepRefusal::kOnTrial;
  return SleepRefusal::kNone;
}

bool Sleep::worthALine(const power::Decision& d, const SleepResult& r, bool by_hand) const {
  if (r.refusal != SleepRefusal::kNone) return true;
  if (by_hand) return true;  // somebody did something: that is always news
  if (d.changed) return true;                      // the band, and the interval with it
  if (d.fault_doublings != 0) return true;         // the interval was lengthened by a fault
  const WakeInfo& w = wake_.info();
  if (w.line_just_disarmed || w.line_rearmed) return true;
  if (w.wake == Wake::kCrash || w.wake == Wake::kWatchdog) return true;
  if (!wake_.recordSurvived()) return true;        // the first sleep after a power cut
  return false;
}

void Sleep::journal(Wrote what, const SleepResult& r, const power::Decision& d, bool by_hand) {
  if (journal_ == nullptr) return;
  const WakeInfo& w = wake_.info();
  Payload p{};
  p.what = what;
  p.wake = w.wake;
  p.band = d.band;
  p.flags = static_cast<std::uint8_t>(
      (d.changed ? kFlagBandChanged : 0) | (d.overridden ? kFlagOverridden : 0) |
      (r.radio_line_armed ? kFlagLineArmed : 0) | (d.charging ? kFlagCharging : 0) |
      (r.notice_sent ? kFlagNoticeSent : 0) | (by_hand ? kFlagByHand : 0));
  if (d.interval_minutes * 60u != r.seconds) p.flags |= kFlagIntervalChanged;
  p.interval_s = r.seconds;
  p.median_mv = d.median_mv;
  p.mean_ma = d.current_known ? d.mean_ma : 0;
  interfaces::WallTime now{};
  p.at_epoch = wall_.now(&now) ? now.epoch : 0;
  p.at_ms = static_cast<std::uint32_t>(clock_.nowMs());
  p.awake_s = r.awake_s;
  p.attention = w.attention.reason;
  p.spurious = cap8(w.spurious_in_window);
  p.faults = cap8(w.faults_in_a_row);
  p.refusal = r.refusal;
  p.wakes = w.wakes;
  std::uint8_t bytes[kPayloadBytes];
  encode(p, bytes);
  (void)journal_->append(update::kind::kDutyCycle, bytes, kPayloadBytes);
}

SleepResult Sleep::go(const power::Decision& decision, bool arm_radio_line, bool by_hand) {
  return sleepFor(decision.interval_seconds, decision, arm_radio_line, by_hand);
}

SleepResult Sleep::goFor(std::uint32_t seconds, const power::Decision& decision,
                         bool arm_radio_line) {
  return sleepFor(seconds, decision, arm_radio_line, true);
}

SleepResult Sleep::sleepFor(std::uint32_t seconds, const power::Decision& decision,
                            bool arm_radio_line, bool by_hand) {
  SleepResult r{};
  r.seconds = seconds;
  r.radio_line_armed = arm_radio_line;
  r.band = decision.band;
  r.awake_s = static_cast<std::uint32_t>(clock_.nowMs() / 1000u);

  if (seconds == 0 && !arm_radio_line) r.refusal = SleepRefusal::kNoSeconds;
  if (r.refusal == SleepRefusal::kNone) r.refusal = wouldRefuse();
  if (r.refusal != SleepRefusal::kNone) {
    journal(Wrote::kRefused, r, decision, by_hand);
    r.journalled = journal_ != nullptr;
    last_ = r;
    return r;
  }

  // 1. The journal, when there is news. A line about the disarm is its own entry, so that "the
  // radio's line stopped being a wake source" can be found in flash without reading a sleep.
  const WakeInfo& w = wake_.info();
  if (w.line_just_disarmed) journal(Wrote::kLineDisarmed, r, decision, by_hand);
  if (w.line_rearmed) journal(Wrote::kLineArmed, r, decision, by_hand);

  // 2. SLEEP_NOTICE, as the last request (§8). A link that is down or a radio that does not
  // advertise ATTENTION is not a reason to stay awake: the notice is a courtesy to the radio's
  // own event-sending, and the refusal is recorded and printed.
  interfaces::WallTime now{};
  if (wall_.now(&now)) r.epoch = now.epoch + seconds;
  const link::client::Answer<link::StatusOnly> notice = radio_.sleepNotice(seconds);
  r.notice_fault = notice.fault;
  r.notice_sent = notice.ok();

  if (worthALine(decision, r, by_hand)) {
    journal(Wrote::kSleeping, r, decision, by_hand);
    r.journalled = journal_ != nullptr;
  }

  // 3. The modem down: always safe to call, and this is the way out of every wake.
  uplink_.takeDown();

  // 4. The pins safe, and then held (§2 rules 1 and 5), with the radio's line given the chip's
  // pull-up when it is to wake us.
  r.pins_held = pins_.holdForSleep(arm_radio_line);

  // 5. The wake's duration into its entry, and then sleep. The record is stored *before* the
  // sleep because after it there is no "after": the next statement runs in a new boot.
  wake_.endingInSleep(r.awake_s);

  interfaces::SleepPlan plan{};
  plan.seconds = seconds;
  plan.radio_line = arm_radio_line;
  if (!sleeper_.sleep(plan)) {
    // The call that does not return, returned. The pads are still held, so put them back before
    // anything else drives one.
    (void)pins_.releaseHolds();
    r.pins_held = false;
    r.refusal = SleepRefusal::kSleeperRefused;
  }
  last_ = r;
  return r;
}

}  // namespace cycle
}  // namespace core
