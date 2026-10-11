#include "core/version.h"

#ifndef TWINOAK_VERSION
#define TWINOAK_VERSION "0.0.0-dev"
#endif

namespace core {

namespace {
// TWINOAK_VERSION must be a string literal for this concatenation to compile; a build that passes
// something else fails here, on purpose, rather than printing it.
constexpr char kVersion[] = TWINOAK_VERSION;
constexpr char kBanner[] = "twinoak-walter " TWINOAK_VERSION;
}  // namespace

const char* version() { return kVersion; }

const char* banner() { return kBanner; }

}  // namespace core
