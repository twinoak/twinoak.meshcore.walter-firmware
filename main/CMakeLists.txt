# main/ - app_main: construct the real things, hand them to the core, run (docs/ARCHITECTURE.md §1).
# Task p0-03: the pins to the safe state first, the boot's reason recorded, the console on USB.
# Task p4-02: `agent` is this firmware's own half of the library's seams (LinkMesh over the link),
# constructed here and handed to the console. Task p4-05: the MeshMaster agent is behind the
# backend seam - `agent-core`, the copied library of docs/ARCHITECTURE.md §13, is called by
# `agent::AgentBackend` and needs no anchor to stay in the image. WALTER_AGENT (the top-level
# CMakeLists.txt's option, on by default) is the build flag: off, main.cpp constructs the null
# backend of phases 0-3 and nothing of the agent, which is the second host configuration
# tools/verify.sh runs the wake cycle's tests in.
# The agent components are required unconditionally, not under `if(WALTER_AGENT)`: ESP-IDF expands a
# component's REQUIRES in an early script-mode pass that does not see the root CMakeLists.txt's
# option(), so a guarded requirement would be dropped and main.cpp's `#include <agent/AgentLoop.h>`
# (under `#if WALTER_AGENT`, which the compile definition below does switch) would fail to resolve.
# When WALTER_AGENT is off main.cpp names nothing of them, and the linker's `--gc-sections` takes
# the unreferenced copied library back out of the image - so "no agent in the link" still holds.
idf_component_register(
  SRCS "main.cpp"
  REQUIRES core platform modem app_update agent-core agent)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
target_compile_definitions(${COMPONENT_LIB} PRIVATE WALTER_AGENT=$<BOOL:${WALTER_AGENT}>)
