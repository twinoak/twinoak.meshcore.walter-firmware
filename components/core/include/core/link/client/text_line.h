// The few fields of a scanner's line this firmware reads (task p16-01).
//
// spec/LINK-PROTOCOL.md §9: NoiseScope 0.1 speaks NDJSON lines on this same UART, not frames -
// "one JSON object per line, `\n`-terminated. Any line **not** starting with `{` is
// human-readable chatter and must be ignored by machine parsers" (the repeater firmware's
// docs/noisescope.md §5, *Framing*). That document also says what a Walter needs of a line:
// "Publish verbatim ... (parse only the `t` field)". This is that, and one field more.
//
// So this is **not a JSON parser** and must never become one. It reads three string fields and
// one boolean of the top-level object - `t`, `cmd`, `ok`, `msg` - which is all §9's half of a
// conversation needs: whether a line is the `ack` of the line that was said, whether that ack
// was ok, what it said, and whether a `done` for a command is among a run of lines. A `sweep`
// line's arrays are forwarded whole and never looked into (task p16-04's business), and
// understanding a line beyond these four fields is out of scope here.
//
// It allocates nothing, it copies nothing, and every answer is a view into the caller's line.
#pragma once

#include <cstddef>

#include "core/link/codec/view.h"

namespace core {
namespace link {
namespace client {

// The value of a top-level `"name": "..."` field, without its quotes. False when the object has
// no such field, when the value is not a string, or when the line was cut inside it. Escape
// sequences are left as they arrived: the four fields this reads are plain words in every
// example of the scanner's own document, and a reader that unescaped would need room to put the
// result in.
bool lineText(Text line, Text name, Text* out);

// The value of a top-level `"name": true|false`. False when there is no such field or the value
// is neither word.
bool lineFlag(Text line, Text name, bool* out);

// Whether the line's `t` is this word: `lineIs(line, text("ack"))`.
bool lineIs(Text line, Text type);

// Whether the line is the `done` of this command - `{"t":"done","cmd":"sweep",…}` - which is
// what the scanner sends "when a finite activity completes" (noisescope.md §5, *Messages*).
bool isDoneFor(Text line, Text command);

// The first word of a command line: `hist 869.4 869.8 25` -> `hist`. §9's `ack` carries the
// command word in `cmd`, so this is how a said line and its answer are matched.
Text firstWord(Text line);

}  // namespace client
}  // namespace link
}  // namespace core
