# main/ - app_main: construct the real things, hand them to the core, run (docs/ARCHITECTURE.md §1).
# Task p0-03: the pins to the safe state first, the boot's reason recorded, the console on USB.
idf_component_register(
  SRCS "main.cpp"
  REQUIRES core platform modem app_update)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
