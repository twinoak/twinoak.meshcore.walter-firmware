# agent-core as an ESP-IDF component.
#
# The twenty-three sources of `src/` and the one include directory of `include/` - what
# `library.json` gives PlatformIO as `srcDir` and `includeDir` - named one by one in the form the
# ESP-IDF's build system reads, because a component lists its sources. A consumer drops this directory
# into `components/` (or names it in `EXTRA_COMPONENT_DIRS`, or takes it from this repository at a
# tag with the component manager's `git` source and a `path` of `lib/agent-core`) and gets the
# library as the component `agent-core`.
#
# **Nothing in this repository builds this file.** There is no ESP-IDF in this repository's
# toolchain - PlatformIO and an Arduino-on-ESP32 framework are what `platformio.ini` names - so
# the furthest a session here can go is a syntax check with no IDF present, which
# `tools/check-idf-component.cmake` does: it defines a stub `idf_component_register`, includes
# this file with `cmake -P`, and then holds the SRCS it was given against the files on disk, so
# that a source added to `src/` and forgotten here is a failure. The first real compile of this
# component is the Walter firmware's phase 4, task 01
# (`twinoak.meshcore.walter-firmware`).
#
# ArduinoJson is declared in `idf_component.yml`, not here: the component manager resolves a
# managed dependency and adds it to this component's requirements itself, which is how the
# Walter's own `components/modem` takes `dptechnics/walter-modem`
# (`twinoak.meshcore.walter-firmware`, `tasks/phase-0/01-the-toolchain.md` and its report). So the
# REQUIRES below is empty: this library needs no other component of the consumer's, which is the
# whole point of the three interfaces.

idf_component_register(
  SRCS
    "src/AgentLoop.cpp"
    "src/Airtime.cpp"
    "src/Backoff.cpp"
    "src/CliReplies.cpp"
    "src/ConfigApply.cpp"
    "src/ConfigRead.cpp"
    "src/ConfigReply.cpp"
    "src/Credentials.cpp"
    "src/Envelopes.cpp"
    "src/Iso8601.cpp"
    "src/JobEnvelopes.cpp"
    "src/JobResults.cpp"
    "src/JobRunner.cpp"
    "src/KeptImage.cpp"
    "src/Log.cpp"
    "src/LogRing.cpp"
    "src/LoginSequence.cpp"
    "src/Observations.cpp"
    "src/RateLimit.cpp"
    "src/ReplyCodec.cpp"
    "src/ResultBuffer.cpp"
    "src/ServerPolicy.cpp"
    "src/Sessions.cpp"
  INCLUDE_DIRS
    "include"
  REQUIRES)

# The strictness `library.json`'s build.flags carry under PlatformIO, carried here too: this
# library is -Werror wherever it is built, and `-fno-rtti` is the ESP-IDF's own default for C++
# (the Walter's build says so) written down rather than assumed. Exceptions are the consumer's
# choice - the ESP-IDF defaults to none and tools/check-library-host.sh compiles the library with
# -fno-exceptions to prove it needs none - so nothing here turns them on or off.
# (The guard is for the syntax check: in `cmake -P` script mode there is no component and no
# target to put a flag on.)
if(DEFINED COMPONENT_LIB)
  target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif()
