#include "platform/sleeper.h"

#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "core/board/pin_map.h"

namespace platform {

namespace {

// The pin a start bit from the radio wakes us on, from the one place the seven pins are named
// (core/board/pin_map.h; tools/verify.sh greps that their *names* stay there).
constexpr gpio_num_t kWakePin = static_cast<gpio_num_t>(core::board::wakePinGpio());

// GPIO8 has to be an RTC IO for any of this to work. The ESP32-S3's are GPIO0-21
// (SOC_RTCIO_PIN_COUNT, components/soc/esp32s3/include/soc/soc_caps.h at ESP-IDF v5.5.5; and
// esp_sleep.h says the same of ext0: "ESP32-S3: 0-21"). A build for a chip where it is not is a
// failed build and not a Walter that never wakes.
static_assert(SOC_RTCIO_PIN_COUNT >= 9, "the radio's line has to be an RTC IO to wake us");
static_assert(SOC_PM_SUPPORT_EXT1_WAKEUP, "this chip has no ext1 wake-up");

// How long the USB connection monitor is given to settle before its answer is believed.
// `usb_serial_jtag_conn_status_init()` sets the status to *connected* and then lets a FreeRTOS
// tick hook disprove it - "Always assume USB Serial/JTAG is connected until we are sure it is
// not connected ... Consider it is disconnected only if SOF intr bit is not raised within
// (ALLOWED_NO_SOF_TICKS + 1) tick periods", USJ_DISCONNECT_CONFIRM_PERIOD_MS being 3
// (components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_connection_monitor.c, v5.5.5). So a
// read in the first few milliseconds of a boot always says "a host is there". Twenty
// milliseconds is several ticks at any configured tick rate down to 100 Hz with room over.
constexpr std::uint32_t kUsbSettleMs = 20;

}  // namespace

void EspSleeper::begin() {
  raw_cause_ = static_cast<int>(esp_sleep_get_wakeup_cause());
  // "To use the EXT1 wakeup, the IO pad(s) are configured as RTC IO. Therefore, before using
  // these pads as digital GPIOs, users need to reconfigure them by calling the rtc_gpio_deinit
  // function" (docs/en/api-reference/system/sleep_modes.rst, *External Wakeup (ext1)*). The pad
  // is the radio's RX, which the UART needs back; the hold ESP-IDF put on it is released with
  // the rest by RadioPins::releaseHolds(), and this is what routes the pad from the RTC mux back
  // to the IO mux. Harmless on a boot that was not a wake.
  (void)rtc_gpio_deinit(kWakePin);
  // Nothing of a previous boot's arming is left in force. esp_deep_sleep_start() would arm only
  // what this boot asked for anyway; this is so that `sleep` can be read as "the sources are
  // exactly the plan's".
  (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  begun_ = true;
}

interfaces::WokeBy EspSleeper::wokeBy() {
  if (!begun_) begin();
  switch (raw_cause_) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return interfaces::WokeBy::kNotFromSleep;
    case ESP_SLEEP_WAKEUP_TIMER:
      return interfaces::WokeBy::kTimer;
    case ESP_SLEEP_WAKEUP_EXT1:
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_GPIO:
      // ext1 is what this firmware arms; the other two are read as the same thing because the
      // only pin it ever arms is the radio's line.
      return interfaces::WokeBy::kRadioLine;
    default:
      return interfaces::WokeBy::kOther;
  }
}

bool EspSleeper::usbHost() {
  // The connection monitor is linked in because the console is the USB Serial/JTAG peripheral
  // (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, sdkconfig.defaults; the component's CMakeLists.txt
  // pulls `usb_serial_jtag_connection_monitor` in under exactly that condition). "USJ is
  // considered connected so long as it is receiving SOF packets from the host, even if there is
  // no serial communications occurring ... Having the USB port connected to a power bank will
  // never be considered as connected" (driver/usb_serial_jtag.h) - which is the right question:
  // a power bank is not a person.
  const std::uint32_t since_boot =
      static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
  if (since_boot < kUsbSettleMs) vTaskDelay(pdMS_TO_TICKS(kUsbSettleMs - since_boot));
  return usb_serial_jtag_is_connected();
}

bool EspSleeper::sleep(const interfaces::SleepPlan& plan) {
  if (!begun_) begin();
  // A sleep with neither source is one only a reset would end ("the chip will be in sleep modes
  // indefinitely until external reset is applied", sleep_modes.rst, *Entering Sleep*). Refused
  // here as well as by the caller, so that no path reaches it.
  if (plan.seconds == 0 && !plan.radio_line) return false;

  (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (plan.seconds != 0) {
    if (esp_sleep_enable_timer_wakeup(static_cast<std::uint64_t>(plan.seconds) * 1000000ull) !=
        ESP_OK) {
      return false;
    }
  }

  if (plan.radio_line) {
    // The order here is the one thing on this page that a scope would catch getting wrong.
    //
    // 1. The pull-up, through the **RTC** IO mux and not the digital one. `ext1_wakeup_prepare()`
    //    routes the pad to the RTC function before it sleeps
    //    (`rtcio_hal_function_select(rtc_pin, RTCIO_LL_FUNC_RTC)`,
    //    components/esp_hw_support/sleep_modes.c at v5.5.5), so the pull-up that is in force
    //    during the sleep is the RTC pad's. `gpio_pullup_en()` would write the other one and the
    //    line would float: a radio that is off, resetting or being flashed would then hold the
    //    Walter awake, which is exactly what the pull-up is for. The pull-down is disabled for
    //    the same reason - a pull-down on a line whose wake level is *low* is a wake every time.
    // 2. **No hold of ours on this pad.** `ext1_wakeup_prepare()` enables the RTCIO hold itself
    //    when the RTC peripheral domain is powered down, which is the default; a hold put on
    //    first would block its own `rtcio_hal_input_enable()` and the function select - the
    //    documentation is explicit that `rtc_gpio_hold_dis` comes before "any pin
    //    re-configuration" (sleep_modes.rst, *External Wakeup (ext1)*). So
    //    `RadioPins::holdForSleep(true)` leaves this one pad out and the chip holds it.
    // 3. **ext1 and not ext0.** ext0 "will work only if RTC peripherals are kept on during
    //    sleep" (esp_sleep.h), and keeping that domain powered for one input pin is current this
    //    unit does not have to spend; ext1 "supports wakeup even when the RTC peripheral is
    //    powered down" and uses the pad's hold to keep the pull-up (sleep_modes.rst). `ANY_LOW`
    //    because the radio's TX idles high and a start bit is the falling edge of it.
    if (rtc_gpio_pullup_en(kWakePin) != ESP_OK) return false;
    if (rtc_gpio_pulldown_dis(kWakePin) != ESP_OK) return false;
    if (esp_sleep_enable_ext1_wakeup_io(1ull << static_cast<int>(kWakePin),
                                        ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
      return false;
    }
  }

  // The modem is down and the radio has been told by now (core/cycle/sleep.h's order), and the
  // pads are held. `esp_deep_sleep_start()` flushes the UART FIFOs itself ("Before entering
  // sleep mode, esp_deep_sleep_start will flush the contents of UART FIFOs", sleep_modes.rst,
  // *UART Output Handling*) and forces the flash down. It does not return.
  esp_deep_sleep_start();
  return false;  // not reached on a board
}

}  // namespace platform
