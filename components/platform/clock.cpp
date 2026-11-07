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

// **delayMs blocks, always, and spins never** (task p9-01). It did spin, below one tick: at the
// default CONFIG_FREERTOS_HZ of 100 a tick is 10 ms, pdMS_TO_TICKS(1) is 0, and this function
// called esp_rom_delay_us(1000) instead - shorter and exact, and the CPU held throughout. On
// 7 November 2026 agent-core's JobRunner::idle, which waits out the airtime budget with
// delayMs(1) in a loop, spun that way for about 40 s inside one 34-line config.read and the 30 s
// task watchdog reset the first unit (tasks/phase-8/FOLLOWUPS.md, *A busy wait below one tick
// still starves the watchdog*; p8-01 had fixed the same fault in the flasher's loop, not here).
//
// So sdkconfig.defaults sets CONFIG_FREERTOS_HZ=1000 and a millisecond is a tick. This assertion
// is what holds that together: lower the tick rate and the build stops here rather than quietly
// start spinning again.
static_assert(configTICK_RATE_HZ >= 1000,
              "delayMs must never busy-wait, so a millisecond has to be at least one FreeRTOS "
              "tick: CONFIG_FREERTOS_HZ must stay at 1000 (sdkconfig.defaults, task p9-01)");

// vTaskDelay(n) blocks for between n-1 and n ticks, so one tick is added: the delay is never
// shorter than asked (the reset pulse must be its 100 ms, the spacing its second). Zero is
// nothing asked for and nothing given - no caller in this firmware or in agent-core passes it.
void EspClock::delayMs(std::uint32_t ms) {
  if (ms == 0) return;
  vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

// The one busy wait left, and the only one the interface promises (interfaces/clock.h): the SWD
// wire's half-bits (4 us at 125 kHz, core/swd/wire.cpp) and the short UART turnarounds are far
// below a tick and must be exact. None of them is long, and core/swd/idle_pause.h is what bounds
// the loops around them.
void EspClock::delayUs(std::uint32_t us) { esp_rom_delay_us(us); }

}  // namespace platform
