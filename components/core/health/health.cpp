#include "core/health/health.h"

namespace core {
namespace health {

const char* name(ResetReason reason) {
  switch (reason) {
    case ResetReason::kUnknown: return "unknown";
    case ResetReason::kPowerOn: return "power-on";
    case ResetReason::kResetPin: return "reset pin";
    case ResetReason::kSoftware: return "software";
    case ResetReason::kPanic: return "panic";
    case ResetReason::kWatchdog: return "watchdog";
    case ResetReason::kBrownout: return "brownout";
    case ResetReason::kDeepSleepWake: return "wake from deep sleep";
    case ResetReason::kUsb: return "usb";
    case ResetReason::kOther: return "other";
  }
  return "?";
}

const char* name(WakeCause cause) {
  switch (cause) {
    case WakeCause::kNotFromSleep: return "not from sleep";
    case WakeCause::kTimer: return "timer";
    case WakeCause::kPin: return "pin";
    case WakeCause::kOther: return "other";
  }
  return "?";
}

const char* name(Restart request) {
  switch (request) {
    case Restart::kNotRequested: return "not requested";
    case Restart::kConsole: return "asked for on the console";
    case Restart::kSelfUpdateSwitch: return "the Walter's own update, switching to the other slot";
    case Restart::kSelfUpdateRollback: return "the Walter's own update, the trial image rejected itself";
    case Restart::kWakeOverrun: return "the wake did not reach sleep inside its bound";
  }
  return "?";
}

ResetReason fromEspResetReason(int code) {
  using namespace esp_codes;
  switch (code) {
    case kRstUnknown: return ResetReason::kUnknown;
    case kRstPowerOn: return ResetReason::kPowerOn;
    case kRstExt: return ResetReason::kResetPin;
    case kRstSw: return ResetReason::kSoftware;
    case kRstPanic: return ResetReason::kPanic;
    case kRstCpuLockup: return ResetReason::kPanic;
    case kRstIntWdt: return ResetReason::kWatchdog;
    case kRstTaskWdt: return ResetReason::kWatchdog;
    case kRstWdt: return ResetReason::kWatchdog;
    case kRstDeepSleep: return ResetReason::kDeepSleepWake;
    case kRstBrownout: return ResetReason::kBrownout;
    case kRstPwrGlitch: return ResetReason::kBrownout;
    case kRstUsb: return ResetReason::kUsb;
    case kRstSdio: return ResetReason::kOther;
    case kRstJtag: return ResetReason::kOther;
    case kRstEfuse: return ResetReason::kOther;
  }
  return ResetReason::kUnknown;
}

WakeCause fromEspWakeCause(int code) {
  using namespace esp_codes;
  switch (code) {
    case kWakeUndefined: return WakeCause::kNotFromSleep;
    case kWakeTimer: return WakeCause::kTimer;
    case kWakeExt0: return WakeCause::kPin;
    case kWakeExt1: return WakeCause::kPin;
    case kWakeGpio: return WakeCause::kPin;
    case kWakeUart: return WakeCause::kPin;
    case kWakeUart1: return WakeCause::kPin;
    case kWakeUart2: return WakeCause::kPin;
  }
  return code < 0 ? WakeCause::kNotFromSleep : WakeCause::kOther;
}

Health::Health(interfaces::Retained& retained, interfaces::Clock& clock)
    : retained_(retained), clock_(clock) {}

void Health::begin(ResetReason reason, WakeCause wake) {
  began_ms_ = clock_.nowMs();
  Record loaded{};
  survived_ = retained_.load(interfaces::RetainedSlot::kHealth, &loaded, sizeof loaded) &&
              loaded.count <= kRemembered;
  if (survived_) {
    record_ = loaded;
  } else {
    // A power cut, or the first boot of this firmware: nothing to remember, nothing to infer.
    record_ = Record{};
  }
  record_.boots++;

  BootRecord entry{};
  entry.reason = reason;
  entry.wake = reason == ResetReason::kDeepSleepWake ? wake : WakeCause::kNotFromSleep;
  entry.requested = reason == ResetReason::kSoftware ? static_cast<Restart>(record_.requested)
                                                     : Restart::kNotRequested;
  entry.previous_uptime_s = survived_ ? record_.last_uptime_s : 0;
  for (std::size_t i = kRemembered - 1; i > 0; --i) record_.items[i] = record_.items[i - 1];
  record_.items[0] = entry;
  if (record_.count < kRemembered) record_.count++;

  record_.requested = static_cast<std::uint8_t>(Restart::kNotRequested);
  record_.last_uptime_s = 0;
  store();
}

void Health::tick() {
  const std::uint32_t up = uptimeS();
  if (up == record_.last_uptime_s) return;
  record_.last_uptime_s = up;
  store();
}

void Health::noteRestart(Restart who) {
  record_.requested = static_cast<std::uint8_t>(who);
  record_.last_uptime_s = uptimeS();
  store();
}

const BootRecord& Health::at(std::size_t index) const {
  static const BootRecord kNone{};
  if (index >= record_.count) return kNone;
  return record_.items[index];
}

std::uint32_t Health::uptimeS() {
  return static_cast<std::uint32_t>((clock_.nowMs() - began_ms_) / 1000);
}

void Health::store() { retained_.store(interfaces::RetainedSlot::kHealth, &record_, sizeof record_); }

}  // namespace health
}  // namespace core
