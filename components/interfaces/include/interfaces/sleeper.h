// Deep sleep, as core/ sees it (task p3-04): arm the sources this sleep may be woken by, go to
// sleep, and - at the next boot - say which source woke us. And whether a USB host is on the
// port, because on this board that is the *only* way a cable can be noticed.
//
// **A wake from deep sleep is a boot.** The ESP32-S3 powers the CPUs and the digital peripherals
// down and the application starts over from `app_main` (ESP-IDF v5.5.5,
// docs/en/api-reference/system/sleep_modes.rst, *Sleep Modes*: "the CPUs, most of the RAM, and
// all digital peripherals that are clocked from APB_CLK are powered off"), so `sleep()` does not
// return on the board. What must cross a sleep goes into retained storage
// (`interfaces/retained.h`: RTC memory, which a power cut clears and a wake does not) - the
// reset record, the supervisor's counts, the uplink's backoff, and from this task the wake log
// and the band the interval policy was last in.
//
// The host's fake returns from `sleep()`, which is how a test walks a month of wakes over one
// retained map (`host_test/fakes/fake_sleeper.h`).
#pragma once

#include <cstdint>

namespace interfaces {

// Which source woke this boot, as the platform read it from the chip. Not the same question as
// `core::health::WakeCause`, which is the *chip's* code: this is the source as the sleeper armed
// it, and `core::cycle::WakeReason` is what turns either into a meaning.
enum class WokeBy : std::uint8_t {
  kNotFromSleep = 0,  // this boot was not a wake from deep sleep
  kTimer,             // the sleep timer ran out: the check-in
  kRadioLine,         // the pin the radio's TX reaches went low: a start bit
  kOther,             // a source this firmware did not arm
};

inline const char* describe(WokeBy source) {
  switch (source) {
    case WokeBy::kNotFromSleep: return "not from sleep";
    case WokeBy::kTimer: return "the timer";
    case WokeBy::kRadioLine: return "the radio's line";
    case WokeBy::kOther: return "another source";
  }
  return "?";
}

// What a sleep may be woken by. A plan with neither source is a sleep nothing but a reset ends,
// which `Sleeper::sleep()` refuses: "the chip will be in sleep modes indefinitely until external
// reset is applied" (sleep_modes.rst, *Entering Sleep*) is not a state this firmware may enter
// by accident.
struct SleepPlan {
  std::uint32_t seconds = 0;  // the timer; 0 arms none
  bool radio_line = false;    // a start bit from the radio (an RTC pin, low level)
};

class Sleeper {
 public:
  virtual ~Sleeper() = default;

  // Arms the plan's sources and goes into deep sleep. **On the board this does not return.** It
  // returns false when the sleep could not be entered at all - an empty plan, or a source the
  // chip refused - and the caller is then still awake, with the pads still held, and must put
  // them back. The host's fake returns true.
  virtual bool sleep(const SleepPlan& plan) = 0;

  // Which source woke this boot. Read once at start-up, before anything arms or disarms one.
  virtual WokeBy wokeBy() = 0;

  // Is a USB host on the port now? This board cannot wake from deep sleep when a cable is
  // plugged in - `esp_sleep_enable_usb_wakeup()` answers `ESP_ERR_NOT_SUPPORTED` on the
  // ESP32-S3, which defines no `SOC_PM_SUPPORT_USB_WAKEUP`, and the one USB wake there is is
  // light sleep's (tasks/phase-3/reports/04.md) - so the rule is: a person presses reset, and a
  // boot that finds a host stays awake.
  virtual bool usbHost() = 0;
};

}  // namespace interfaces
