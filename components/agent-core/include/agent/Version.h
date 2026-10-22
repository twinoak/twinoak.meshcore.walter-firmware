// The library's own version, which is this repository's.
//
// The firmware's version is the board's and comes from the build (src/version.h: the release
// workflow bakes the tag in as -D MESHMASTER_AGENT_VERSION). The library's is its manifest's:
// `library.json` and `idf_component.yml` both say this number, a consumer pins it as a tag
// `vX.Y.Z` of this repository (docs/upstream.md), and test/test_library holds the three against
// each other so that one of them cannot move without the others.
//
// A consumer that reports what it runs reports both: the firmware's version and this.

#pragma once

#define AGENT_CORE_VERSION "0.3.0"

namespace mm {

// The same string, for code that would rather have a symbol than a macro.
constexpr const char* kAgentCoreVersion = AGENT_CORE_VERSION;

}  // namespace mm
