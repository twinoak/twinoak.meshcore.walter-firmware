#include "platform/pins.h"

#include "driver/gpio.h"

namespace platform {

using interfaces::PinMode;

// gpio_config() (components/esp_driver_gpio/src/gpio.c, v5.5.5) sets the direction, both pulls,
// disables the interrupt, and selects the pad's GPIO function in the IO MUX - so a pin a
// peripheral once owned comes back to us. The output modes are the "input + output" variants
// so that gpio_get_level() reads the pad, which is what `pins` prints and what lets us see the
// line nRESET actually sits at while we pull it low.
bool EspPins::setMode(std::uint8_t gpio, PinMode mode) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << gpio;
  config.intr_type = GPIO_INTR_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  switch (mode) {
    case PinMode::kInput: config.mode = GPIO_MODE_INPUT; break;
    case PinMode::kInputPullUp:
      config.mode = GPIO_MODE_INPUT;
      config.pull_up_en = GPIO_PULLUP_ENABLE;
      break;
    case PinMode::kOutput: config.mode = GPIO_MODE_INPUT_OUTPUT; break;
    case PinMode::kOpenDrain: config.mode = GPIO_MODE_INPUT_OUTPUT_OD; break;
  }
  return gpio_config(&config) == ESP_OK;
}

bool EspPins::write(std::uint8_t gpio, bool high) {
  return gpio_set_level(static_cast<gpio_num_t>(gpio), high ? 1 : 0) == ESP_OK;
}

bool EspPins::read(std::uint8_t gpio) { return gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0; }

// gpio_hold_en() freezes the pad (driver/gpio.h: "the GPIO's output level and direction are
// latched"); gpio_deep_sleep_hold_en() keeps those holds through deep sleep ("if you want to hold
// the state of a digital GPIO during Deep-sleep, please call gpio_deep_sleep_hold_en"). Off: the
// reverse, in the reverse order. The caller has configured the pads to the wanted state before
// either (interfaces/pins.h).
bool EspPins::holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) {
  bool ok = true;
  if (on) {
    for (std::size_t i = 0; i < count; ++i) {
      ok = gpio_hold_en(static_cast<gpio_num_t>(gpios[i])) == ESP_OK && ok;
    }
    gpio_deep_sleep_hold_en();
  } else {
    gpio_deep_sleep_hold_dis();
    for (std::size_t i = 0; i < count; ++i) {
      ok = gpio_hold_dis(static_cast<gpio_num_t>(gpios[i])) == ESP_OK && ok;
    }
  }
  return ok;
}

}  // namespace platform
