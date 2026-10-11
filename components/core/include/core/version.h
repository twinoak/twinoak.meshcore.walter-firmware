// What this firmware calls itself. core/ has no ESP-IDF include (docs/ARCHITECTURE.md §1): the
// version comes in as a compile definition, TWINOAK_VERSION, set by the build from the project
// version (CMakeLists.txt, PROJECT_VER) and by host_test/ from its own option; without one it is
// "0.0.0-dev". Everything here is a string literal composed at compile time: no heap, no
// formatting, nothing that can fail at boot.
#pragma once

namespace core {

// The product name, as the manifest's `product` minus "-firmware" (spec/FIRMWARE-MANIFEST.md §2).
constexpr const char* kProduct = "twinoak-walter";

// The version the build gave, e.g. "0.0.0-dev" or "0.3.1+1a2b3c4".
const char* version();

// "<product> <version>": the first line on the console, and what the bench reads.
const char* banner();

}  // namespace core
