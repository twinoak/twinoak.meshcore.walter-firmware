// What a trace line may not carry out (task p10-06). The modem library's TX and RX lines are the
// only account of what the modem was told and what it answered, and the stall that silences this
// unit cannot be read without them (`tasks/phase-10/FOLLOWUPS.md`); but TLS is inside the modem,
// so the UART carries the **HTTP head as it is** - `Authorization: Bearer <server.token>` in
// plaintext - and the SIM's PIN goes out as `AT+CPIN=<lte.pin>` on the same wire. Both are
// secrets this firmware prints nowhere (`docs/console.md`, *Secrets*; S-12), and a trace is a
// printed line.
//
// So every line of the trace passes through this before it reaches the port, and it takes out
// three things:
//
//   1. **the value of `Authorization:`**, to the end of the header - the one place a token is
//      put, by this firmware's own request writer (`core/net/http1.cpp`);
//   2. **the value of `AT+CPIN=`**, to the end of the command - the one place a PIN is put, by
//      the library's `unlockSIM`;
//   3. **every occurrence of a value it was given**, byte for byte: the token and the PIN as
//      they are in the settings.
//
// 1 and 2 are positional and exact - the position is this firmware's own doing - and they hold
// even for a value this redactor was never told (a token typed while the trace was on). 3 is the
// belt: it holds wherever a value turns up, in a line of the library's or in any other line that
// reaches the same hook. Neither is a pattern-guess at what a secret looks like.
//
// **Nothing ever grows.** A stretch taken out is overwritten with asterisks of its own length -
// the same asterisks the console echoes while a secret is being typed - so `apply()` rewrites in
// place, cannot overflow the caller's buffer and cannot fail. That matters because the caller is
// a log hook on the modem library's own task (`components/modem/modem_trace.h`).
#pragma once

#include <cstdarg>
#include <cstddef>

#include "core/settings/keys.h"

namespace core {
namespace trace {

// The longest value held: a setting's longest (`core/settings/keys.h`, `server.token`'s 160).
constexpr std::size_t kMaxSecret = settings::kMaxValueLen;

// How many are held at once: the server's token and the SIM's PIN, and no third secret exists
// (`docs/console.md`, *Secrets*).
constexpr std::size_t kSecrets = 2;

// Shorter than this a held value is not searched for, because a two-character "secret" would
// asterisk ordinary text out of every line and leave a trace nobody can read. Four is the SIM
// PIN's own minimum (`core/settings/keys.h`: 4 to 8 digits) and far below any token. A value
// shorter than this is still taken out by the two positional rails above, which are the exact
// ones - the token and the PIN each have exactly one place they are written into - so nothing
// is lost but the belt.
constexpr std::size_t kLeastSecret = 4;

// What replaces a byte of a secret.
constexpr char kAsterisk = '*';

// The two header and command prefixes, so that a test can name them rather than retype them.
constexpr const char* kAuthorization = "Authorization:";
constexpr const char* kPinCommand = "AT+CPIN=";

class Redactor {
 public:
  // Holds `value` as secret number `which` (0 to kSecrets - 1). A null or empty value holds
  // nothing, which is right for an unset setting. Returns false - and holds **nothing** of it,
  // so that `apply` can never half-remove one - when it is longer than kMaxSecret or `which` is
  // not a slot.
  bool keep(std::size_t which, const char* value);

  // Holds nothing at all again.
  void forget();

  // How many of the slots hold a value that is searched for (one that is at least kLeastSecret
  // long).
  std::size_t holding() const;

  // Rewrites `line` in place: every stretch named above becomes asterisks of the same length.
  // Returns how many stretches were taken out, so that a caller can count the lines it changed
  // and the bench can see the rail working. `length` is the line's length; it never changes.
  std::size_t apply(char* line, std::size_t length) const;

 private:
  char secret_[kSecrets][kMaxSecret + 1] = {};
};

// What one line of the trace is, after the hook has made it.
struct Formatted {
  std::size_t length = 0;  // bytes of `buffer` to write; a longer line is cut, never dropped
  std::size_t taken = 0;   // stretches the redactor took out of it
};

// **The log hook's whole body**, so that all of it has a host test and not only the redaction:
// `format` and `args` - exactly what `esp_log_set_vprintf` hands over, one call to one line -
// formatted into `buffer`, then the secrets taken out of it. The caller holds the lock around
// this and writes `buffer` to the port afterwards; it has no decision of its own left to make
// (`components/modem/modem_trace.cpp`).
//
// A line longer than `capacity` is cut at the buffer. Cutting is safe for the rail: the two
// positional rails end a value at the end of the buffer as readily as at a CR, so a head cut in
// the middle of its `Authorization:` has that header blanked to the cut.
Formatted formatAndRedact(const Redactor& redactor, char* buffer, std::size_t capacity,
                          const char* format, std::va_list args);

}  // namespace trace
}  // namespace core
