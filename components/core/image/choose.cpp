#include "core/image/choose.h"

#include <cstring>

#include "core/image/this_image.h"
#include "core/version.h"

namespace core {
namespace image {

namespace {

struct Parsed {
  std::uint32_t part[3];
  bool prerelease;
};

// X.Y.Z[-pre][+build]: three decimal numbers (each at most nine digits) separated by dots, then
// nothing, a '-' or a '+'.
bool parseVersion(const char* text, Parsed* out) {
  if (text == nullptr) return false;
  const char* p = text;
  for (int i = 0; i < 3; ++i) {
    if (*p < '0' || *p > '9') return false;
    std::uint32_t value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      if (++digits > 9) return false;
      value = value * 10 + static_cast<std::uint32_t>(*p - '0');
      ++p;
    }
    out->part[i] = value;
    if (i < 2) {
      if (*p != '.') return false;
      ++p;
    }
  }
  out->prerelease = *p == '-';
  return *p == 0 || *p == '-' || *p == '+';
}

bool same(const char* a, const char* b) { return a != nullptr && b != nullptr && std::strcmp(a, b) == 0; }

}  // namespace

ThisUnit thisUnit() {
  ThisUnit unit;
  unit.version = core::version();
  unit.link_majors[0] = kLinkMajor;
  unit.link_major_count = 1;
  return unit;
}

const char* describe(ChooseReason reason) {
  switch (reason) {
    case ChooseReason::kNone: return "chosen";
    case ChooseReason::kNoImage: return "no image in the manifest has this slot's role, board and band";
    case ChooseReason::kAmbiguous: return "more than one image has this slot's role, board and band";
    case ChooseReason::kRequiresMalformed: return "the image's requires cannot be read";
    case ChooseReason::kRequiresWalterNewer: return "the image requires a newer Walter than this build";
    case ChooseReason::kRequiresLinkMajor: return "the image requires a link major this build does not speak";
    case ChooseReason::kManifestLinkMajor: return "the manifest's link major is one this build does not speak";
    case ChooseReason::kNoFileOfUse: return "the image has no file of the wanted use";
  }
  return "?";
}

bool compareVersions(const char* a, const char* b, int* out) {
  Parsed pa;
  Parsed pb;
  if (!parseVersion(a, &pa) || !parseVersion(b, &pb)) return false;
  for (int i = 0; i < 3; ++i) {
    if (pa.part[i] < pb.part[i]) {
      *out = -1;
      return true;
    }
    if (pa.part[i] > pb.part[i]) {
      *out = 1;
      return true;
    }
  }
  // The same X.Y.Z: a pre-release is lower than the release (semver §11).
  if (pa.prerelease == pb.prerelease) {
    *out = 0;
  } else {
    *out = pa.prerelease ? -1 : 1;
  }
  return true;
}

Choice chooseImage(const Manifest& manifest, const SlotNeeds& needs, Use use, const ThisUnit& unit) {
  Choice choice;
  // The one image with the three words. Two of them is a manifest that cannot say which it
  // means, and is refused rather than guessed at.
  int found = -1;
  for (std::uint8_t i = 0; i < manifest.image_count; ++i) {
    const ImageEntry& image = manifest.images[i];
    if (!same(image.role, needs.role) || !same(image.board, needs.board) || !same(image.band, needs.band)) {
      continue;
    }
    if (found >= 0) {
      choice.reason = ChooseReason::kAmbiguous;
      return choice;
    }
    found = i;
  }
  if (found < 0) {
    choice.reason = ChooseReason::kNoImage;
    return choice;
  }
  const ImageEntry& image = manifest.images[found];

  // Its requirements. A requirement this firmware cannot read is not met: a manifest that says
  // "you need X" in words this build does not understand is asking for something it has not.
  if (image.requires_present && image.requires_malformed) {
    choice.reason = ChooseReason::kRequiresMalformed;
    return choice;
  }
  if (image.has_requires_walter) {
    int order = 0;
    if (!compareVersions(unit.version, image.requires_walter, &order) || order < 0) {
      choice.reason = ChooseReason::kRequiresWalterNewer;
      return choice;
    }
  }
  if (image.has_requires_link_major && !unit.speaks(image.requires_link_major)) {
    choice.reason = ChooseReason::kRequiresLinkMajor;
    return choice;
  }
  // And the link the image itself speaks (the manifest's `link`): S-7 - a radio image of a major
  // this Walter does not speak is not for this unit yet, whatever `requires` says. Not for the
  // Walter's own `ota` image (task p2-04): that one is *how* a new major arrives - the Walter
  // goes first - and whether it still speaks the radio's major is the self-update's own rule,
  // read off `link.major` and `requires.link_major` (core/update/self_update.h).
  if (use != Use::kOta && !unit.speaks(manifest.link_major)) {
    choice.reason = ChooseReason::kManifestLinkMajor;
    return choice;
  }

  for (std::uint8_t f = 0; f < image.file_count; ++f) {
    if (image.files[f].use == use) {
      choice.image = &image;
      choice.file = &image.files[f];
      choice.image_index = static_cast<std::uint8_t>(found);
      choice.file_index = f;
      choice.reason = ChooseReason::kNone;
      return choice;
    }
  }
  choice.reason = ChooseReason::kNoFileOfUse;
  return choice;
}

}  // namespace image
}  // namespace core
