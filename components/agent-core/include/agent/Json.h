// ArduinoJson, included the one way the library includes it.
//
// The library parses three things with ArduinoJson - the config reply, a job claim and a job's
// parameters - and `ConfigReply.cpp` and `JobEnvelopes.cpp` are the only two files that touch it.
// Both reach it through here rather than through `<ArduinoJson.h>` directly, so that the one
// warning the dependency raises in our translation units is turned off in one place and for the
// dependency's own code alone.
//
// **The warning.** GCC 13 reports `-Wmaybe-uninitialized` inside ArduinoJson 7.4.3's own
// `CollectionIterator` - `ObjectImpl.hpp:36` and `:43`, `return iterator()` - when `findKey` is
// inlined into `getMember`, and attributes it to the translation unit it was inlined into, which
// is ours. It is a middle-end warning raised after inlining, so neither an `-isystem` include
// path nor a per-file `-Wno-` on the dependency silences it: the first consumer of the library
// hit it with `-Werror` on and had to exempt our two sources whole
// (twinoak.meshcore.walter-firmware, its p4-01, *Reported upstream* §2). A diagnostic pragma does
// reach it, because GCC resolves a late diagnostic against the pragma state **at the location it
// is reported at** - and that location is inside the include below.
//
// It takes an optimiser that keeps the inlined copy and still cannot see through it: on CI's GCC
// 13.3 the plain `-O2` of `tools/check-library-host.py`'s matrix does not raise it, and the
// sanitizer configuration a consumer's host tests are built with (`-fsanitize=address,undefined`
// at `-O2`, which is exactly what the first consumer runs) does. That configuration is in the
// matrix for this reason, and it is the regression test for this header: remove the pragma and
// the check goes red.
//
// Scoped to the include and to one warning. `-Wno-error` for the library, or for these two files,
// would cost a consumer a real warning of ours, which `library.json`'s `build.flags` exist to
// give them.

#pragma once

// GCC alone: `-Wmaybe-uninitialized` is not a warning clang has, and an unknown warning name in a
// pragma is itself a warning there - which under this library's `-Werror` would be an error in a
// compiler nothing here is built with (the task's own *out of scope*).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <ArduinoJson.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
