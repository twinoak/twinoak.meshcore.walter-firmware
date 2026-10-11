// The protocol's numbers in words (task p1-02): what the console prints and what a log line will
// want. Every string is the specification's own wording, with the section it comes from, so that a
// person reading `rak status` is reading spec/LINK-PROTOCOL.md and not somebody's paraphrase.
//
// None of them ever answers a null pointer, and a value this firmware does not know is "unknown" -
// §5.1 says a minor version adds values to exactly these fields and that a receiver passes on what
// it does not know. So the console prints the number *and* the word (`role 1 (repeater)`): the
// number is the answer, the word is the gloss, and a radio one minor ahead is still readable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/link/codec/wire.h"

namespace core {
namespace link {
namespace client {

// The longest any of the two lists below can be, with room for the unknown-bits tail.
constexpr std::size_t kWordsMax = 128;

const char* statusName(std::uint8_t value);              // §3
const char* roleName(std::uint8_t value);                  // §5.1
const char* resetReasonName(std::uint8_t value);         // §5.3
const char* timeStateName(std::uint8_t value);            // §5.4
const char* timeSourceName(std::uint8_t value);          // §5.4
const char* rebootModeName(std::uint8_t value);            // §5.5
const char* sendResultName(std::uint8_t value);            // §6.2
const char* replyStateName(std::uint8_t value);           // §6.3
const char* pathModeName(std::uint8_t value);              // §6.1
const char* attentionName(std::uint8_t value);           // §8
const char* eventName(std::uint8_t value);            // §4

// The capability bits of §5.1, comma-separated, with any bit this firmware does not know as
// "unknown 0x..". Always terminated; returns the length written; `none` when no bit is set.
std::size_t capabilityWords(std::uint32_t caps, char* out, std::size_t size);

// The STATUS flags of §5.3, the same way.
std::size_t statusFlagWords(std::uint16_t flags, char* out, std::size_t size);

}  // namespace client
}  // namespace link
}  // namespace core
