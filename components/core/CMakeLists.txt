# core/ - everything that is logic. No ESP-IDF include, no FreeRTOS, no heap after start-up
# (docs/ARCHITECTURE.md §1). What it needs of the world it takes as an interface from interfaces/
# (the architecture's hal/). The same sources are compiled on the host by host_test/.
idf_component_register(
  SRCS "version/version.cpp"
       "image/this_image.cpp"
       "board/radio_pins.cpp"
       "health/health.cpp"
       "console/parser.cpp"
       "console/console.cpp"
       "link/codec/cobs.cpp"
       "link/codec/packet.cpp"
       "link/codec/frame.cpp"
       "link/codec/messages.cpp"
       "link/client/port.cpp"
       "link/client/client.cpp"
       "link/client/words.cpp"
       "swd/wire.cpp"
       "swd/dp.cpp"
       "swd/mem_ap.cpp"
       "nrf52/nrf52.cpp"
  INCLUDE_DIRS "include"
  REQUIRES interfaces)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})

# The product version reaches core/ as a string literal (include/core/version.h).
idf_build_get_property(_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TWINOAK_VERSION="${_ver}")

# Before core/ compiles, every one of its files is read for an ESP-IDF include
# (tools/cmake/no_idf_include.cmake); one is a failed build with a sentence.
add_custom_target(core_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_LIST_DIR} -P
          ${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/no_idf_include.cmake
  COMMENT "core has no ESP-IDF include"
  VERBATIM)
add_dependencies(${COMPONENT_LIB} core_no_idf_include)
