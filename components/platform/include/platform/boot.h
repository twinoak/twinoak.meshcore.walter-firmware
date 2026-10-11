// Why this boot happened, read from the chip and mapped to core's enums; and the restart.
#pragma once

#include "core/health/health.h"
#include "interfaces/restart.h"

namespace platform {

core::health::ResetReason resetReason();
core::health::WakeCause wakeCause();

class EspRestart final : public interfaces::Restart {
 public:
  void restart() override;
};

}  // namespace platform
