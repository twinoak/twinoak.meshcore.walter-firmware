# main/ - app_main: construct the real things, hand them to the core, run (docs/ARCHITECTURE.md §1).
# Task p0-03: the pins to the safe state first, the boot's reason recorded, the console on USB.
# Task p4-01: agent_anchor.cpp is the one file of this firmware that includes a header of
# `agent-core`, the copied library of docs/ARCHITECTURE.md §13 - it keeps the library in the image
# while nothing calls it, and `agent-core` is in REQUIRES for that one file's sake. Task p4-02:
# `agent` is this firmware's own half of the library's seams (LinkMesh over the link), constructed
# here and handed to the console.
idf_component_register(
  SRCS "main.cpp" "agent_anchor.cpp"
  REQUIRES core platform modem app_update agent-core agent)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
