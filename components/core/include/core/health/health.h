// Why the last boot happened (task p0-03), after the agent firmware's Health (src/Health.h at
// 51faafb427cba41b0de9983736b6621bc155f829): the board's reset reason and wake cause mapped to a
// small enum, a boot counter, and the last five boots with their reasons and how long the run
// before each lasted - kept in retained storage, so that a Walter found quiet can say what
// happened to it. A power cut clears the record, and says so.
//
// The mapping from ESP-IDF's codes is here, in core/, so that it has a host test; platform/boot
// static_asserts that the codes it copies are ESP-IDF's.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interfaces/clock.h"
#include "interfaces/retained.h"

namespace core {
namespace health {

enum class ResetReason : std::uint8_t {
  kUnknown = 0,
  kPowerOn,        // a power cut, or the first power-up
  kResetPin,       // the module's reset pin (the adapter's SW1, the recovery header)
  kSoftware,       // esp_restart(): asked for (see Restart below)
  kPanic,          // an exception, an assertion, a CPU lock-up
  kWatchdog,       // the task, interrupt or RTC watchdog
  kBrownout,       // the brownout detector, or a power glitch
  kDeepSleepWake,  // not a fault: woke from deep sleep (then WakeCause says why)
  kUsb,            // reset by the USB peripheral (a host's request, e.g. esptool)
  kOther,          // SDIO, JTAG, eFuse: not expected on a mast
};

enum class WakeCause : std::uint8_t {
  kNotFromSleep = 0,  // this boot was not a wake from deep sleep
  kTimer,             // the sleep timer: the check-in
  kPin,               // a pin: the radio's line (ext0/ext1/GPIO/UART wake-ups)
  kOther,
};

// Who asked for a software restart, recorded before it happens so that the next boot can say so
// instead of only "software reset". Later tasks add their own; the numbers are stored.
enum class Restart : std::uint8_t {
  kNotRequested = 0,
  kConsole = 1,  // `reboot`
};

const char* name(ResetReason reason);
const char* name(WakeCause cause);
const char* name(Restart request);

// ESP-IDF's codes (esp_reset_reason_t in components/esp_system/include/esp_system.h and
// esp_sleep_wakeup_cause_t in components/esp_hw_support/include/esp_sleep.h, v5.5.5), copied so
// that the mapping is portable; platform/boot.cpp holds each against the real enumerator.
namespace esp_codes {
constexpr int kRstUnknown = 0, kRstPowerOn = 1, kRstExt = 2, kRstSw = 3, kRstPanic = 4,
              kRstIntWdt = 5, kRstTaskWdt = 6, kRstWdt = 7, kRstDeepSleep = 8, kRstBrownout = 9,
              kRstSdio = 10, kRstUsb = 11, kRstJtag = 12, kRstEfuse = 13, kRstPwrGlitch = 14,
              kRstCpuLockup = 15;
constexpr int kWakeUndefined = 0, kWakeAll = 1, kWakeExt0 = 2, kWakeExt1 = 3, kWakeTimer = 4,
              kWakeTouchpad = 5, kWakeUlp = 6, kWakeGpio = 7, kWakeUart = 8, kWakeUart1 = 9,
              kWakeUart2 = 10;
}  // namespace esp_codes

ResetReason fromEspResetReason(int code);
WakeCause fromEspWakeCause(int code);

constexpr std::size_t kRemembered = 5;

// One boot. Plain data: it is stored as bytes.
struct BootRecord {
  ResetReason reason;
  WakeCause wake;
  Restart requested;              // who asked, when reason is kSoftware (else kNotRequested)
  std::uint32_t previous_uptime_s;  // how long the run before this boot lasted; 0 if unknown
};

class Health {
 public:
  Health(interfaces::Retained& retained, interfaces::Clock& clock);

  // At boot, with what the platform read from the chip. Reads the record, counts this boot,
  // pushes it as the newest entry (with the previous run's last known uptime), stores it.
  void begin(ResetReason reason, WakeCause wake);

  // About once a second: keeps "the uptime, a moment ago" in the record, which is what the next
  // boot reports as the previous run's length.
  void tick();

  // Before a planned restart: the next boot's record says who asked.
  void noteRestart(Restart who);

  bool recordSurvived() const { return survived_; }  // false after a power cut: the log starts here
  std::uint32_t boots() const { return record_.boots; }
  std::size_t count() const { return record_.count; }
  const BootRecord& at(std::size_t index) const;  // 0 is this boot
  const BootRecord& thisBoot() const { return at(0); }
  std::uint32_t uptimeS();

 private:
  struct Record {  // interfaces::RetainedSlot::kHealth
    std::uint32_t boots;          // since the record began (a power cut)
    std::uint32_t last_uptime_s;  // the uptime, a moment ago
    std::uint8_t count;           // how many of items[] are real
    std::uint8_t requested;       // a Restart, set before a planned restart
    std::uint8_t reserved[2];
    BootRecord items[kRemembered];
  };

  void store();

  interfaces::Retained& retained_;
  interfaces::Clock& clock_;
  Record record_{};
  std::uint64_t began_ms_ = 0;
  bool survived_ = false;
};

}  // namespace health
}  // namespace core
