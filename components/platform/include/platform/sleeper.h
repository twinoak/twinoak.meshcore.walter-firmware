// interfaces::Sleeper on ESP-IDF's deep sleep (task p3-04; docs/ARCHITECTURE.md §10). Thin: the
// decisions - how long, which sources, in what order the pads are handled - are
// core/cycle/sleep.h's; this is the calls, each with the document it came from beside it.
//
// Read the .cpp for the order around sleep entry and exit, which is the thing a bench scope is
// pointed at. The three answers it embodies, for a reader in a hurry:
//
//   * **the board's own sleep, not the modem library's.** `WalterModem::sleep()` arms the timer
//     and nothing else (`esp_deep_sleep(us)`), so neither the radio's line nor anything else
//     could bring the Walter back, and it gives nothing a chance to hold a pad, write the
//     journal or tell the radio first.
//   * **ext1, not ext0, for the radio's line.** Both reach GPIO8; ext0 keeps the whole RTC
//     peripheral domain powered through the sleep and ext1 does not, and the sleep current is
//     what this task exists for.
//   * **USB cannot wake this board.** `esp_sleep_enable_usb_wakeup()` answers
//     `ESP_ERR_NOT_SUPPORTED` on the ESP32-S3. A boot that finds a host stays awake instead.
#pragma once

#include <cstdint>

#include "interfaces/sleeper.h"

namespace platform {

class EspSleeper final : public interfaces::Sleeper {
 public:
  // At start-up, before anything arms or disarms a source and before the pads are reconfigured:
  // the wake cause is read and kept, and the pad the radio's line was armed on is routed back
  // from the RTC mux to the IO mux (`rtc_gpio_deinit`), which ESP-IDF requires before it can be
  // a digital GPIO again. Safe to call on a boot that was not a wake.
  void begin();

  bool sleep(const interfaces::SleepPlan& plan) override;
  interfaces::WokeBy wokeBy() override;
  bool usbHost() override;

  // The chip's own code, for the console: `esp_sleep_get_wakeup_cause()`'s value.
  int rawCause() const { return raw_cause_; }

 private:
  int raw_cause_ = 0;  // ESP_SLEEP_WAKEUP_UNDEFINED
  bool begun_ = false;
};

}  // namespace platform
