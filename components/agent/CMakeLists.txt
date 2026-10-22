# components/agent/ - the three seams of docs/ARCHITECTURE.md §13 as this firmware fills them, over
# components/core/ and the copied library in components/agent-core/. Task p4-02: `LinkMesh`, the
# library's `mm::IMesh` as one request of spec/LINK-PROTOCOL.md §6 per call, and the probe behind
# the console's `rak mesh status`. Tasks 03 and 04 add the uplink and the platform beside it.
#
# Logic, like core/: no ESP-IDF include (the same build step holds it), no heap (the host suite's
# no-allocation check reads every file of it), tested on the host against a simulated RAK
# (host_test/sim/sim_rak.h) and the library's own simulated repeater.
idf_component_register(
  SRCS "link_mesh.cpp"
       "mesh_probe.cpp"
  INCLUDE_DIRS "include"
  REQUIRES core agent-core)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})

# Before it compiles, every file of it is read for an ESP-IDF include, as core/ is
# (tools/cmake/no_idf_include.cmake); one is a failed build with a sentence.
add_custom_target(agent_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_LIST_DIR} -DNAME=agent -P
          ${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/no_idf_include.cmake
  COMMENT "agent has no ESP-IDF include"
  VERBATIM)
add_dependencies(${COMPONENT_LIB} agent_no_idf_include)
