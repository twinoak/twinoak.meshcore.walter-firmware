#include "core/image/signature.h"

namespace core {
namespace image {

namespace {

constexpr char kAlgorithm[] = "ed25519";
constexpr std::size_t kAlgorithmSize = sizeof(kAlgorithm) - 1;
constexpr std::size_t kBase64Size = 88;  // 64 bytes: 21 groups of 3 and one byte over, padded "=="

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool lowerHexByte(const char* text, std::uint8_t* out) {
  std::uint8_t value = 0;
  for (int i = 0; i < 2; ++i) {
    const char c = text[i];
    std::uint8_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<std::uint8_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<std::uint8_t>(c - 'a' + 10);
    } else {
      return false;  // §3: lowercase; an upper-case digit is not the line the spec describes
    }
    value = static_cast<std::uint8_t>((value << 4) | d);
  }
  *out = value;
  return true;
}

}  // namespace

std::size_t base64Decode(const char* text, std::size_t size, std::uint8_t* out, std::size_t out_size) {
  if (text == nullptr || size == 0 || size % 4 != 0) return 0;
  std::size_t written = 0;
  for (std::size_t at = 0; at < size; at += 4) {
    const bool last = at + 4 == size;
    int v[4];
    int pad = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text[at + i];
      if (c == '=') {
        // Padding: only in the last group, only in its last two places, and nothing after it.
        if (!last || i < 2) return 0;
        pad++;
        v[i] = 0;
        continue;
      }
      if (pad > 0) return 0;  // a character after a '='
      v[i] = base64Value(c);
      if (v[i] < 0) return 0;
    }
    const std::uint32_t bits = (static_cast<std::uint32_t>(v[0]) << 18) |
                               (static_cast<std::uint32_t>(v[1]) << 12) |
                               (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
    const std::size_t bytes = static_cast<std::size_t>(3 - pad);
    // Canonical: the bits the padding leaves unused must be zero (RFC 4648 §3.5). The 24 bits
    // are v0 23..18, v1 17..12, v2 11..6, v3 5..0: with one '=' the two bytes are bits 23..8 and
    // v2's low two bits (7..6) are unused; with two, the one byte is bits 23..16 and v1's low
    // four (15..12) are. The padded places are zero already, so the masks reach down to bit 0.
    if (pad == 1 && (bits & 0xFFu) != 0) return 0;
    if (pad == 2 && (bits & 0xFFFFu) != 0) return 0;
    if (written + bytes > out_size) return 0;
    out[written++] = static_cast<std::uint8_t>(bits >> 16);
    if (bytes > 1) out[written++] = static_cast<std::uint8_t>(bits >> 8);
    if (bytes > 2) out[written++] = static_cast<std::uint8_t>(bits);
  }
  return written;
}

bool parseSignatureLine(const char* text, std::size_t size, SignatureLine* out) {
  if (text == nullptr || out == nullptr) return false;
  // Strip one line ending, and only one.
  if (size >= 2 && text[size - 2] == '\r' && text[size - 1] == '\n') {
    size -= 2;
  } else if (size >= 1 && text[size - 1] == '\n') {
    size -= 1;
  }
  const std::size_t want = kAlgorithmSize + 1 + kKeyIdHexSize + 1 + kBase64Size;
  if (size != want) return false;
  for (std::size_t i = 0; i < kAlgorithmSize; ++i) {
    if (text[i] != kAlgorithm[i]) return false;
  }
  if (text[kAlgorithmSize] != ' ') return false;
  const char* id = text + kAlgorithmSize + 1;
  SignatureLine parsed;
  for (std::size_t i = 0; i < kKeyIdSize; ++i) {
    if (!lowerHexByte(id + 2 * i, &parsed.key_id[i])) return false;
  }
  if (id[kKeyIdHexSize] != ' ') return false;
  const char* sig = id + kKeyIdHexSize + 1;
  if (base64Decode(sig, kBase64Size, parsed.signature, kEd25519SignatureSize) != kEd25519SignatureSize) {
    return false;
  }
  *out = parsed;
  return true;
}

const char* describe(SignatureStatus status) {
  switch (status) {
    case SignatureStatus::kOk: return "the signature verifies";
    case SignatureStatus::kNoSignature: return "no signature: the line is not `ed25519 <key id> <base64>`";
    case SignatureStatus::kUnknownKey: return "the key id names no key this build trusts";
    case SignatureStatus::kBadSignature: return "the signature does not verify over the manifest's bytes";
  }
  return "?";
}

SignatureStatus verifyManifestSignature(const TrustTable& trust, const std::uint8_t* manifest,
                                        std::size_t size, const char* line, std::size_t line_size,
                                        const TrustedKey** used) {
  if (used != nullptr) *used = nullptr;
  SignatureLine parsed;
  if (!parseSignatureLine(line, line_size, &parsed)) return SignatureStatus::kNoSignature;
  // The key by its id, and that one only (§3: "it lets a verifier that trusts more than one key
  // pick the right one"). A table with no keys has no id to match: everything stops here.
  const TrustedKey* key = trust.find(parsed.key_id);
  if (key == nullptr) return SignatureStatus::kUnknownKey;
  if (used != nullptr) *used = key;
  if (manifest == nullptr && size > 0) return SignatureStatus::kBadSignature;
  return ed25519Verify(key->key, parsed.signature, manifest, size) ? SignatureStatus::kOk
                                                                   : SignatureStatus::kBadSignature;
}

}  // namespace image
}  // namespace core
