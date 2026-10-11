# host_test/ - a CMake project of its own: components/core/ + fakes + simulators + tests, built with
# the container's host compiler under AddressSanitizer and UndefinedBehaviorSanitizer and run by
# ctest (docs/ARCHITECTURE.md §1, tasks/README.md). tools/verify.sh runs it as step 3:
#
#   cmake -S host_test -B <build> -G Ninja && cmake --build <build> && ctest --test-dir <build>
#
# core/ is compiled from the same files the firmware compiles, with the same flags
# (tools/cmake/ours.cmake), after the same check that none of them includes ESP-IDF.
cmake_minimum_required(VERSION 3.22)
project(twinoak_walter_host_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)
set(CORE ${REPO}/components/core)
set(INTERFACES ${REPO}/components/interfaces)
include(${REPO}/tools/cmake/ours.cmake)

# The sanitizers, for everything in this project: the tests exist to make things go wrong, and an
# out-of-bounds read or a signed overflow that happens to work is one of those things.
# -fno-sanitize-recover makes a UBSan finding a failed test, not a log line.
set(SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
add_compile_options(${SANITIZE})
add_link_options(${SANITIZE})

# -- core/, as the firmware compiles it --------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CORE}/*.cpp)
add_library(core STATIC ${CORE_SOURCES})
target_include_directories(core PUBLIC ${CORE}/include ${INTERFACES}/include)
twinoak_ours(core)
# The version the firmware gets from PROJECT_VER; here from the cache so the test can pin it.
set(TWINOAK_VERSION "0.0.0-dev" CACHE STRING "what core::version() returns in this build")
target_compile_definitions(core PRIVATE TWINOAK_VERSION="${TWINOAK_VERSION}")

add_custom_target(core_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CORE} -P ${REPO}/tools/cmake/no_idf_include.cmake
  COMMENT "core has no ESP-IDF include"
  VERBATIM)
add_dependencies(core core_no_idf_include)

# -- the spec's fixtures, as a table ------------------------------------------------------------
# spec/fixtures/manifest/index.json's `descriptors`, turned into C++ by
# fixtures/gen_descriptor_fixtures.py at build time (task p0-02): the tests walk the spec's own
# cases instead of a transcription of them, and the image files beside it are read at run time
# from TWINOAK_SPEC_FIXTURES. Regenerated whenever index.json or the generator changes.
set(SPEC_FIXTURES ${REPO}/spec/fixtures/manifest)
find_program(PYTHON3 NAMES python3 python REQUIRED)
set(FIXTURE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/descriptor_fixtures.h)
add_custom_command(
  OUTPUT ${FIXTURE_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND ${PYTHON3} ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_descriptor_fixtures.py
          ${SPEC_FIXTURES}/index.json ${FIXTURE_HEADER}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_descriptor_fixtures.py ${SPEC_FIXTURES}/index.json
  COMMENT "the spec's descriptor fixtures -> descriptor_fixtures.h"
  VERBATIM)

# -- the tests ----------------------------------------------------------------------------------
# doctest v2.5.3, vendored (third_party/doctest/README.md). -isystem: its warnings are its own.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/third_party/doctest)

# The fakes (fakes/): what stands behind the interfaces on the host. The fake pins check the pin
# rules themselves and fail the running test through doctest, so they are part of the test binary.
# The simulators (sim/) are the other half of a test: the scripted radio (task p1-02), and from
# phase 1's task 03 the SWD target.
file(GLOB_RECURSE FAKE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fakes/*.cpp)
file(GLOB_RECURSE SIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/sim/*.cpp)
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/core/*.cpp)
add_executable(core_tests main.cpp ${FAKE_SOURCES} ${SIM_SOURCES} ${TEST_SOURCES} ${FIXTURE_HEADER})
target_link_libraries(core_tests PRIVATE core doctest)
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_LIST_DIR})
twinoak_ours(core_tests)
target_compile_definitions(core_tests PRIVATE
  TWINOAK_VERSION="${TWINOAK_VERSION}"
  TWINOAK_SPEC_FIXTURES="${SPEC_FIXTURES}"
  TWINOAK_REPO_ROOT="${REPO}")

enable_testing()
add_test(NAME core_tests COMMAND core_tests --no-intro)
set_tests_properties(core_tests PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
