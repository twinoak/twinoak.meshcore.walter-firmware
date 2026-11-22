#include "platform/uart.h"

#include "driver/uart.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/gpio_sig_map.h"

#include "core/board/pin_map.h"
#include "core/link/client/port.h"

namespace platform {

namespace {

// UART0 is the ESP32-S3's own console (which this board does not use - the console is on USB
// Serial/JTAG, sdkconfig.defaults) and UART1 is free. 115200 8N1, no flow control: §1's table.
constexpr uart_port_t kPort = UART_NUM_1;
constexpr int kBaud = static_cast<int>(core::link::client::kBaud);

// **The receive ring, by arithmetic** (task p18-03). The number and the reasoning are
// `core/link/client/port.h`'s - the baud rate and the longest the reader is away between two
// reads - because what sizes the ring is the session loop and not the driver. 4 096 bytes: 355 ms
// of this line, better than twice the 150 ms the reader may be away, and sixteen frames of §2.
//
// It was 1 024 until this task, with the note that a task away for a tick loses nothing. A task
// away for a *call to the server* loses nine lines of a `hist`, which is what the unit at Bellevue
// did on 21 November.
constexpr int kRxBuffer = static_cast<int>(core::link::client::kRxRingBytes);

// **The queue that says a loss happened.** The driver is installed with one from this task on, and
// read() drains it: `UART_BUFFER_FULL` is the ring full - the ISR could not push what it had
// stashed, dropped it and turned the receive interrupts off until the reader drains
// (`esp_driver_uart/src/uart.c`, the `xRingbufferSendFromISR` that fails at ~L1304) - and
// `UART_FIFO_OVF` is the hardware's own 128-byte FIFO over, after which the ISR resets it
// (~L1371). Neither event carries a size: `uart_event_t::size` is "UART data size for UART_DATA
// event" (`driver/uart.h`), so what is counted is the moment and not the bytes.
//
// Eight entries, which is what the queue needs to be useful: the events are counted and thrown
// away at every read, and a queue that overflows loses only the *word* that a loss happened - so
// the count is a floor and never an invention.
constexpr int kEventQueue = 8;

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
  // which is what §2's "send a frame in one piece" wants of a sender. **And an event queue**, so
  // that a ring that filled or a FIFO that ran over is something this port can say (task p18-03);
  // without one the driver drops the events and nothing anywhere knows a byte went.
  QueueHandle_t events = nullptr;
  if (uart_driver_install(kPort, kRxBuffer, 0, kEventQueue, &events, 0) != ESP_OK) {
    return false;
  }
  events_ = events;
  if (uart_param_config(kPort, &config) != ESP_OK) {
    uart_driver_delete(kPort);  // which deletes the event queue with it
    events_ = nullptr;
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

// Whatever the driver has queued since the last read, counted and thrown away. Never blocks.
void RadioUart::drainEvents() {
  if (events_ == nullptr) {
    return;
  }
  uart_event_t event;
  while (xQueueReceive(static_cast<QueueHandle_t>(events_), &event, 0) == pdTRUE) {
    if (event.type == UART_BUFFER_FULL || event.type == UART_FIFO_OVF) {
      if (losses_ != 0xFFFFFFFFu) {
        ++losses_;  // saturating: a count that has stopped being exact must not start being wrong
      }
    }
  }
}

std::size_t RadioUart::read(std::uint8_t* out, std::size_t max) {
  if (!open_ || out == nullptr || max == 0) {
    return 0;
  }
  // Before the bytes, so that a loss the driver noticed while this reader was away is counted
  // with the read that follows it and not with the one after.
  drainEvents();
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
