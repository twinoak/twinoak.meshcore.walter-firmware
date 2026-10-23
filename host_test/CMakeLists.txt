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

# The MeshMaster agent behind the backend seam (task p4-05), the same option the firmware's
# top-level CMakeLists.txt has. ON is the unit as built: the copied library, components/agent/,
# the agent's own tests and the library's suites. OFF is phases 0-3's unit - the wake cycle over
# the null backend, nothing of the agent compiled - which tools/verify.sh builds as a second
# configuration and runs the wake cycle's tests in, so that the build without an agent stays a
# build. The flag reaches the test sources through one generated header rather than a compile
# definition, so that ccache keeps every object the flag does not change.
option(WALTER_AGENT "the MeshMaster agent behind the backend seam" ON)
if(WALTER_AGENT)
  set(WALTER_AGENT_VALUE 1)
else()
  set(WALTER_AGENT_VALUE 0)
endif()

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

# The build flag, as a header the few test sources that depend on it include
# (host_test/core/cycle/wake_bench.h): `#define WALTER_AGENT 0` or `1`.
file(CONFIGURE OUTPUT ${GENERATED}/walter_agent_config.h
     CONTENT "// Generated by host_test/CMakeLists.txt from the WALTER_AGENT option.\n#pragma once\n#define WALTER_AGENT @WALTER_AGENT_VALUE@\n"
     @ONLY)

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
# agent/ (task p4-02) is the tests of components/agent/ that need no simulated repeater: the
# console's `rak mesh status` against the scripted radio, and the no-allocation check.
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/core/*.cpp
                                                 ${CMAKE_CURRENT_LIST_DIR}/agent/*.cpp
                                                 ${CMAKE_CURRENT_LIST_DIR}/rails/*.cpp)
if(NOT WALTER_AGENT)
  # Without the agent nothing of components/agent/ or the copied library is compiled: the agent's
  # tests and the RAK of §6 (which stands in front of a repeater that is the library's IMesh) go.
  list(FILTER TEST_SOURCES EXCLUDE REGEX "/host_test/agent/")
  list(FILTER SIM_SOURCES EXCLUDE REGEX "/sim_rak\\.cpp$")
endif()
add_executable(core_tests main.cpp ${FAKE_SOURCES} ${SIM_SOURCES} ${TEST_SOURCES} ${FIXTURE_HEADER}
               ${MANIFEST_FIXTURE_HEADER})
target_link_libraries(core_tests PRIVATE core doctest)
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_LIST_DIR})
# The vendored TweetNaCl extract's entry points, for the tests that hold our wrappers against it.
target_include_directories(core_tests SYSTEM PRIVATE ${CORE}/third_party)
twinoak_ours(core_tests)
# FIXTURES_DIR is the copied contract fixtures (task p4-01's second tree), under the same name
# the library's own suites are given it: task p4-04's heartbeat test holds this unit's body
# against meshmaster.dk's `AgentHeartbeat.json` and must read the fixture rather than a
# transcription of it. It is set after the two trees are located, below.
target_compile_definitions(core_tests PRIVATE
  TWINOAK_VERSION="${TWINOAK_VERSION}"
  TWINOAK_SPEC_FIXTURES="${SPEC_FIXTURES}"
  TWINOAK_REPO_ROOT="${REPO}")

if(NOT WALTER_AGENT)
  message(STATUS "WALTER_AGENT=0: the agent, the copied library and their tests are not built")
else()

# -- agent-core, on the host (task p4-01) -------------------------------------------------------
# The copied library of docs/ARCHITECTURE.md §13, compiled here from `components/agent-core/` -
# **the same files the board build compiles**, not a second copy - with this project's sanitizers
# and this repository's flags, so that a warning in the library is an error here as it is there.
# The library is a component of its own and is not under components/core/: core/'s no-ESP-IDF rule
# is core/'s, and the library's own boundary (no Arduino, no esp_*, nothing but <agent/...>,
# ArduinoJson and the standard library) is held by tools/check_rails.py's rail A.
set(AGENT_CORE ${REPO}/components/agent-core)
set(ARDUINOJSON ${REPO}/managed_components/bblanchon__arduinojson)
if(NOT EXISTS ${ARDUINOJSON}/src/ArduinoJson.h)
  message(FATAL_ERROR
    "agent-core needs ArduinoJson, which is a managed component and is not resolved yet: "
    "${ARDUINOJSON}/src/ArduinoJson.h is missing. tools/verify.sh's `deps` step resolves it "
    "(tools/verify.ps1 -Only deps) and runs before the host tests; this project compiles the "
    "same bytes the board gets and does not fetch its own copy.")
endif()

# The sources are the ones the component's own CMakeLists.txt registers, read out of it rather
# than globbed: "the same bytes the board gets" has to mean the same *set* too, and a bump that
# adds a source upstream must not quietly be host-only or board-only. The file is never edited
# here, so reading it is reading upstream.
file(READ ${AGENT_CORE}/CMakeLists.txt AGENT_CORE_CMAKE)
string(REGEX MATCHALL "\"src/[A-Za-z0-9_]+\\.cpp\"" AGENT_CORE_REGISTERED "${AGENT_CORE_CMAKE}")
list(TRANSFORM AGENT_CORE_REGISTERED REPLACE "\"" "")
list(SORT AGENT_CORE_REGISTERED)
file(GLOB AGENT_CORE_ON_DISK RELATIVE ${AGENT_CORE} CONFIGURE_DEPENDS ${AGENT_CORE}/src/*.cpp)
list(SORT AGENT_CORE_ON_DISK)
if(NOT AGENT_CORE_REGISTERED STREQUAL AGENT_CORE_ON_DISK)
  message(FATAL_ERROR
    "components/agent-core/CMakeLists.txt registers [${AGENT_CORE_REGISTERED}] and src/ holds "
    "[${AGENT_CORE_ON_DISK}]. The copy is byte for byte upstream's, so this is upstream's to fix: "
    "docs/upstream.md, tier two.")
endif()
list(TRANSFORM AGENT_CORE_REGISTERED PREPEND ${AGENT_CORE}/ OUTPUT_VARIABLE AGENT_CORE_SOURCES)
list(LENGTH AGENT_CORE_SOURCES AGENT_CORE_COUNT)
message(STATUS "agent-core: ${AGENT_CORE_COUNT} sources from components/agent-core, ArduinoJson from managed_components")

add_library(agent_core STATIC ${AGENT_CORE_SOURCES})
target_include_directories(agent_core PUBLIC ${AGENT_CORE}/include)
# ArduinoJson is upstream's dependency and not ours: a system include path, as doctest's and
# core/third_party's are, so that its warnings stay its own and -Werror stays about the library.
target_include_directories(agent_core SYSTEM PUBLIC ${ARDUINOJSON}/src)
# This repository's flags, in this repository's own dialect - nothing of the library's own is
# exempted and nothing is compiled differently here from the way our code is. Both of the local
# measures v0.2.0 needed are gone at v0.3.0, because upstream took both findings (its report 20):
#
#   the dialect. `ConfigReply.cpp`'s parameter named `unix` made the library unbuildable in any
#   GNU dialect on a Linux host, so it was compiled as `-std=c++17` here. It is `epoch` now, and
#   upstream's `tools/check-library-host.py` greps the library for every name a compiler
#   predefines and compiles it in both dialects, so the next one fails there and not here.
#
#   the warning. GCC 13 at -O2 reports `-Wmaybe-uninitialized` inside ArduinoJson's own
#   `CollectionIterator` and attributes it to the translation unit it was inlined into, which is
#   the library's; `ConfigReply.cpp` and `JobEnvelopes.cpp` had to be exempted whole. They reach
#   ArduinoJson through the library's own `<agent/Json.h>` now, which pushes a diagnostic pragma
#   around the include - scoped to the dependency's code and to that one warning - and upstream's
#   host check carries the sanitizer configuration that raises it, as the regression test.
twinoak_ours(agent_core)

# -- the library's own suites, here (task p4-01) ------------------------------------------------
# test/test_runner (46 cases: all thirteen job types, one send per command, the credential wiped,
# the lease, the offline buffer) and test/test_loop (47: the loop, the heartbeat's bytes, the
# backoff, the reboot guard, every wake case, and what a sleep keeps - the image's bytes, a torn
# one refused whole, the uplink's ledger), copied under host_test/agent-core/ with the four
# fakes and both fixture sets beside them, at upstream's own paths - run here against the
# same bytes. That they pass here as they do upstream is the proof the copy is whole
# (docs/library.md §12).
#
# They are GoogleTest and this repository's own tests are doctest, so GoogleTest is here for these
# two binaries and nothing else - pinned to 1.17.0, the version upstream's [env:native] passes
# them with, by the release tarball's URL and its SHA-256 (no tag, no branch, no floating ref).
# The download lands in the build directory, which is a named volume, so only a cold run fetches
# it; a cold run needs the network for the managed components anyway (tools/verify.sh's `deps`).
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)      # the suites use none of it
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(AGENT_CORE_HOST ${CMAKE_CURRENT_LIST_DIR}/agent-core)

# PROJECT_DIR is upstream's repository root as its suites expect to find it, and here that is two
# directories of this one: the library is `components/agent-core/` and its test support is
# `host_test/agent-core/`. Several cases read files relative to it - the runner's `Citations` case
# reads `test/support/FakeMesh.h` and `platformio.ini`, and since v0.3.0 three cases of the loop
# suite read the library's **own sources** at `lib/agent-core/src/...` to hold what the text of
# `AgentLoop.cpp` and `JobRunner.cpp` may say (no store outside `wake()`, every server call
# through the uplink ledger, no unbounded flush). So upstream's layout is staged here, in the
# build directory, out of symbolic links to the two trees:
#
#   <build>/agent-core-project/lib/agent-core -> components/agent-core   (the bytes the board gets)
#                             /test           -> host_test/agent-core/test
#                             /tests          -> host_test/agent-core/tests
#                             /platformio.ini -> host_test/agent-core/platformio.ini
#
# Links and not copies, on purpose: a case that reads the library's source reads the same bytes
# the board compiles and the same bytes `-Verify` hashed, and a second copy of a copy is a second
# thing to drift. Nothing of this is in the repository - it is one directory of the build volume.
# (Upstream could spare every consumer this with a `LIBRARY_DIR` define beside the three it
# already takes; tasks/phase-4/reports/01.md reports it.)
set(AGENT_CORE_PROJECT ${CMAKE_CURRENT_BINARY_DIR}/agent-core-project)
file(MAKE_DIRECTORY ${AGENT_CORE_PROJECT}/lib)
set(AGENT_CORE_LINKS
  ${AGENT_CORE}            ${AGENT_CORE_PROJECT}/lib/agent-core
  ${AGENT_CORE_HOST}/test  ${AGENT_CORE_PROJECT}/test
  ${AGENT_CORE_HOST}/tests ${AGENT_CORE_PROJECT}/tests
  ${AGENT_CORE_HOST}/platformio.ini ${AGENT_CORE_PROJECT}/platformio.ini)
while(AGENT_CORE_LINKS)
  list(POP_FRONT AGENT_CORE_LINKS original link)
  if(NOT EXISTS ${link})
    file(CREATE_LINK ${original} ${link} RESULT made SYMBOLIC)
    if(NOT made EQUAL 0)
      message(FATAL_ERROR
        "could not link ${link} -> ${original}: ${made}. The library's own suites read files "
        "relative to upstream's repository root, which is staged there; the host tests are built "
        "in the ESP-IDF container (tools/verify.ps1), where a symbolic link needs no privilege.")
    endif()
  endif()
endwhile()

# Each suite has a main() of its own, as it does upstream, so each is its own binary and its own
# ctest entry: a failure names the suite. The three defines are upstream's [env:native]'s, over
# the staged root instead of over that tree.
foreach(suite runner loop)
  set(target agent_core_${suite}_tests)
  add_executable(${target} ${AGENT_CORE_HOST}/test/test_${suite}/test_${suite}.cpp)
  target_link_libraries(${target} PRIVATE agent_core GTest::gtest)
  target_include_directories(${target} PRIVATE ${AGENT_CORE_HOST}/test)
  target_compile_definitions(${target} PRIVATE
    PROJECT_DIR="${AGENT_CORE_PROJECT}"
    FIXTURES_DIR="${AGENT_CORE_HOST}/tests/fixtures/contracts"
    COMPANION_DIR="${AGENT_CORE_HOST}/tests/fixtures/companion")
  twinoak_ours(${target})
endforeach()

# -- components/agent/, on the host (tasks p4-02, p4-03) ----------------------------------------
# This firmware's own half of the library's seams: LinkMesh (mm::IMesh as one request of
# spec/LINK-PROTOCOL.md §6 per call, over core/'s link client), ModemUplink (mm::IUplink as the
# seven calls of the agent API over core/net/'s HTTPS client) and the probes behind the console's
# `rak mesh status`, `agent url` and `agent probe`. Logic, like core/: the same flags, the same
# no-ESP-IDF build step, the host suite's no-allocation check
# (host_test/agent/test_agent_no_allocation.cpp). core_tests links it for the console's tests and
# the uplink's own (host_test/agent/); the GoogleTest binaries below drive it with the library's
# runner and loop.
set(AGENT ${REPO}/components/agent)
file(GLOB_RECURSE AGENT_SOURCES CONFIGURE_DEPENDS ${AGENT}/*.cpp)
add_library(agent STATIC ${AGENT_SOURCES})
target_include_directories(agent PUBLIC ${AGENT}/include)
target_link_libraries(agent PUBLIC core agent_core)
twinoak_ours(agent)
add_custom_target(agent_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${AGENT} -DNAME=agent -P ${REPO}/tools/cmake/no_idf_include.cmake
  COMMENT "agent has no ESP-IDF include"
  VERBATIM)
add_dependencies(agent agent_no_idf_include)
target_link_libraries(core_tests PRIVATE agent)
# And the contract fixtures, for task p4-04's heartbeat test (the comment above core_tests' own
# definitions says why it is set here: the copied trees are located further up this file).
target_compile_definitions(core_tests PRIVATE
  FIXTURES_DIR="${AGENT_CORE_HOST}/tests/fixtures/contracts")
# The library's own job builder (`support/Jobs.h`: a claimed job's JSON, member for member the
# contract fixture's) for the agent's doctest cases (task p4-05). That header is GoogleTest-free;
# the fakes beside it are not and are used by the GoogleTest binaries alone.
target_include_directories(core_tests PRIVATE ${AGENT_CORE_HOST}/test)

# -- the mesh over the link: the runner's scenarios, again, and the link's own cases (task p4-02)
# Two more GoogleTest binaries, because the simulated stock repeater they put behind the RAK is
# the library's FakeMesh, which is written against GoogleTest:
#
#   agent_core_runner_link_tests  test_runner.cpp - the library's own 46 cases, **unchanged** -
#                                 compiled with host_test/agent-link/shim/ first on its include
#                                 path, where `support/FakeMesh.h` and `support/FakePlatform.h`
#                                 shadow the library's with the same classes behind a simulated
#                                 RAK (sim/sim_rak.h) and the agent's LinkMesh: every scenario's
#                                 sends are frames, every reply an EV_MESH_REPLY, every assertion
#                                 the same, and client_tx held equal to the repeater's sends at
#                                 the end of every job;
#   agent_link_tests              host_test/agent-link/test_link_mesh.cpp - what the scenarios do
#                                 not say: the lost event, the lost request, the job timeout, a
#                                 password read back as zeros, the local door per core type,
#                                 client_tx exact, the radio restarting under a job.
#
# And a third (task p4-03), the same way round for the uplink:
#
#   agent_core_loop_uplink_tests  test_loop.cpp - the library's own 47 cases, **unchanged** -
#                                 compiled with host_test/agent-uplink/shim/ first on its include
#                                 path, where `support/FakeUplink.h` shadows the library's
#                                 scripted meshmaster.dk with the same knobs and records behind
#                                 the agent's ModemUplink, core/net/'s client and the scripted
#                                 server (sim/fake_http.h): every call a request over the
#                                 firmware's own HTTP/1.1, every fault a real status, and the
#                                 loop does not know which uplink it has.
#
# All three carry the fakes and the simulators of core_tests (the scripted radio asserts with
# doctest's REQUIRE inside its own code, so doctest's runtime without its main is linked too:
# third_party/doctest_runtime.cpp), and all three carry host_test/shim/, whose
# `support/FakePlatform.h` ties the library's platform clock to the link's fake clock - what
# the runner's scenarios needed through the framing (p4-02) and the loop's through the socket
# (p4-03: a connect's cost is a wait on the clock the platform's budget is measured on).
set(AGENT_LINK ${CMAKE_CURRENT_LIST_DIR}/agent-link)
set(AGENT_UPLINK ${CMAKE_CURRENT_LIST_DIR}/agent-uplink)
set(SHIM ${CMAKE_CURRENT_LIST_DIR}/shim)
foreach(binary agent_core_runner_link_tests agent_link_tests agent_core_loop_uplink_tests)
  if(binary STREQUAL agent_core_runner_link_tests)
    set(source ${AGENT_CORE_HOST}/test/test_runner/test_runner.cpp)
    set(shim ${AGENT_LINK}/shim)
  elseif(binary STREQUAL agent_core_loop_uplink_tests)
    set(source ${AGENT_CORE_HOST}/test/test_loop/test_loop.cpp)
    set(shim ${AGENT_UPLINK}/shim)
  else()
    set(source ${AGENT_LINK}/test_link_mesh.cpp)
    set(shim ${AGENT_LINK}/shim)
  endif()
  add_executable(${binary} ${source} ${FAKE_SOURCES} ${SIM_SOURCES}
                 ${CMAKE_CURRENT_LIST_DIR}/third_party/doctest_runtime.cpp)
  target_link_libraries(${binary} PRIVATE agent GTest::gtest doctest)
  # The binary's own shim first, the shared one next, then the library's own test support they
  # shadow, then this project.
  target_include_directories(${binary} PRIVATE ${shim} ${SHIM} ${AGENT_CORE_HOST}/test
                             ${CMAKE_CURRENT_LIST_DIR})
  target_compile_definitions(${binary} PRIVATE
    PROJECT_DIR="${AGENT_CORE_PROJECT}"
    FIXTURES_DIR="${AGENT_CORE_HOST}/tests/fixtures/contracts"
    COMPANION_DIR="${AGENT_CORE_HOST}/tests/fixtures/companion"
    TWINOAK_VERSION="${TWINOAK_VERSION}"
    TWINOAK_SPEC_FIXTURES="${SPEC_FIXTURES}"
    TWINOAK_REPO_ROOT="${REPO}")
  twinoak_ours(${binary})
endforeach()

# -- the agent behind the seam, with a repeater on the air (task p4-05) -------------------------
# One more GoogleTest binary, for the same reason as the three above: the repeater behind the
# simulated RAK is the library's own FakeMesh, which asserts with GoogleTest.
#
#   agent_backend_tests  host_test/agent-backend/*.cpp - the wake bench (host_test/core/cycle/
#                        wake_bench.h) with the agent's half of a Walter on it and the library's
#                        simulated stock repeater behind the RAK: a `status` on a neighbour
#                        through the whole wake, a `telemetry` and a `config.read` on the own
#                        repeater through the local door, and the simulated month as an exit
#                        node - random coverage, random jobs for neighbours and for the own
#                        repeater, two offers and two applies (one rolls back), a server away for
#                        a week.
#
# Upstream's test support is on the include path as it is (no shim: the repeater's clock is tied
# to the world's by the test itself), and doctest's runtime beside GoogleTest for the bench.
file(GLOB AGENT_BACKEND_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/agent-backend/*.cpp)
add_executable(agent_backend_tests ${AGENT_BACKEND_SOURCES} ${FAKE_SOURCES} ${SIM_SOURCES}
               ${CMAKE_CURRENT_LIST_DIR}/third_party/doctest_runtime.cpp
               ${FIXTURE_HEADER} ${MANIFEST_FIXTURE_HEADER})
# gtest_main, because these sources are this repository's own and carry no main() of their own
# (the library's own suites do, which is why the three binaries above do not link it).
target_link_libraries(agent_backend_tests PRIVATE agent GTest::gtest GTest::gtest_main doctest)
target_include_directories(agent_backend_tests PRIVATE ${AGENT_CORE_HOST}/test
                           ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(agent_backend_tests SYSTEM PRIVATE ${CORE}/third_party)
target_compile_definitions(agent_backend_tests PRIVATE
  PROJECT_DIR="${AGENT_CORE_PROJECT}"
  FIXTURES_DIR="${AGENT_CORE_HOST}/tests/fixtures/contracts"
  COMPANION_DIR="${AGENT_CORE_HOST}/tests/fixtures/companion"
  TWINOAK_VERSION="${TWINOAK_VERSION}"
  TWINOAK_SPEC_FIXTURES="${SPEC_FIXTURES}"
  TWINOAK_REPO_ROOT="${REPO}")
twinoak_ours(agent_backend_tests)

endif()  # WALTER_AGENT

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

if(WALTER_AGENT)
# The library's own two suites (task p4-01), under the same sanitizers as everything else here.
foreach(suite runner loop)
  add_test(NAME agent_core_${suite} COMMAND agent_core_${suite}_tests)
  set_tests_properties(agent_core_${suite} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
endforeach()

# The runner's scenarios through the link, and the link's own cases (task p4-02); the loop's
# scenarios through the modem's uplink (task p4-03); the agent behind the seam with a repeater on
# the air, and the month (task p4-05).
foreach(binary agent_core_runner_link_tests agent_link_tests agent_core_loop_uplink_tests agent_backend_tests)
  add_test(NAME ${binary} COMMAND ${binary})
  set_tests_properties(${binary} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
endforeach()
endif()

# walterctl pushing the specification's fixture release into that console, and the refusals it and
# the Walter answer with (tools/test_walterctl.py).
add_test(NAME walterctl COMMAND ${PYTHON3} ${REPO}/tools/test_walterctl.py
         --console $<TARGET_FILE:walter_console> --generated ${MANIFEST_FIXTURE_DIR})
set_tests_properties(walterctl PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=print_stacktrace=1")
