// A blocking pause between two SWD transfers, so that the idle task of this core runs (task
// p8-01).
//
// Nothing on the SWD path blocks. The wire's half-bit waits are `delayUs`, which on the board is
// `esp_rom_delay_us`; and `EspClock::delayMs(1)` - what the NVMC's READY poll and the DHCSR poll
// wait - is a busy wait too, because below one tick (10 ms at `CONFIG_FREERTOS_HZ=100`)
// `platform/clock.cpp` busy-waits rather than call `vTaskDelay`. A whole radio image is minutes
// of that: the idle task of the core never runs, and the task watchdog (`sdkconfig.defaults`:
// `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`, panic on, the idle tasks of both cores subscribed) resets
// the Walter. That is what happened to the first unit on 6 November 2026 - twelve watchdog boots
// and a radio left with half an application (`tasks/phase-6/FOLLOWUPS.md`, *The SWD flasher
// starves the task watchdog*).
//
// So every loop of this firmware that drives SWD for more than a second calls `letIdleTaskRun()`
// between one transfer's last bit and the next one's first. The watchdog itself is never fed from
// the flasher: a firmware that really hangs must still be reset, and the idle task running is
// exactly the thing the watchdog is there to notice the absence of (ESP-IDF v5.5,
// `api-reference/system/wdts.rst`: the idle tasks are what the task watchdog watches by default,
// and they reset it on behalf of each core).
//
// **Never inside a transfer.** The nRF52840 gives SWDCLK a minimum - Product Specification,
// "Debug and trace > Electrical specification > Control access port", f_SWDCLK min 0.125 MHz
// (`core/swd/wire.h` picks the default half-period from it) - so a transfer that is already under
// way may not be stretched. Between transfers there is no such rule: ADIv5 (IHI 0031G) B4.1.1
// lets the host stop the clock altogether once it has clocked "at least 8 idle cycles" after a
// transfer, which `Wire::transfer` always does before it releases the line. A pause there leaves
// the line idle and the clock low, exactly as releasing the pins after a command does.
#pragma once

#include <cstdint>

#include "interfaces/clock.h"

namespace core {
namespace swd {

// One FreeRTOS tick at `CONFIG_FREERTOS_HZ = 100` (`sdkconfig.defaults`). `delayMs` keeps its
// promise - never shorter than asked - so `EspClock` turns this into `vTaskDelay(2)`: 10 to 20 ms
// of real blocking, and the idle task needs one scheduling slot of it.
constexpr std::uint32_t kIdlePauseMs = 10;

inline void letIdleTaskRun(interfaces::Clock& clock) { clock.delayMs(kIdlePauseMs); }

}  // namespace swd
}  // namespace core
