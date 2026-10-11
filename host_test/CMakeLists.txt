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
target_include_directories(core PUBLIC ${CORE}/include)
twinoak_ours(core)
# The version the firmware gets from PROJECT_VER; here from the cache so the test can pin it.
set(TWINOAK_VERSION "0.0.0-dev" CACHE STRING "what core::version() returns in this build")
target_compile_definitions(core PRIVATE TWINOAK_VERSION="${TWINOAK_VERSION}")

add_custom_target(core_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CORE} -P ${REPO}/tools/cmake/no_idf_include.cmake
  COMMENT "core has no ESP-IDF include"
  VERBATIM)
add_dependencies(core core_no_idf_include)

# -- the tests ----------------------------------------------------------------------------------
# doctest v2.5.3, vendored (third_party/doctest/README.md). -isystem: its warnings are its own.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/third_party/doctest)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/core/*.cpp)
add_executable(core_tests main.cpp ${TEST_SOURCES})
target_link_libraries(core_tests PRIVATE core doctest)
twinoak_ours(core_tests)
target_compile_definitions(core_tests PRIVATE TWINOAK_VERSION="${TWINOAK_VERSION}")

enable_testing()
add_test(NAME core_tests COMMAND core_tests --no-intro)
set_tests_properties(core_tests PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
