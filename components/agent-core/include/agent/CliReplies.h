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
// disagree. Host-portable: string functions and nothing else.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// The firmware took the command: the reply, with leading whitespace skipped, starts with `expect`
// - `OK` for a `set`, `OK - loaded` for the line that ends a region load, and the two sentences
// `region home` and `region default` answer with, neither of which starts with `OK` (task p4-57).
// Ordinal, case-sensitive: every refusal CommonCLI spells starts otherwise.
bool cliReplyIsConfirmed(const char* reply, const char* expect = "OK");

// A refusal in one of the spellings above, case-folded; nullptr is not a reply and not an error
// either.
bool cliReplyIsHardError(const char* reply);

// ------------------------------------------------------------------ the prefix a line carries
//
// **A console reply carries no tag, so the line carries a tag of its own** (task p1.10-49).
// `MyMesh::handleCommand` reflects an optional prefix: a line whose third character is `|` and
// which is longer than four characters has its first three characters copied to the head of the
// reply, and the rest is run (examples/simple_repeater/MyMesh.cpp:1235-1239 at 81dbae59, the
// same four lines in `simple_room_server`, `simple_sensor` and the companion). `a7|get name` is
// answered `a7|> Grenaa Chimney #1`. It is upstream's own feature, there for the companion
// radio's CLI, and in the repeater since commit 1209d54d of 27 February 2025 - released in
// `repeater-v1.0.0a`.
//
// Without it the first console text from the peer while one is awaited is the answer, whatever
// it answers: the reply to `get name` can arrive while `get radio` is awaited and be stored as
// the radio, and a configuration read is thirty such lines in a row. Nothing would show it
// either - a `get` answers a value and not the key it belongs to.

// The prefix's own length on the wire: two characters and the `|`.
constexpr size_t kCliPrefixLen = 3;

// **The FIRMWARE_VER_LEVEL from which a login reply says the prefix is reflected.** The level is
// in the login reply since commit c2159634 of 25 September 2025 (`repeater-v1.9.0`), seven
// months *after* the prefix, so a repeater that names any level at all reflects it. A repeater
// that names none is either older than the prefix (`rep-v6` and before, which has no access
// list either and is no exit-node target) or between the two releases - and the two cannot be
// told apart, so such a line goes unprefixed, as every line did before this task. Nothing
// looser than the level is used: the `ver` text is a console reply, which is the very thing
// being matched.
constexpr uint8_t kCliPrefixVerLevel = 1;

// The prefix of sequence number `n`, as `"xy|"`: two lowercase hex characters of the byte, which
// are never `|` themselves. `out` takes kCliPrefixLen + 1 bytes.
void cliPrefixFor(uint8_t n, char* out, size_t cap);

// **Whether a prefix may be put on this line at all**, which is three readings of the pinned
// firmware and not a preference:
//
//   - `strlen(command) > 4` is `handleCommand`'s own condition, so a line of fewer than two
//     characters prefixed would keep its prefix and be run as `a7|<line>`. The region loader's
//     terminator is one space, and that is how it stays bare;
//   - the reply is written into the same 160-byte window the prefix is reflected into
//     (`onPeerDataRecv`: `uint8_t temp[166]`, the reply at `&temp[5]`), so three characters of
//     the prefix are three the reply has not got. Every handler at the pin writes well inside
//     that except `region` and `region def …`, which are `RegionMap::exportTo(reply, 160)` - a
//     full map plus a prefix would run off the end of that buffer, which is the repeater's
//     stack. Those two go bare;
//   - a line whose reply nobody awaits is never prefixed, because the prefix alone is a
//     non-empty reply (`if (text_len > 0)`) and would turn a silent line into a packet. That is
//     the caller's to know, not this function's: the region loader's lines go out through
//     `IMesh::sendCli` directly and never reach here.
bool cliLineTakesPrefix(const char* command);

// The reply with this line's prefix taken off, or **nullptr when it is not this line's reply**:
// another prefix, or none. `prefix` is what `cliPrefixFor` wrote; an empty prefix passes every
// reply through, which is the unprefixed line's rule as it always was.
const char* cliReplyBehindPrefix(const char* reply, const char* prefix);

}  // namespace mm
