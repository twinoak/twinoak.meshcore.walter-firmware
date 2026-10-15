# host_test/ - a CMake project of its own: components/core/ + fakes + simulators + tests, built with
# the container's host compiler under AddressSanitizer and UndefinedBehaviorSanitizer and run by
# ctest (docs/ARCHITECTURE.md §1, tasks/README.md). tools/verify.sh runs it as step 3:
#
#   cmake -S host_test -B <build> -G Ninja && cmake --build <build> && ctest --test-dir <build>
#
# core/ is compiled from the same files the firmware compiles, with the same flags
# (tools/cmake/ours.cmake), after the same check that none of them includes ESP-IDF.
cmake_minimum_required(VERSION 3.22)
# C as well as C++: the vendored TweetNaCl extract (task p2-01) is a C file, compiled here as the
# firmware compiles it.
project(twinoak_walter_host_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Optimised, with debug information (task p1-05): the flasher's power-loss loop drives a bit-level
# simulation of the SWD wire through hundreds of flashes, and at -O0 that is ten times slower
# than at -O2 for no gain in what the tests prove. The sanitizers below are as thorough either way.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
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
find_program(PYTHON3 NAMES python3 python REQUIRED)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CORE}/*.cpp ${CORE}/*.c)

# The trust table (task p2-01): keys/release-keys.txt -> trust_keys.h, exactly as
# components/core/CMakeLists.txt makes it for the firmware. The host build's table is the
# repository's (empty today), so the tests that use the build's own table see what a unit sees;
# tests that need the test key make tables of their own. TWINOAK_TRUST_TEST_KEY=ON builds the
# +TESTKEY variant of core for a look, as the firmware's option does.
option(TWINOAK_TRUST_TEST_KEY "trust the specification's public test key (a test build)" OFF)
set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TRUST_HEADER ${GENERATED}/trust_keys.h)
set(TRUST_COMMAND ${PYTHON3} ${REPO}/tools/gen_trust_table.py ${REPO}/keys/release-keys.txt ${TRUST_HEADER})
set(TRUST_DEPENDS ${REPO}/tools/gen_trust_table.py ${REPO}/keys/release-keys.txt
                  ${REPO}/tools/release/verify_release.py ${REPO}/spec/tools/manifestref.py)
if(TWINOAK_TRUST_TEST_KEY)
  list(APPEND TRUST_COMMAND --test-key ${REPO}/spec/fixtures/manifest/index.json)
  list(APPEND TRUST_DEPENDS ${REPO}/spec/fixtures/manifest/index.json)
endif()
add_custom_command(
  OUTPUT ${TRUST_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED}
  COMMAND ${TRUST_COMMAND}
  DEPENDS ${TRUST_DEPENDS}
  COMMENT "the trust table: keys/release-keys.txt -> trust_keys.h"
  VERBATIM)

# The TLS roots (task p3-02): keys/tls/*.pem -> tls_roots.h, exactly as components/core/
# CMakeLists.txt makes it for the firmware; the generator holds each PEM's header to the
# certificate and refuses a mismatch, so a wrong fingerprint in keys/tls/ is a failed build here
# as well as there.
set(TLS_HEADER ${GENERATED}/tls_roots.h)
file(GLOB TLS_PEMS ${REPO}/keys/tls/*.pem)
add_custom_command(
  OUTPUT ${TLS_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED}
  COMMAND ${PYTHON3} ${REPO}/tools/gen_tls_roots.py ${REPO}/keys/tls ${TLS_HEADER}
  DEPENDS ${REPO}/tools/gen_tls_roots.py ${TLS_PEMS}
  COMMENT "the TLS roots: keys/tls/*.pem -> tls_roots.h"
  VERBATIM)

add_library(core STATIC ${CORE_SOURCES} ${TRUST_HEADER} ${TLS_HEADER})
target_include_directories(core PUBLIC ${CORE}/include ${INTERFACES}/include)
target_include_directories(core PRIVATE ${GENERATED})
twinoak_ours(core)
if(TWINOAK_TRUST_TEST_KEY)
  target_compile_definitions(core PRIVATE TWINOAK_TRUST_TEST_KEY=1)
endif()
# The version the firmware gets from PROJECT_VER; here from the cache so the test can pin it.
set(TWINOAK_VERSION "0.0.0-dev" CACHE STRING "what core::version() returns in this build")
target_compile_definitions(core PRIVATE TWINOAK_VERSION="${TWINOAK_VERSION}")

# The vendored third-party code under core/third_party/ (task p2-01; each directory's README.md
# has the origin, version, licence and hash). A system include path, as doctest's is, so that a
# header's warnings are its own. The one .c file - the TweetNaCl verification extract - is held
# to our flags except -Wno-sign-compare, which its loops need, and -fno-sanitize=shift-base,
# because car25519 and modL left-shift negative values: defined on every compiler this project
# uses, undefined to UBSan (third_party/tweetnacl/README.md). Everything else of ASan and UBSan
# applies to it.
target_include_directories(core SYSTEM PRIVATE ${CORE}/third_party)
set_source_files_properties(${CORE}/third_party/tweetnacl/tweetnacl_verify.c
  PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare;-fno-sanitize=shift-base")

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
set(FIXTURE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/descriptor_fixtures.h)
add_custom_command(
  OUTPUT ${FIXTURE_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND ${PYTHON3} ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_descriptor_fixtures.py
          ${SPEC_FIXTURES}/index.json ${FIXTURE_HEADER}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_descriptor_fixtures.py ${SPEC_FIXTURES}/index.json
  COMMENT "the spec's descriptor fixtures -> descriptor_fixtures.h"
  VERBATIM)

# And the manifest fixtures (task p2-01): index.json's test key, RFC vector, signature cases and
# file cases as a table, plus the signed variants the verdict tests need - a manifest with its
# keys reordered, one with an unknown product, one whose file is the two-descriptor image ... -
# each made from the spec's fixtures and signed with the test key by spec/tools/manifestref.py,
# the reference, into generated/manifest/. Needs the `cryptography` the image has
# (tools/requirements-verify.txt).
set(MANIFEST_FIXTURE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/manifest_fixtures.h)
set(MANIFEST_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/manifest)
add_custom_command(
  OUTPUT ${MANIFEST_FIXTURE_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MANIFEST_FIXTURE_DIR}
  COMMAND ${PYTHON3} ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_manifest_fixtures.py
          ${SPEC_FIXTURES} ${MANIFEST_FIXTURE_DIR} ${MANIFEST_FIXTURE_HEADER}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fixtures/gen_manifest_fixtures.py ${SPEC_FIXTURES}/index.json
          ${REPO}/spec/tools/manifestref.py
  COMMENT "the spec's manifest fixtures and signed variants -> manifest_fixtures.h"
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
# core/ is the tests of the logic; rails/ is the suite's own statement about the rails
# (task p3-06): the assertions that are about every test rather than about one, and which doctest
# runs last because it orders test cases by file path.
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/core/*.cpp
                                                 ${CMAKE_CURRENT_LIST_DIR}/rails/*.cpp)
add_executable(core_tests main.cpp ${FAKE_SOURCES} ${SIM_SOURCES} ${TEST_SOURCES} ${FIXTURE_HEADER}
               ${MANIFEST_FIXTURE_HEADER})
target_link_libraries(core_tests PRIVATE core doctest)
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_LIST_DIR})
# The vendored TweetNaCl extract's entry points, for the tests that hold our wrappers against it.
target_include_directories(core_tests SYSTEM PRIVATE ${CORE}/third_party)
twinoak_ours(core_tests)
target_compile_definitions(core_tests PRIVATE
  TWINOAK_VERSION="${TWINOAK_VERSION}"
  TWINOAK_SPEC_FIXTURES="${SPEC_FIXTURES}"
  TWINOAK_REPO_ROOT="${REPO}")

# -- the console behind a pipe (task p2-02) -----------------------------------------------------
# The firmware's own console and image store, over a simulated flash, on stdin and stdout, so that
# tools/walterctl.py - the laptop's end of `image receive` - is tested before a board exists. It is
# not a fake of the firmware: it is core/'s console with sim/sim_flash.h under it and the
# specification's public test key in its trust table (host_test/console_host/main.cpp).
add_executable(walter_console console_host/main.cpp ${MANIFEST_FIXTURE_HEADER})
target_link_libraries(walter_console PRIVATE core)
target_include_directories(walter_console PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
                           ${CMAKE_CURRENT_LIST_DIR})
twinoak_ours(walter_console)

enable_testing()
add_test(NAME core_tests COMMAND core_tests --no-intro)
set_tests_properties(core_tests PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")

# walterctl pushing the specification's fixture release into that console, and the refusals it and
# the Walter answer with (tools/test_walterctl.py).
add_test(NAME walterctl COMMAND ${PYTHON3} ${REPO}/tools/test_walterctl.py
         --console $<TARGET_FILE:walter_console> --generated ${MANIFEST_FIXTURE_DIR})
set_tests_properties(walterctl PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
