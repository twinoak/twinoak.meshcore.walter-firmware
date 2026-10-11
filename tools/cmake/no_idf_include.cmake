# components/core/ has no ESP-IDF include (docs/ARCHITECTURE.md §1): the logic is tested on the
# host, so nothing in it may reach for FreeRTOS, a driver, the SoC or the sdkconfig. This script
# reads every source and header under CORE_DIR and fails with a sentence on the first include that
# names one. It runs as a build step before core/ is compiled - by the firmware build
# (components/core/CMakeLists.txt) and by host_test/ - so a file edited after configure is still
# read. Run by hand:  cmake -DCORE_DIR=components/core -P tools/cmake/no_idf_include.cmake
#
# The list is the ESP-IDF tree's own prefixes plus the two neighbours core/ must not know about.
# A new ESP-IDF include that is not caught here is added to the list, never to core/.
if(NOT DEFINED CORE_DIR)
  message(FATAL_ERROR "no_idf_include.cmake: CORE_DIR is not set")
endif()

set(_forbidden
  "esp_" "esp32" "esp-idf" "freertos/" "driver/" "soc/" "hal/" "rom/" "xtensa" "riscv/"
  "sdkconfig" "nvs" "spi_flash" "bootloader" "lwip/" "mbedtls/" "esp_log" "Arduino" "fatfs"
  "WalterModem" "platform/" "modem/")

file(GLOB_RECURSE _files
  "${CORE_DIR}/*.cpp" "${CORE_DIR}/*.h" "${CORE_DIR}/*.hpp" "${CORE_DIR}/*.c" "${CORE_DIR}/*.inc")

foreach(_file IN LISTS _files)
  file(STRINGS "${_file}" _includes REGEX "^[ \t]*#[ \t]*include")
  foreach(_line IN LISTS _includes)
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]*)[>\"].*$" "\\1" _target "${_line}")
    foreach(_prefix IN LISTS _forbidden)
      string(FIND "${_target}" "${_prefix}" _at)
      if(_at EQUAL 0)
        file(RELATIVE_PATH _rel "${CORE_DIR}/.." "${_file}")
        message(FATAL_ERROR
          "core has no ESP-IDF include: ${_rel} includes <${_target}>. Logic in components/core/ "
          "is compiled on the host and tested there; what it needs of the board it takes as an "
          "interface from components/interfaces/ (docs/ARCHITECTURE.md §1).")
      endif()
    endforeach()
  endforeach()
endforeach()
list(LENGTH _files _n)
message(STATUS "core: ${_n} files, no ESP-IDF include")
