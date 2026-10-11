// The pins, as core/ sees them: a number, a mode, a level. Nothing ESP-specific in a signature,
// so that host_test/ can put fake pins behind it that record every transition and check the rules
// of docs/ARCHITECTURE.md §2, and platform/ the ESP-IDF GPIO driver (task p0-03).
//
// Every call that configures or drives a pin returns whether the driver accepted it: ESP-IDF's
// GPIO calls return an error code, and the board logic (core/board/radio_pins.h) treats a refusal
// as "stop and go to the safe state" rather than carrying on with a pin in an unknown state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace interfaces {

enum class PinMode : std::uint8_t {
  kInput,        // high impedance, no pull: the pin is not driving and not pulling
  kInputPullUp,  // high impedance with the chip's weak pull-up (tens of kΩ): a quiet idle level
  kOutput,       // push-pull: drives high or low
  kOpenDrain,    // drives low when written false; does not drive at all when written true
};

class Pins {
 public:
  virtual ~Pins() = default;

  // Configures the pin. For kOutput and kOpenDrain the level last written is what is driven, so
  // a caller writes the level first and configures second: the pin never drives a level nobody
  // asked for, not even for a microsecond.
  virtual bool setMode(std::uint8_t gpio, PinMode mode) = 0;

  // Sets the level a driven mode drives. Harmless in an input mode (the level is remembered).
  virtual bool write(std::uint8_t gpio, bool high) = 0;

  // The level on the pad, whatever is driving it.
  virtual bool read(std::uint8_t gpio) = 0;

  // Holds these pads as they are now - mode and level - through deep sleep and until the holds
  // are released, so that neither entering sleep nor waking from it glitches a line. The pads are
  // configured to the wanted state before the holds go on, and configured to the same state
  // again before they come off (docs/ARCHITECTURE.md §2 rule 5).
  virtual bool holdThroughSleep(const std::uint8_t* gpios, std::size_t count, bool on) = 0;
};

}  // namespace interfaces
