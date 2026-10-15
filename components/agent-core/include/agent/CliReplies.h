// How the firmware's CLI replies are read, on both ends (task p4-54; MeshMaster.Contracts'
// CliReplies in twinoak/meshmaster.dk).
//
// CommonCLI at d9296435 (src/helpers/CommonCLI.cpp) has no status code. A `set` it took starts
// with `OK` - `OK` for most keys (line 504, advert.interval), `OK - repeat is now OFF`/`ON`
// (535), `OK - 50.0%` for dutycycle (460); a `get` it answered starts with `> `; a value it
// refused starts with `Error` or `ERROR` (`Error, max 64` at 639/648/657, `ERROR: dutycycle must
// be 1-100` at 453, `Error: interval range is 60-240 minutes` at 499); a clock it would not move
// back is `ERR: clock cannot go backwards` (207) or `(ERR: clock cannot go backwards)` (226); a
// key handleSetCmd does not know is `unknown config: …` (802), one handleGetCmd does not know is
// `??: …` (985), a command handleCommand does not know is `Unknown command` (444), and the
// repeater's own `Err - bad params` a malformed setperm (simple_repeater/MyMesh.cpp). Everything
// else - `clock`'s time, `ver`, a neighbour list - is an answer and not an error.
//
// One reading, shared with the server, so the job's outcome here and the settlement there cannot
// disagree. Host-portable: two string functions.

#pragma once

namespace mm {

// The firmware took the command: the reply, with leading whitespace skipped, starts with `expect`
// - `OK` for a `set`, `OK - loaded` for the line that ends a region load, and the two sentences
// `region home` and `region default` answer with, neither of which starts with `OK` (task p4-57).
// Ordinal, case-sensitive: every refusal CommonCLI spells starts otherwise.
bool cliReplyIsConfirmed(const char* reply, const char* expect = "OK");

// A refusal in one of the spellings above, case-folded; nullptr is not a reply and not an error
// either.
bool cliReplyIsHardError(const char* reply);

}  // namespace mm
