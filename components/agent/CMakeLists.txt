# components/agent/ - the three seams of docs/ARCHITECTURE.md §13 as this firmware fills them, over
# components/core/ and the copied library in components/agent-core/. Task p4-02: `LinkMesh`, the
# library's `mm::IMesh` as one request of spec/LINK-PROTOCOL.md §6 per call, and the probe behind
# the console's `rak mesh status`. Task p4-03: `ModemUplink`, the library's `mm::IUplink` as the
# seven calls of the agent API over phase 3's HTTPS client, and the probe behind `agent url` and
# `agent probe`. Task p4-04: `WalterPlatform` and the `mm::IClock` behind it, the `Identity` the
# heartbeat is built from, the §7 drain into the library's observation ring, and the probe behind
# `agent id`, `agent heartbeat` and `agent obs`.
#
# Logic, like core/: no ESP-IDF include (the same build step holds it), no heap (the host suite's
# no-allocation check reads every file of it), tested on the host against a simulated RAK
# (host_test/sim/sim_rak.h), a scripted server behind the firmware's own HTTP/1.1
# (host_test/sim/fake_http.h) and the library's own simulated repeater.
idf_component_register(
  SRCS "agent_probe.cpp"
       "identity.cpp"
       "link_mesh.cpp"
       "mesh_probe.cpp"
       "modem_uplink.cpp"
       "observations.cpp"
       "uplink_probe.cpp"
       "walter_platform.cpp"
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
