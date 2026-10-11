ESP-IDF implementations of the interfaces in `components/interfaces/` (what `docs/ARCHITECTURE.md`
calls `hal/`). Thin: a pin is a GPIO call, a byte stream is a UART driver. No decision is taken
here; a decision belongs in `core/` where a host test can make it go wrong.

| File | Interface | On |
|---|---|---|
| `pins.cpp` | `interfaces::Pins` | `gpio_config`, `gpio_set_level`, `gpio_get_level`, `gpio_hold_en/dis`, `gpio_deep_sleep_hold_en/dis` |
| `clock.cpp` | `interfaces::Clock` | `esp_timer_get_time`, `vTaskDelay`, `esp_rom_delay_us` |
| `retained.cpp` | `interfaces::Retained` | `RTC_NOINIT_ATTR` slots with a CRC-32 (`esp_rom_crc32_le`) |
| `boot.cpp` | `interfaces::Restart`, the reset reason and wake cause | `esp_reset_reason`, `esp_sleep_get_wakeup_cause`, `esp_restart`; `static_assert`s hold `core/health`'s copied codes to the real enumerators |
| `usb_console.cpp` | `interfaces::TextOut` and a byte in | the USB Serial/JTAG driver and its VFS |

Nothing outside this directory calls a `gpio_*` function (`tools/verify.sh` greps for it).
