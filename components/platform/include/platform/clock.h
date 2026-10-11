// interfaces::Clock on ESP-IDF: esp_timer for the time, FreeRTOS for the long delays, the ROM's
// busy loop for the short ones.
#pragma once

#include "interfaces/clock.h"

namespace platform {

class EspClock final : public interfaces::Clock {
 public:
  std::uint64_t nowMs() override;
  void delayMs(std::uint32_t ms) override;
  void delayUs(std::uint32_t us) override;
};

}  // namespace platform
