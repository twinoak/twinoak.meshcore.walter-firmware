# main/ - app_main: construct the real things, hand them to the core, run (docs/ARCHITECTURE.md §1).
# Today: print the banner, say which slot is running and in what state, and idle.
idf_component_register(
  SRCS "main.cpp"
  REQUIRES core modem app_update)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
