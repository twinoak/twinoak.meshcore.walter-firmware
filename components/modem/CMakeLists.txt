# modem/ - the Walter's LTE modem behind interfaces/' uplink (docs/ARCHITECTURE.md §1, §9). The library,
# dptechnics/walter-modem, is a managed component this component's idf_component.yml names at
# exactly one version; the component manager adds it to this component's requirements itself.
#
# This is the only component of this firmware that names a WalterModem symbol
# (tools/verify.sh checks it). It is thin: every method of include/modem/walter_uplink.h is the
# library calls and the mapping of their results, and nothing of it can be tested without a SIM
# and a tower. The sequence, the deadlines, the budget and the backoff are core/uplink/, which
# host_test/ runs against host_test/sim/fake_modem.h (task p3-01).
idf_component_register(
  SRCS "walter_uplink.cpp"
  INCLUDE_DIRS "include"
  REQUIRES interfaces core)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
