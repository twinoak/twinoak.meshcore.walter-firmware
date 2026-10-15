// UTC timestamps the way the server writes them: "2026-09-23T14:00:01Z", with an optional
// fraction ("...:01.1234567Z", which .NET's DateTime serialises when the value has one).
//
// Host-portable, no time zone: the board's clock is UTC and so is every timestamp of the agent
// protocol (spec/API.md section 1). timegm() is not on every host, so the civil-date arithmetic
// is here in the open.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// Parse "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+00:00)" into UNIX seconds. Any other shape, any
// other zone, or a date outside 1970..2105 is refused.
bool parseUtc(const char* text, uint32_t& epoch);

// Format UNIX seconds as "YYYY-MM-DDTHH:MM:SSZ". `out` needs 21 bytes.
void formatUtc(uint32_t epoch, char* out, size_t cap);

// Format UNIX seconds as "HH:MM:SS". `out` needs 9 bytes.
void formatClock(uint32_t epoch, char* out, size_t cap);

}  // namespace mm
