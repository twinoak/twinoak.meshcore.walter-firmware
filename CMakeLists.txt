# The Walter firmware - an ESP-IDF project, built only inside the image tools/idf-image.txt names
# (S-9): tools/verify.ps1 on this machine, the same script in CI. Layout: docs/ARCHITECTURE.md §1.
cmake_minimum_required(VERSION 3.22)

# The product version. The release (phase 0, task 02) passes -DPROJECT_VER=X.Y.Z+<sha7>; a build
# that is told nothing is "0.0.0-dev" - never `git describe`, because a build says what it was told
# it is, and the manifest's `version` must be the same string (spec/FIRMWARE-MANIFEST.md §2).
if(NOT DEFINED PROJECT_VER)
  set(PROJECT_VER "0.0.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(twinoak-walter)
