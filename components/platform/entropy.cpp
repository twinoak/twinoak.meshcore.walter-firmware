#include "platform/entropy.h"

#include "esp_random.h"

namespace platform {

// esp_random(): "the RNG is a true random number generator ... as long as an RF subsystem is
// enabled", and without one it is a pseudo-random sequence seeded at boot
// (docs/en/api-reference/system/random.rst, v5.5.5). On this unit the modem is a separate
// module on a UART and the ESP32-S3's own radios are never brought up, so what this returns is
// the seeded sequence - which is exactly the contract interfaces/entropy.h asks for, because
// the seed is drawn from the hardware at start-up and differs between two units that booted
// together. Nothing in this firmware signs or encrypts with it.
std::uint32_t EspEntropy::bits32() { return esp_random(); }

}  // namespace platform
