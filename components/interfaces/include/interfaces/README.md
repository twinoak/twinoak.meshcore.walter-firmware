Interfaces only — pure virtual classes core/ is written against and host_test/ fakes. No ESP-IDF
include. Included as `"interfaces/<name>.h"`.

| Header | What core/ asks of the world | Platform (ESP-IDF) | Host test |
|---|---|---|---|
| `pins.h` | a pin's mode (input, input with pull-up, output, open-drain), write, read, pads held through deep sleep | the GPIO driver (`platform/pins`) | `host_test/fakes/fake_pins` — records every transition with its time and checks the rules of `docs/ARCHITECTURE.md` §2 |
| `clock.h` | milliseconds since boot, a delay in ms, a busy delay in µs | `esp_timer`, `vTaskDelay`, `esp_rom_delay_us` (`platform/clock`) | `fake_clock` — delays advance the clock instantly, with a hook for tests that need to act mid-delay |
| `retained.h` | small records that survive a reset and a deep sleep, not a power cut | RTC memory with a CRC-32 (`platform/retained`) | `fake_retained` — a map, with a power cut |
| `text_out.h` | bytes to the console | the USB serial port (`platform/usb_console`) | `capture_out` — a string |
| `restart.h` | restart the board | `esp_restart()` (`platform/boot`) | `fake_restart` — records that it was asked |
| `byte_stream.h` | the UART to the radio: open and close (the pins are RadioPins'), read what is there, write, flush, "has the line been idle" | UART1 at 115200 8N1 on GPIO7/8 (`platform/uart`) | `host_test/sim/scripted_radio` — a radio driven by the link's fixtures |
| `wall_clock.h` | the time of day and where it came from, or "I have no time to give" | nothing yet: phase 3's modem reads it from the network (`main/main.cpp` answers false) | `fake_wall_clock` — a time a test hands over |
| `flash_region.h` | a region of the Walter's own flash: read, write, erase by sector, the sector size — and the shape of NOR flash, which a write only clears bits of | the `rakfw` partition of `partitions.csv` (`platform/flash_partition`) | `host_test/sim/sim_flash` — only clears bits, counts erases per sector, and can lose power at the Nth operation |
| `own_update.h` | the Walter's two application slots: which runs and in what state, write an image into the other, make it the next to boot, confirm or reject the running one (task p2-04) | ESP-IDF's OTA machinery over `ota_0`/`ota_1` with rollback enabled (`platform/own_update`) | `host_test/fakes/fake_own_update` — two slots and the bootloader's rules at a reboot (pending → aborted, new → pending), with a power cut at the Nth operation |

| `uplink.h` | the way out of the unit: the steps of getting onto the LTE-M network - is the modem there, the SIM, the RAT, the context, the radio on, attach, registration, packet data - and the four things worth asking once it is up (the time and where it came from, the signal, the identity, what power saving the network granted), each taking a deadline. The order, the deadlines, the wake's budget and the backoff are **not** here: they are `core/uplink/policy.h` | `walter-modem` on UART2 (`modem/walter_uplink`) | `host_test/sim/fake_modem` — a modem that is silent, has no card, a locked card, never registers, registers and drops, gives no time or gives 1970, and that costs its caller time for every call |

Phase 3 adds the key-value store and the sleeper; the uplink is task p3-01's, above. (The
self-update's uplink condition is `core/update/uplink_check.h`, a seam of core's own that phase 3's
HTTPS client fills.)
