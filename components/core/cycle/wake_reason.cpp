#include "core/cycle/wake_reason.h"

namespace core {
namespace cycle {

namespace {
constexpr WakeEntry kNoEntry{};
}  // namespace

const char* name(Wake wake) {
  switch (wake) {
    case Wake::kUnknown: return "unknown";
    case Wake::kTimer: return "timer";
    case Wake::kAttention: return "attention";
    case Wake::kSpurious: return "spurious";
    case Wake::kUsb: return "usb";
    case Wake::kPowerOn: return "power-on";
    case Wake::kReset: return "reset";
    case Wake::kCrash: return "crash";
    case Wake::kWatchdog: return "watchdog";
    case Wake::kCount: break;
  }
  return "?";
}

const char* describe(Wake wake) {
  switch (wake) {
    case Wake::kUnknown: return "it is not known why this Walter is awake";
    case Wake::kTimer: return "the timer: the check-in";
    case Wake::kAttention: return "the radio's line: it raised attention";
    case Wake::kSpurious:
      return "the radio's line, and nothing valid on it: a spurious wake";
    case Wake::kUsb: return "a USB host is on the port: a person";
    case Wake::kPowerOn:
      return "a power-up: the radio was reset with us and is still booting";
    case Wake::kReset: return "a reset: the pin, or a restart somebody asked for";
    case Wake::kCrash: return "a crash: a panic, an assertion or a brownout";
    case Wake::kWatchdog: return "a watchdog: the last wake did not reach sleep";
    case Wake::kCount: break;
  }
  return "?";
}

WakeReason::WakeReason(interfaces::Retained& retained, interfaces::Clock& clock)
    : retained_(retained), clock_(clock) {}

void WakeReason::load() {
  survived_ = retained_.load(interfaces::RetainedSlot::kDutyCycle, &record_, sizeof record_);
  if (!survived_) {
    record_ = Record{};
    band_ = power::Band::kUnknown;
    wakes_in_saving_ = 0;
    asked_s_ = 0;
    count_ = 0;
    return;
  }
  if (record_.count > kWakesRemembered) record_.count = kWakesRemembered;
  count_ = record_.count;
  asked_s_ = record_.asked_s;
  band_ = record_.band < static_cast<std::uint8_t>(power::Band::kCount)
              ? static_cast<power::Band>(record_.band)
              : power::Band::kUnknown;
  wakes_in_saving_ = record_.wakes_in_saving;
}

void WakeReason::save() {
  record_.count = static_cast<std::uint8_t>(count_);
  record_.band = static_cast<std::uint8_t>(band_);
  record_.wakes_in_saving =
      wakes_in_saving_ > 0xFF ? 0xFF : static_cast<std::uint8_t>(wakes_in_saving_);
  (void)retained_.store(interfaces::RetainedSlot::kDutyCycle, &record_, sizeof record_);
}

void WakeReason::push(Wake wake, const Attention& attention) {
  for (std::size_t i = kWakesRemembered - 1; i > 0; --i) record_.items[i] = record_.items[i - 1];
  record_.items[0] = WakeEntry{wake, attention.reason, attention.detail, 0};
  if (count_ < kWakesRemembered) ++count_;
}

const WakeEntry& WakeReason::at(std::size_t index) const {
  if (index >= count_) return kNoEntry;
  return record_.items[index];
}

Wake WakeReason::listenForAttention(AttentionSource* attentions, Attention* out) {
  // §8: the radio sends 0x00 0x00, waits 300 ms and then repeats the event every second, ten
  // times, "until any valid request arrives" - and the first bytes are what woke us, so they are
  // gone. So the whole window is listened to, in slices, and the first valid attention ends it.
  const std::uint64_t began = clock_.nowMs();
  if (attentions == nullptr) {
    info_.listened_ms = 0;
    return Wake::kSpurious;
  }
  for (;;) {
    attentions->listen();
    if (attentions->take(out)) {
      const std::uint64_t now = clock_.nowMs();
      info_.listened_ms = static_cast<std::uint32_t>(now - began);
      return Wake::kAttention;
    }
    const std::uint64_t now = clock_.nowMs();
    if (now - began >= kAttentionWindowMs) {
      info_.listened_ms = static_cast<std::uint32_t>(now - began);
      return Wake::kSpurious;
    }
    clock_.delayMs(kAttentionPollMs);
  }
}

void WakeReason::begin(const health::BootRecord& boot, interfaces::WokeBy woke, bool usb_host,
                       AttentionSource* attentions) {
  load();
  info_ = WakeInfo{};
  info_.usb_host = usb_host;
  info_.woke_by = woke;
  info_.reset = boot.reason;

  // Our own power-up glitches GPIO1-14 low for about 60 us and that reaches nRESET
  // (docs/ARCHITECTURE.md §2 rule 5), so the radio was reset with us and is still booting. The
  // three reasons are the supervisor's own (core/supervisor/supervisor.cpp, classify(): a
  // power-on, a brownout, and an unknown reason read as the worst).
  info_.radio_may_be_booting = boot.reason == health::ResetReason::kPowerOn ||
                               boot.reason == health::ResetReason::kBrownout ||
                               boot.reason == health::ResetReason::kUnknown;

  Attention attention{};
  Wake wake = Wake::kUnknown;
  switch (boot.reason) {
    case health::ResetReason::kDeepSleepWake:
      switch (woke) {
        case interfaces::WokeBy::kTimer:
          wake = Wake::kTimer;
          break;
        case interfaces::WokeBy::kRadioLine:
          wake = listenForAttention(attentions, &attention);
          break;
        case interfaces::WokeBy::kOther:
        case interfaces::WokeBy::kNotFromSleep:
          // The chip says it came out of deep sleep and the sleeper cannot say what for. Fall
          // back on the chip's own wake cause, which health has already mapped: a pin is the
          // only pin this firmware arms.
          wake = boot.wake == health::WakeCause::kTimer  ? Wake::kTimer
                 : boot.wake == health::WakeCause::kPin ? listenForAttention(attentions, &attention)
                                                        : Wake::kUnknown;
          break;
      }
      break;
    case health::ResetReason::kPowerOn:
      wake = Wake::kPowerOn;
      break;
    case health::ResetReason::kResetPin:
    case health::ResetReason::kUsb:
      wake = Wake::kReset;
      break;
    case health::ResetReason::kSoftware:
      // This firmware's own bound on a wake ends it by reset and records who asked, so that the
      // next wake can tell it from a person's `reboot` (core/cycle/wake_guard.h).
      wake = boot.requested == health::Restart::kWakeOverrun ? Wake::kWatchdog : Wake::kReset;
      break;
    case health::ResetReason::kPanic:
    case health::ResetReason::kBrownout:
      wake = Wake::kCrash;
      break;
    case health::ResetReason::kWatchdog:
      wake = Wake::kWatchdog;
      break;
    case health::ResetReason::kOther:
    case health::ResetReason::kUnknown:
      wake = Wake::kUnknown;
      break;
  }

  // A boot that found a USB host is a person, whatever woke the board: this board cannot wake on
  // a cable (see the header), so the cable was plugged in and reset pressed, or it was there all
  // along. The reason the chip gave is kept in `info_.reset` either way, and a *crash* is never
  // relabelled - a panic with a laptop attached is still a panic and still counts.
  if (usb_host && wake != Wake::kCrash && wake != Wake::kWatchdog) wake = Wake::kUsb;

  // The spurious-wake rate limit. A timer wake is what re-arms the line: it is the only thing
  // that marks the passing of time a Walter can be sure of.
  const bool was_disarmed = record_.disarmed != 0;
  if (wake == Wake::kTimer) {
    record_.spurious_in_window = 0;
    if (was_disarmed) {
      record_.disarmed = 0;
      info_.line_rearmed = true;
    }
  } else if (wake == Wake::kSpurious) {
    if (record_.spurious_in_window < 0xFF) ++record_.spurious_in_window;
    if (record_.spurious_total != 0xFFFFFFFFu) ++record_.spurious_total;
    if (record_.spurious_in_window >= kSpuriousPerWindow && !was_disarmed) {
      record_.disarmed = 1;
      info_.line_just_disarmed = true;
    }
  } else if (wake == Wake::kAttention) {
    // A real attention is not chatter: the window starts again.
    record_.spurious_in_window = 0;
  }

  // Faults in a row: anything that is not a fault ends the run.
  if (wake == Wake::kCrash || wake == Wake::kWatchdog) {
    if (record_.faults_in_a_row < 0xFF) ++record_.faults_in_a_row;
  } else {
    record_.faults_in_a_row = 0;
  }

  if (record_.wakes != 0xFFFFFFFFu) ++record_.wakes;
  push(wake, attention);

  info_.wake = wake;
  info_.attention = attention;
  info_.line_disarmed = record_.disarmed != 0;
  info_.spurious_in_window = record_.spurious_in_window;
  info_.spurious_total = record_.spurious_total;
  info_.faults_in_a_row = record_.faults_in_a_row;
  info_.wakes = record_.wakes;
  begun_ = true;
  save();
}

void WakeReason::bandDecided(power::Band band) {
  // Leaving `saving` forgets the every-Nth counter, so that a pack which recovers and then sinks
  // again gets a check-in on the wake it re-enters the band, and not four wakes later.
  if (band != power::Band::kSaving) wakes_in_saving_ = 0;
  band_ = band;
  if (begun_) save();
}

void WakeReason::modemWasUsed() {
  // **1 and not 0.** 0 is "the modem has not been used since this band was entered", which is a
  // wake the modem may be used in; 1 is "it was used last wake". So the counter runs 1, 2, 3, 4
  // and the fourth wake after a use is the next one that may (core/power/interval_policy.h).
  wakes_in_saving_ = 1;
  if (begun_) save();
}

void WakeReason::modemWasSkipped() {
  if (wakes_in_saving_ < 0xFF) ++wakes_in_saving_;
  if (begun_) save();
}

// The Walter's own attention (task p4-04). Additive, and stored at once rather than at the end
// of the wake: what raises it is a 401 or a refused restart, and both are moments a wake may not
// come back from.
void WakeReason::raiseAgentAttention(std::uint8_t bits) {
  if (bits == 0) return;
  record_.agent_attention = static_cast<std::uint8_t>(record_.agent_attention | bits);
  if (begun_) save();
}

void WakeReason::clearAgentAttention() {
  if (record_.agent_attention == 0) return;
  record_.agent_attention = 0;
  if (begun_) save();
}

// What this wake did, as it becomes true (task p10-02). Additive and stored at once, for the
// reason the attention above is: a wake may not come back from the thing it is doing, and these
// bits are what the next boot's `wake` has to say about it.
void WakeReason::didThisWake(std::uint8_t bits) {
  if (bits == 0 || count_ == 0) return;
  WakeEntry& e = record_.items[0];
  const std::uint8_t want = static_cast<std::uint8_t>(bits | kDidSaid);
  if ((e.did & want) == want) return;  // nothing new: no write
  e.did = static_cast<std::uint8_t>(e.did | want);
  if (begun_) save();
}

// Why this wake sent nothing (task p13-02), into this wake's own entry. The same shape as the
// bits above: nothing new is no write, and a write goes to retained storage at once so that a
// wake ended by a reset, a crash or the bound still leaves what it had settled.
void WakeReason::sentThisWake(std::uint8_t reached, std::uint32_t owed_s) {
  if (count_ == 0) return;
  const std::uint16_t owed = owed_s > kOwedAtMostS ? static_cast<std::uint16_t>(kOwedAtMostS)
                                                   : static_cast<std::uint16_t>(owed_s);
  WakeEntry& e = record_.items[0];
  if (e.sent == reached && e.owed_s == owed) return;
  e.sent = reached;
  e.owed_s = owed;
  if (begun_) save();
}

void WakeReason::endingInSleep(std::uint32_t lasted_s, std::uint32_t asked_s) {
  // 0 is reserved for "it never reached sleep", so a wake shorter than a second is recorded as
  // one second: it did reach sleep, and that is the thing the entry is asked about. And the
  // ceiling is the field's: a wake longer than 18 hours is recorded as 18 hours rather than
  // wrapping to a small number, which is the one way sixteen bits could lie (task p10-02).
  const std::uint32_t kept = lasted_s == 0 ? 1 : lasted_s;
  record_.items[0].lasted_s =
      static_cast<std::uint16_t>(kept > kLastedAtMostS ? kLastedAtMostS : kept);
  // And what this sleep asks its timer for, for the boot on the other side of it to read. 0 is
  // honest here: a sleep with no timer at all (the radio's line alone) is a sleep whose length
  // nothing can know, and `timeAwayMs()` answers 0 for it.
  record_.asked_s = asked_s;
  save();
}

std::uint32_t WakeReason::timeAwayMs() const {
  // Only a timer wake. Everything else was cut short by whatever woke the board, and nothing
  // here knows how much of the sleep had passed when it was (see the header).
  if (!begun_ || info_.wake != Wake::kTimer || asked_s_ == 0) return 0;
  // In 64 bits: the longest interval the policy can ask for is a day doubled four times by faults
  // (core/power/interval_policy.h), and a person's `sleep <seconds>` is whatever they typed.
  const std::uint64_t believed =
      static_cast<std::uint64_t>(asked_s_) * 1000u * (100u - kTimerErrorPercent) / 100u;
  return believed > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(believed);
}

void WakeReason::forget() {
  record_ = Record{};
  count_ = 0;
  band_ = power::Band::kUnknown;
  wakes_in_saving_ = 0;
  asked_s_ = 0;
  info_ = WakeInfo{};
  begun_ = false;
  survived_ = false;
  save();
}

}  // namespace cycle
}  // namespace core
