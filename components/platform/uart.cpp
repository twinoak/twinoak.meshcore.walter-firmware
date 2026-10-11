#include "platform/uart.h"

#include "driver/uart.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_sig_map.h"

#include "core/board/pin_map.h"

namespace platform {

namespace {

// UART0 is the ESP32-S3's own console (which this board does not use - the console is on USB
// Serial/JTAG, sdkconfig.defaults) and UART1 is free. 115200 8N1, no flow control: §1's table.
constexpr uart_port_t kPort = UART_NUM_1;
constexpr int kBaud = 115200;

// The receive buffer. ESP-IDF's driver insists on at least 128 bytes; a frame of this link is at
// most 246 (§2), and 1 KB is eight of them - enough that a task away for a tick loses nothing,
// small enough to be invisible in 512 KB of RAM.
constexpr int kRxBuffer = 1024;

// The two pins, from the one place that names them (core/board/pin_map.h, which is
// INTERCONNECT.md's table): GPIO7 is the Walter's TX into the radio's RX, GPIO8 the other way.
constexpr int kTx = core::board::uartTxGpio();
constexpr int kRx = core::board::uartRxGpio();

std::uint64_t nowMs() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }

}  // namespace

bool RadioUart::begin() {
  if (installed_) {
    return true;
  }
  const uart_config_t config = {
      .baud_rate = kBaud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
      .flags = {},
  };
  // No transmit ring: uart_write_bytes then returns when the bytes are in the hardware's hands,
  // which is what §2's "send a frame in one piece" wants of a sender.
  if (uart_driver_install(kPort, kRxBuffer, 0, 0, nullptr, 0) != ESP_OK) {
    return false;
  }
  if (uart_param_config(kPort, &config) != ESP_OK) {
    uart_driver_delete(kPort);
    return false;
  }
  installed_ = true;
  last_activity_ms_ = nowMs();
  return true;
}

bool RadioUart::open() {
  if (!installed_ && !begin()) {
    return false;
  }
  if (open_) {
    return true;
  }
  // The pads are RadioPins' and have just been enabled by it (core/link/client/port.cpp): route
  // the peripheral to them now. UART_PIN_NO_CHANGE leaves RTS and CTS alone - there are none.
  if (uart_set_pin(kPort, kTx, kRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    return false;
  }
  uart_flush_input(kPort);  // whatever arrived while the line was nobody's
  open_ = true;
  last_activity_ms_ = nowMs();
  return true;
}

void RadioUart::close() {
  if (!open_) {
    return;
  }
  uart_wait_tx_done(kPort, pdMS_TO_TICKS(200));
  // Route the transmitter away from the pad, so that the pad is a GPIO again at the level
  // RadioPins last wrote (high) and uartRelease() can turn it into an input with its pull-up
  // without the UART driving underneath. The receiver needs nothing: an input is an input.
  esp_rom_gpio_connect_out_signal(kTx, SIG_GPIO_OUT_IDX, false, false);
  open_ = false;
}

std::size_t RadioUart::read(std::uint8_t* out, std::size_t max) {
  if (!open_ || out == nullptr || max == 0) {
    return 0;
  }
  const int got = uart_read_bytes(kPort, out, static_cast<std::uint32_t>(max), 0);
  if (got <= 0) {
    return 0;
  }
  touch();
  return static_cast<std::size_t>(got);
}

std::size_t RadioUart::write(const std::uint8_t* data, std::size_t size) {
  if (!open_ || data == nullptr || size == 0) {
    return 0;
  }
  const int wrote = uart_write_bytes(kPort, data, size);
  if (wrote <= 0) {
    return 0;
  }
  touch();
  return static_cast<std::size_t>(wrote);
}

bool RadioUart::flush(std::uint32_t timeout_ms) {
  if (!open_) {
    return false;
  }
  return uart_wait_tx_done(kPort, pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

bool RadioUart::idleFor(std::uint32_t ms) {
  if (!open_) {
    return false;
  }
  const std::uint64_t now = nowMs();
  if (now < last_activity_ms_) {
    return true;  // the clock restarted (a wake from deep sleep): the line has been quiet since
  }
  return now - last_activity_ms_ >= ms;
}

void RadioUart::touch() { last_activity_ms_ = nowMs(); }

}  // namespace platform
