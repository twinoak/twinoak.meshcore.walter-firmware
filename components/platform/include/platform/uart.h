// interfaces::ByteStream on the UART to the radio: UART1, 115200 8N1, no flow control, on the two
// pins of spec/LINK-PROTOCOL.md §1 and docs/ARCHITECTURE.md §2 (task p1-02). Thin: no decision is
// taken here - which bytes go out and when is core/link/client's.
//
// The one thing worth reading twice is open()/close(). The pads belong to RadioPins, which drives
// TX only between uartEnable() and uartRelease() (ARCHITECTURE §2 rule 1), so the peripheral is
// routed to them in open() and routed *away* in close(), leaving the pad as a GPIO at the level
// RadioPins last wrote. core/link/client/port.h is the only caller of either and sequences the two
// halves; nothing here knows the rules.
#pragma once

#include <cstdint>

#include "interfaces/byte_stream.h"

namespace platform {

class RadioUart final : public interfaces::ByteStream {
 public:
  // Installs the driver and configures the port, without taking a pin. False when the driver
  // could not be installed; the firmware then runs on with no link (and says so).
  bool begin();

  bool open() override;
  void close() override;
  std::size_t read(std::uint8_t* out, std::size_t max) override;
  std::size_t write(const std::uint8_t* data, std::size_t size) override;
  bool flush(std::uint32_t timeout_ms) override;
  bool idleFor(std::uint32_t ms) override;

  bool installed() const { return installed_; }

 private:
  void touch();  // something happened on the line, now

  bool installed_ = false;
  bool open_ = false;
  std::uint64_t last_activity_ms_ = 0;
};

}  // namespace platform
