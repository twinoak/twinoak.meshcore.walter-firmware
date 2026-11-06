// The console's bytes on the module's USB-C, which is the ESP32-S3's own USB Serial/JTAG
// peripheral (sdkconfig.defaults: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG). The driver is installed
// here and the VFS pointed at it, so printf and the console share one interrupt-driven path.
#pragma once

#include <cstdint>

#include "interfaces/text_out.h"

namespace platform {

class UsbConsole final : public interfaces::TextOut {
 public:
  // Installs the driver. Returns false if it could not be; the firmware runs on without a console.
  bool begin();

  // Writes, turning '\n' into CR LF for the terminal (core/console/crlf.h). Blocks for at most a
  // few ticks when no host is reading: a Walter on a mast prints into nothing and must not stall
  // for it.
  void write(const char* text, std::size_t length) override;

  // Writes bytes verbatim: no conversion, and the loop keeps going until the driver has taken
  // every one of them or a second has passed (task p8-02). False means the frame was cut short.
  bool writeBytes(const std::uint8_t* bytes, std::size_t count) override;

  // One byte, or -1 after timeout_ms with nothing typed.
  int readByte(std::uint32_t timeout_ms);
};

}  // namespace platform
