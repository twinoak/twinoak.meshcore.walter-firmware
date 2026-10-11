#include "platform/usb_console.h"

#include <cstdint>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_vfs_common.h"
#include "freertos/FreeRTOS.h"

namespace platform {

namespace {
constexpr TickType_t kWriteTimeout = pdMS_TO_TICKS(20);
}

bool UsbConsole::begin() {
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.rx_buffer_size = 256;
  config.tx_buffer_size = 1024;
  if (usb_serial_jtag_driver_install(&config) != ESP_OK) return false;
  // stdout (ESP-IDF's own log lines, and main's printf) goes through the same driver, with the
  // VFS's LF -> CR LF conversion; this class does its own conversion below.
  usb_serial_jtag_vfs_use_driver();
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
  return true;
}

void UsbConsole::write(const char* text, std::size_t length) {
  std::size_t start = 0;
  for (std::size_t i = 0; i < length; ++i) {
    if (text[i] != '\n') continue;
    if (i > start) usb_serial_jtag_write_bytes(text + start, i - start, kWriteTimeout);
    usb_serial_jtag_write_bytes("\r\n", 2, kWriteTimeout);
    start = i + 1;
  }
  if (length > start) usb_serial_jtag_write_bytes(text + start, length - start, kWriteTimeout);
}

int UsbConsole::readByte(std::uint32_t timeout_ms) {
  std::uint8_t byte = 0;
  const int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(timeout_ms));
  return n == 1 ? static_cast<int>(byte) : -1;
}

}  // namespace platform
