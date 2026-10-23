# The Walter firmware - an ESP-IDF project, built only inside the image tools/idf-image.txt names
# (S-9): tools/verify.ps1 on this machine, the same script in CI. Layout: docs/ARCHITECTURE.md §1.
cmake_minimum_required(VERSION 3.22)

# The product version. The release (phase 0, task 02) passes -DPROJECT_VER=X.Y.Z+<sha7>; a build
# that is told nothing is "0.0.0-dev" - never `git describe`, because a build says what it was told
# it is, and the manifest's `version` must be the same string (spec/FIRMWARE-MANIFEST.md §2).
if(NOT DEFINED PROJECT_VER)
  set(PROJECT_VER "0.0.0-dev")
endif()

# A test build (task p2-01): -DTWINOAK_TRUST_TEST_KEY=1 puts the specification's public test key
# into the trust table (components/core/CMakeLists.txt) and stamps the version with +TESTKEY, so
# that the console, the descriptor and esp_app_desc_t all say what the build is;
# tools/release/build_release.py refuses to release it. Off, and the key absent, by default -
# tools/verify.sh always passes the option explicitly, because a cache variable sticks.
option(TWINOAK_TRUST_TEST_KEY "trust the specification's public test key (a test build, never released)" OFF)
if(TWINOAK_TRUST_TEST_KEY)
  set(PROJECT_VER "${PROJECT_VER}+TESTKEY")
endif()

# The MeshMaster agent behind the backend seam (task p4-05, docs/ARCHITECTURE.md §13). Off, the
# image is phases 0-3's unit: the wake cycle over the null backend, no agent, no copied library
# in the link. main/CMakeLists.txt reads it; host_test/CMakeLists.txt has the same option for the
# host build, and tools/verify.sh runs the wake cycle's tests in both configurations.
option(WALTER_AGENT "the MeshMaster agent behind the backend seam" ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(twinoak-walter)
