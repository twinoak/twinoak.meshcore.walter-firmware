// A bounded reader of one flat JSON object (task p4-05): the payload of a `twinoak.*` job as the
// claim carried it, which the copied library does not keep - `mm::parseClaimResponse` reads the
// payloads of its own thirteen types into named fields and drops the rest (agent/JobEnvelopes.h
// has no raw payload), so this firmware keeps the text of an extension job's payload itself
// (walter_agent/modem_uplink.h) and reads its members here. The four payloads are flat objects
// of strings, integers and booleans (meshmaster.dk's `FirmwareOfferPayload`, `FirmwareApplyPayload`,
// `FirmwareStatePayload`, `PowerCyclePayload`; their fixtures are the test), and nothing of them
// is nested but what this skips over.
//
// It is **not** a JSON parser: no document, no heap, no validation beyond what it needs to find a
// member at depth one of an object. A string's escapes are undone the way the writer's clause 1
// and 2 made them (agent/JsonWriter.h), and a `\uXXXX` - which none of the four payloads ever
// carries - becomes `?` rather than a guess at a code point. A member that is not there, or not
// of the kind asked for, is "not found", and the caller decides what that means.
//
// Logic: no ESP-IDF include, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace agent {
namespace json {

// A span of the text: a member's raw value - a string with its quotes, a number, `true`/`false`/
// `null`, or a whole nested object or array - or nothing.
struct Value {
  const char* at = nullptr;
  std::size_t len = 0;
  bool found() const { return at != nullptr; }
};

// The value of member `name` at depth one of the object `obj` (`len` bytes, beginning at its
// `{` after any whitespace). Nested containers and strings are stepped over whole. Not found when
// the object is malformed, the member is absent, or the text ends first.
Value member(const char* obj, std::size_t len, const char* name);

// The `index`-th element of an array value, or not found.
Value element(const Value& array, int index);

// The members, by kind. Each is false - and `out` untouched - when the value is not of that kind
// or, for a string, does not fit `cap` (with its terminator).
bool text(const Value& value, char* out, std::size_t cap);
bool integer(const Value& value, long long* out);
bool boolean(const Value& value, bool* out);
bool isNull(const Value& value);
bool isObject(const Value& value);

}  // namespace json
}  // namespace agent
