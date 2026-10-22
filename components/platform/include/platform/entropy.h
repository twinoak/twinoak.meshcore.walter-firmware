// interfaces::Entropy on ESP-IDF: esp_random().
#pragma once

#include "interfaces/entropy.h"

namespace platform {

class EspEntropy final : public interfaces::Entropy {
 public:
  std::uint32_t bits32() override;
};

}  // namespace platform
