#include "platform/clock.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace platform {

// esp_timer_get_time(): microseconds since boot, 64-bit, monotonic - and, per
// docs/en/api-reference/system/esp_timer.rst (v5.5.5), restarting from zero after a deep sleep
// (interfaces/clock.h says what that means for the code above this).
std::uint64_t EspClock::nowMs() { return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000u; }

// vTaskDelay(n) blocks for between n-1 and n ticks, so one tick is added: the delay is never
// shorter than asked (the reset pulse must be its 100 ms, the spacing its second). Below one
// tick (10 ms at the default CONFIG_FREERTOS_HZ=100) a busy wait is both shorter and exact.
void EspClock::delayMs(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  if (ticks == 0) {
    esp_rom_delay_us(ms * 1000u);
    return;
  }
  vTaskDelay(ticks + 1);
}

void EspClock::delayUs(std::uint32_t us) { esp_rom_delay_us(us); }

}  // namespace platform
