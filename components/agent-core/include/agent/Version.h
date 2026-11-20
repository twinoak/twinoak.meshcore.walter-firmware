// The library's own version, which is this repository's.
//
// The firmware's version is the board's and comes from the build (src/version.h: the release
// workflow bakes it in as -D MESHMASTER_AGENT_VERSION). The library's is this repository's, and
// since task p1.11-52 the one place that number lives is the file `VERSION` at the root, which
// names the release that comes NEXT: this header, `library.json` and `idf_component.yml` carry a
// copy of it, test/test_library holds this one against the file on disk, and tools/release.ps1's
// `check` - a step of every CI run - holds all three. A consumer pins the number as a tag
// `vX.Y.Z` of this repository (docs/upstream.md); the release workflow moves all four on, in its
// own pull request, after it has published (docs/release.md).
//
// A consumer that reports what it runs reports both: the firmware's version and this.

#pragma once

#define AGENT_CORE_VERSION "0.7.3"

namespace mm {

// The same string, for code that would rather have a symbol than a macro.
constexpr const char* kAgentCoreVersion = AGENT_CORE_VERSION;

}  // namespace mm
