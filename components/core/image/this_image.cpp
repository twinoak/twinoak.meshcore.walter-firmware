#include "core/image/this_image.h"

#ifndef TWINOAK_VERSION
#define TWINOAK_VERSION "0.0.0-dev"
#endif

namespace core {
namespace image {

namespace {

// What this image is (spec/FIRMWARE-MANIFEST.md §2 and §4): one role on one board in one band.
// `slot` is "any" because this image is not for a radio position at all - it is the manager's own;
// `flags` is 0, which is all §4 defines. The version is the build's (CMakeLists.txt, PROJECT_VER)
// and the same macro core/version.cpp turns into the banner, so that the console, the descriptor
// and the release's manifest cannot disagree about what this firmware is.
//
// Built here, at compile time, CRC-32 and all. A version string too long for the 32-byte field
// stops the build with the sentence below instead of shipping a descriptor nobody can parse.
constexpr Built kBuilt =
    build(Role::kManager, kAnySlot, kLinkMajor, kLinkMinor, 0, TWINOAK_VERSION, "walter", "lte-m");
static_assert(kBuilt.ok,
              "this image's descriptor does not fit: TWINOAK_VERSION is longer than 31 characters, "
              "or the board or band name is longer than its field "
              "(spec/FIRMWARE-MANIFEST.md section 4)");

}  // namespace

// The object in the image: `const`, so .rodata and therefore flash; `alignas(4)` so that its
// offset inside the .bin is on the boundary §4 says a reader searches on. Initialised from a
// constexpr object of a trivially copyable type, so it is constant-initialised: there is no
// start-up code behind it and nothing to go wrong at boot.
alignas(4) const Descriptor kThisImage = kBuilt.descriptor;

Fields this_image() {
  Fields fields;
  // Parsed rather than copied, so that what a caller sees is what a reader of the image sees.
  // It cannot fail - kBuilt.ok is a static_assert above - and if it ever did, the fields say
  // role `unknown` and empty strings rather than something that looks right.
  if (!parse(kThisImage.bytes, kDescriptorSize, &fields)) {
    return Fields{};
  }
  return fields;
}

}  // namespace image
}  // namespace core
