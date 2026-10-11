# modem/ - the Walter's LTE modem behind interfaces/' uplink (docs/ARCHITECTURE.md §1, §9). The library,
# dptechnics/walter-modem, is a managed component this component's idf_component.yml names at
# exactly one version; the component manager adds it to this component's requirements itself.
# Today there is no uplink yet: only the anchor that keeps the library in the link (task p0-01).
idf_component_register(
  SRCS "modem_anchor.cpp"
  REQUIRES interfaces)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})
