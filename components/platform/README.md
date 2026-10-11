ESP-IDF implementations of the interfaces in `components/interfaces/` (what `docs/ARCHITECTURE.md`
calls `hal/`). Thin: a pin is a GPIO call, a byte stream is a UART driver. No decision is taken
here; a decision belongs in `core/` where a host test can make it go wrong. Empty until phase 0's
task 03.
