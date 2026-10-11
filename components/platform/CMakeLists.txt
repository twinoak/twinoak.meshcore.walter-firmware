# platform/ - the ESP-IDF implementations of interfaces/ (the architecture's hal/): thin, no
# logic, "kept so thin that reading it is the test" (docs/ARCHITECTURE.md §1). Task p0-03: the
# pins on the GPIO driver, the clock, retained storage in RTC memory, the boot reason, the USB
# console's bytes.
idf_component_register(
  SRCS "pins.cpp" "clock.cpp" "retained.cpp" "boot.cpp" "usb_console.cpp"
  INCLUDE_DIRS "include"
  REQUIRES interfaces core esp_driver_gpio esp_timer esp_driver_usb_serial_jtag vfs esp_system
           esp_hw_support)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
