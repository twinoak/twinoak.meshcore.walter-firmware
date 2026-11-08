// How the firmware's CLI replies are read, on both ends (task p4-54; MeshMaster.Contracts'
// CliReplies in twinoak/meshmaster.dk).
//
// CommonCLI at 81dbae59 (src/helpers/CommonCLI.cpp; since 1.18 the radio keys answer from
// src/helpers/CommonRadioPrefs.cpp, with the same words) has no status code. A `set` it took starts
// with `OK` - `OK` for most keys (line 511, advert.interval), `OK - repeat is now OFF`/`ON`
// (541), `OK - 50.0%` for dutycycle (CommonRadioPrefs.cpp:97); a `get` it answered starts with `> `; a value it
// refused starts with `Error` or `ERROR` (`Error, max 64` at 557/566/575, `ERROR: dutycycle must
// be 1-100` at CommonRadioPrefs.cpp:91, `Error: interval range is 60-240 minutes` at 506); a clock it would not move
// back is `ERR: clock cannot go backwards` (218) or `(ERR: clock cannot go backwards)` (237); a
// key handleSetCmd does not know is `unknown config: …` (696), one handleGetCmd does not know is
// `??: …` (833), a command handleCommand does not know is `Unknown command` (482), and the
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
