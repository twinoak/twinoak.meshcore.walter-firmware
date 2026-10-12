# platform/ - the ESP-IDF implementations of interfaces/ (the architecture's hal/): thin, no
# logic, "kept so thin that reading it is the test" (docs/ARCHITECTURE.md §1). Task p0-03: the
# pins on the GPIO driver, the clock, retained storage in RTC memory, the boot reason, the USB
# console's bytes. Task p1-02: the UART to the radio (uart.cpp). Task p2-02: the `rakfw`
# partition behind interfaces::FlashRegion (flash_partition.cpp).
idf_component_register(
  SRCS "pins.cpp" "clock.cpp" "retained.cpp" "boot.cpp" "usb_console.cpp" "uart.cpp"
       "flash_partition.cpp"
  INCLUDE_DIRS "include"
  REQUIRES interfaces core esp_driver_gpio esp_driver_uart esp_timer
           esp_driver_usb_serial_jtag vfs esp_system esp_hw_support esp_rom soc
           esp_partition)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
