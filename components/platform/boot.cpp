#include "platform/boot.h"

#include <cstdio>

#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace platform {

// core/health copied ESP-IDF's codes so that the mapping could be tested on the host; this is
// where the copies are held against the real enumerators (esp_system.h, esp_sleep.h, v5.5.5).
namespace codes = core::health::esp_codes;
static_assert(codes::kRstUnknown == ESP_RST_UNKNOWN, "esp_reset_reason_t changed");
static_assert(codes::kRstPowerOn == ESP_RST_POWERON, "esp_reset_reason_t changed");
static_assert(codes::kRstExt == ESP_RST_EXT, "esp_reset_reason_t changed");
static_assert(codes::kRstSw == ESP_RST_SW, "esp_reset_reason_t changed");
static_assert(codes::kRstPanic == ESP_RST_PANIC, "esp_reset_reason_t changed");
static_assert(codes::kRstIntWdt == ESP_RST_INT_WDT, "esp_reset_reason_t changed");
static_assert(codes::kRstTaskWdt == ESP_RST_TASK_WDT, "esp_reset_reason_t changed");
static_assert(codes::kRstWdt == ESP_RST_WDT, "esp_reset_reason_t changed");
static_assert(codes::kRstDeepSleep == ESP_RST_DEEPSLEEP, "esp_reset_reason_t changed");
static_assert(codes::kRstBrownout == ESP_RST_BROWNOUT, "esp_reset_reason_t changed");
static_assert(codes::kRstSdio == ESP_RST_SDIO, "esp_reset_reason_t changed");
static_assert(codes::kRstUsb == ESP_RST_USB, "esp_reset_reason_t changed");
static_assert(codes::kRstJtag == ESP_RST_JTAG, "esp_reset_reason_t changed");
static_assert(codes::kRstEfuse == ESP_RST_EFUSE, "esp_reset_reason_t changed");
static_assert(codes::kRstPwrGlitch == ESP_RST_PWR_GLITCH, "esp_reset_reason_t changed");
static_assert(codes::kRstCpuLockup == ESP_RST_CPU_LOCKUP, "esp_reset_reason_t changed");
static_assert(codes::kWakeUndefined == ESP_SLEEP_WAKEUP_UNDEFINED, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeAll == ESP_SLEEP_WAKEUP_ALL, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeExt0 == ESP_SLEEP_WAKEUP_EXT0, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeExt1 == ESP_SLEEP_WAKEUP_EXT1, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeTimer == ESP_SLEEP_WAKEUP_TIMER, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeTouchpad == ESP_SLEEP_WAKEUP_TOUCHPAD, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeUlp == ESP_SLEEP_WAKEUP_ULP, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeGpio == ESP_SLEEP_WAKEUP_GPIO, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeUart == ESP_SLEEP_WAKEUP_UART, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeUart1 == ESP_SLEEP_WAKEUP_UART1, "esp_sleep_wakeup_cause_t changed");
static_assert(codes::kWakeUart2 == ESP_SLEEP_WAKEUP_UART2, "esp_sleep_wakeup_cause_t changed");

core::health::ResetReason resetReason() {
  return core::health::fromEspResetReason(static_cast<int>(esp_reset_reason()));
}

core::health::WakeCause wakeCause() {
  return core::health::fromEspWakeCause(static_cast<int>(esp_sleep_get_wakeup_cause()));
}

void EspRestart::restart() {
  std::fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(100));  // let the port drain "rebooting"
  esp_restart();
}

}  // namespace platform
