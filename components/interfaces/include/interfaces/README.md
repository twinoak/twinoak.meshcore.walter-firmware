Interfaces only — pure virtual classes core/ is written against and host_test/ fakes. No ESP-IDF
include. Included as `"interfaces/<name>.h"`.

| Header | What core/ asks of the world | Platform (ESP-IDF) | Host test |
|---|---|---|---|
| `pins.h` | a pin's mode (input, input with pull-up, output, open-drain), write, read, pads held through deep sleep | the GPIO driver (`platform/pins`) | `host_test/fakes/fake_pins` — records every transition with its time and checks the rules of `docs/ARCHITECTURE.md` §2 |
| `clock.h` | milliseconds since boot, a delay in ms, a busy delay in µs | `esp_timer`, `vTaskDelay`, `esp_rom_delay_us` (`platform/clock`) | `fake_clock` — delays advance the clock instantly, with a hook for tests that need to act mid-delay |
| `retained.h` | small records that survive a reset and a deep sleep, not a power cut | RTC memory with a CRC-32 (`platform/retained`) | `fake_retained` — a map, with a power cut |
| `text_out.h` | bytes to the console | the USB serial port (`platform/usb_console`) | `capture_out` — a string |
| `restart.h` | restart the board | `esp_restart()` (`platform/boot`) | `fake_restart` — records that it was asked |

Phase 1 adds the byte stream (the UART), phase 3 the key-value store, the uplink and the sleeper.
