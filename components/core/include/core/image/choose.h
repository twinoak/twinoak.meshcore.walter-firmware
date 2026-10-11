// Choosing the image (task p2-01): spec/FIRMWARE-MANIFEST.md §5 check 3 - "there is an image
// whose role, board and band are what this radio position holds or may hold, and whose
// `requires` this unit meets" - and the one file of the wanted `use` in it: `swd` for a radio,
// `ota` for the Walter's own product.
//
// What the slot needs is the caller's to say (the image store and the update know what a slot
// holds); what this unit is comes from the build: core::version() and the link major(s) it
// speaks (S-7: when the link's major changes the Walter goes first and speaks both until the
// radio has followed, so it is a list).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/image/manifest.h"

namespace core {
namespace image {

// What a radio position holds or may hold: the three words of an image's identity (§2).
struct SlotNeeds {
  board::Slot slot;
  const char* role = nullptr;   // "repeater", "scanner", "manager" ...
  const char* board = nullptr;  // "rak4630-single", "walter" ...
  const char* band = nullptr;   // "868", "433", "lte-m"
};

// What this unit is, as far as a manifest's `requires` can ask.
constexpr std::size_t kMaxLinkMajors = 2;  // S-7: at most the old major and the new one
struct ThisUnit {
  const char* version = "";  // core::version(): X.Y.Z, X.Y.Z+<sha7>, 0.0.0-dev ...
  std::uint8_t link_majors[kMaxLinkMajors] = {};
  std::size_t link_major_count = 0;
  bool speaks(std::uint32_t major) const {
    for (std::size_t i = 0; i < link_major_count; ++i) {
      if (link_majors[i] == major) return true;
    }
    return false;
  }
};

// This build: its version string and the link major it speaks (core/image/this_image.h).
ThisUnit thisUnit();

enum class ChooseReason : std::uint8_t {
  kNone,
  kNoImage,               // no image has the role, board and band the slot needs
  kAmbiguous,             // more than one has (ids differ but the three words do not)
  kRequiresMalformed,     // the image's `requires` cannot be read, so it cannot be met
  kRequiresWalterNewer,   // requires.walter is newer than this build
  kRequiresLinkMajor,     // requires.link_major is a major this build does not speak
  kManifestLinkMajor,     // the manifest's own link.major is one this build does not speak
  kNoFileOfUse,           // the image has no file of the wanted use
};

const char* describe(ChooseReason reason);

struct Choice {
  const ImageEntry* image = nullptr;  // into the manifest given; null when reason != kNone
  const FileEntry* file = nullptr;
  std::uint8_t image_index = 0;
  std::uint8_t file_index = 0;
  ChooseReason reason = ChooseReason::kNone;
  bool ok() const { return reason == ChooseReason::kNone; }
};

// The one image and its file of `use`, or why there is none. The manifest must have passed
// validate(); this does not check it again.
Choice chooseImage(const Manifest& manifest, const SlotNeeds& needs, Use use, const ThisUnit& unit);

// Version comparison for `requires.walter`: X.Y.Z, numerically, major first; a `-pre-release`
// suffix sorts below the bare version and `+build` is ignored (semver 2.0.0 §10 and §11). Returns
// -1, 0 or 1 like strcmp; false when either text is not X.Y.Z at all.
bool compareVersions(const char* a, const char* b, int* out);

}  // namespace image
}  // namespace core
