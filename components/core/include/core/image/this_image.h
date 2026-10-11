// This firmware's own descriptor: the 96 bytes of spec/FIRMWARE-MANIFEST.md §4 that say, from
// inside the image, that it is the `manager` role on a `walter` in band `lte-m`, at this version,
// speaking this version of the box link.
//
// It is one `const` object (`kThisImage`), aligned to 4 bytes because §4 says a reader finds a
// descriptor on a 4-byte boundary, built entirely at compile time - its CRC-32 included - and
// referenced by main/main.cpp, which is what keeps the linker (-ffunction-sections -fdata-sections
// -Wl,--gc-sections) from dropping it. The release tooling finds exactly one of it in each
// published file and holds it against the manifest entry (tools/release/build_release.py); an
// image that referenced it from nowhere would carry none and the release would refuse to be made.
#pragma once

#include "core/image/descriptor.h"

namespace core {
namespace image {

// The box link this firmware speaks: spec/LINK-PROTOCOL.md is "protocol 1.0". The same two numbers
// go into the descriptor's `link_major`/`link_minor`, into the manifest's `link` object, and into
// the link client's HELLO when phase 1 writes it.
constexpr std::uint8_t kLinkMajor = 1;
constexpr std::uint8_t kLinkMinor = 0;

// The bytes in the image. Declared `extern` so that it has external linkage and main/ can
// reference it; defined, aligned and built in image/this_image.cpp.
extern const Descriptor kThisImage;

// The fields of kThisImage, parsed back out of its bytes - so a caller reads what is in the image
// rather than a second copy of the intent. All-zero fields mean the bytes are not a descriptor,
// which cannot happen in a build this file compiled in.
Fields this_image();

}  // namespace image
}  // namespace core
