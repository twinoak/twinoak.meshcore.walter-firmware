// interfaces::Pins on ESP-IDF's GPIO driver. Thin: a mode is one gpio_config(), a level one
// gpio_set_level(), a hold one gpio_hold_en(). No decision is taken here (core/board/radio_pins).
#pragma once

#include "interfaces/pins.h"

namespace platform {

class EspPins final : public interfaces::Pins {
 public:
  bool setMode(std::uint8_t gpio, interfaces::PinMode mode) override;
  bool write(std::uint8_t gpio, bool high) override;
  bool read(std::uint8_t gpio) override;
  bool holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) override;
};

}  // namespace platform
