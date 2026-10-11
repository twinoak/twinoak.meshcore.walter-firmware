// interfaces::Retained in the ESP32-S3's RTC memory: one slot per RetainedSlot, each a magic, a
// size, the bytes and a CRC-32 over the rest. RTC_NOINIT_ATTR memory is not touched by the
// bootloader or by start-up, so it survives a software reset, a panic, a watchdog and a deep
// sleep; a power cut leaves it random, which the checksum catches.
#pragma once

#include "interfaces/retained.h"

namespace platform {

class RtcRetained final : public interfaces::Retained {
 public:
  bool load(interfaces::RetainedSlot slot, void* out, std::size_t size) override;
  bool store(interfaces::RetainedSlot slot, const void* data, std::size_t size) override;
};

}  // namespace platform
