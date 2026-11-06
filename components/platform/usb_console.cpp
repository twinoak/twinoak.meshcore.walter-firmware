#include "platform/usb_console.h"

#include <cstdint>

#include "core/console/crlf.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_vfs_common.h"
#include "freertos/FreeRTOS.h"

namespace platform {

namespace {
// Text: a few ticks, and what a port nobody reads will not take is dropped (text_out.h).
constexpr TickType_t kWriteTimeout = pdMS_TO_TICKS(20);
// Bytes: a frame goes out whole or not at all, so this waits far longer than the 20 ms above -
// a frame is at most 247 bytes (core/link/codec/wire.h, kMaxWrite) into a 1 KB transmit buffer,
// so a host that is reading at all never comes near this, and a host that is not gets one
// second of patience and a false. walterctl waits 5 s for an answer (tools/walterctl.py,
// REPLY_TIMEOUT) and the transfer gives up after 20 s of silence, so a second of blocking here
// costs a retry at worst.
constexpr TickType_t kBytesTimeout = pdMS_TO_TICKS(1000);
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
  // The conversion is core/console/crlf.h's, so that host_test/console_host/ can do the same one
  // and a test sees the bytes this board sends (task p8-02). The write itself stays as it was:
  // one call a converted piece, a few ticks each, and what does not fit is dropped.
  char buffer[core::console::kCrlfBuffer];
  std::size_t at = 0;
  while (at < length) {
    const core::console::CrlfRun run =
        core::console::toCrlf(text + at, length - at, buffer, sizeof buffer);
    if (run.put == 0) return;  // the buffer is 128 bytes: this cannot happen
    usb_serial_jtag_write_bytes(buffer, run.put, kWriteTimeout);
    at += run.taken;
  }
}

bool UsbConsole::writeBytes(const std::uint8_t* bytes, std::size_t count) {
  if (bytes == nullptr) return count == 0;
  // Verbatim, and whole: the driver takes what fits in its transmit buffer and returns how much
  // that was, so a frame larger than the room left needs more than one call. The loop keeps the
  // remaining ticks itself (an unsigned difference, so a tick-count wrap is still right) rather
  // than comparing against a deadline.
  std::size_t at = 0;
  TickType_t left = kBytesTimeout;
  TickType_t last = xTaskGetTickCount();
  while (at < count && left > 0) {
    const int wrote = usb_serial_jtag_write_bytes(bytes + at, count - at, left);
    if (wrote > 0) at += static_cast<std::size_t>(wrote);
    const TickType_t now = xTaskGetTickCount();
    const TickType_t spent = now - last;
    last = now;
    left = spent >= left ? 0 : static_cast<TickType_t>(left - spent);
  }
  return at == count;
}

int UsbConsole::readByte(std::uint32_t timeout_ms) {
  std::uint8_t byte = 0;
  const int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(timeout_ms));
  return n == 1 ? static_cast<int>(byte) : -1;
}

}  // namespace platform
