// The signature line (task p2-01): spec/FIRMWARE-MANIFEST.md §3, `ed25519 <key id> <signature>`,
// one line of ASCII - and check 1 of §5: the line names a key this build trusts and the
// signature verifies over the manifest's bytes.
//
// Parsed strictly: the word `ed25519`, one space, exactly 16 lowercase hex characters, one space,
// exactly 88 characters of standard base64 (64 bytes, so two `=` of padding, the padding bits
// zero), then the end of the text or one line ending (LF or CR LF). Anything else - another
// algorithm, upper-case hex, a tab, a second line, a non-canonical encoding - is "no signature",
// which is a refusal: nothing is tried. The key is chosen by its id from the table; an id the
// table does not hold is a refusal too, and the other keys are not tried.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/image/crypto/ed25519.h"
#include "core/image/trust.h"

namespace core {
namespace image {

// "ed25519 " + 16 + " " + 88 + "\r\n": the longest line that is a signature line.
constexpr std::size_t kSignatureLineMax = 7 + 1 + kKeyIdHexSize + 1 + 88 + 2;

struct SignatureLine {
  std::uint8_t key_id[kKeyIdSize] = {};
  std::uint8_t signature[kEd25519SignatureSize] = {};
};

// The line as text (not necessarily terminated) in, its two fields out; false is "no signature".
bool parseSignatureLine(const char* text, std::size_t size, SignatureLine* out);

// Standard base64 (RFC 4648 §4), strict: the alphabet only, a length that is a multiple of four,
// `=` only at the end and only as the encoding needs it, the unused bits of the last character
// zero. Returns the number of bytes written, or 0 for anything that is not that.
std::size_t base64Decode(const char* text, std::size_t size, std::uint8_t* out, std::size_t out_size);

enum class SignatureStatus : std::uint8_t {
  kOk,
  kNoSignature,   // the line is not a signature line
  kUnknownKey,    // the key id names no key this build trusts
  kBadSignature,  // the signature does not verify over these bytes with that key
};

const char* describe(SignatureStatus status);

// Check 1, over a manifest that is all in memory. `used`, when given, receives the key the line
// named (even when the signature then fails), or nullptr.
SignatureStatus verifyManifestSignature(const TrustTable& trust, const std::uint8_t* manifest,
                                        std::size_t size, const char* line, std::size_t line_size,
                                        const TrustedKey** used = nullptr);

}  // namespace image
}  // namespace core
