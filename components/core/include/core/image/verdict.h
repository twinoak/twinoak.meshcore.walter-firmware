// The verdict (task p2-01): spec/FIRMWARE-MANIFEST.md §5, the six checks a Walter makes before a
// file goes anywhere near a radio, in the specification's order, by their numbers:
//
//   1. the signature line names a key this build trusts and verifies over the manifest's bytes
//      (signature.h, trust.h);
//   2. the manifest parses and validates (manifest.h), its product known;
//   3. there is one image for what this slot holds or may hold, meeting `requires`, with a file
//      of the wanted use (choose.h);
//   4. the file's size and SHA-256 are the entry's (file_check.h);
//   5. the file holds exactly one descriptor and it agrees with the entry (file_check.h);
//   6. for an swd file: it fits the manifest's app_region, and that region lies inside this
//      build's own profile of the board (core/flasher/board_profile.h) - a manifest cannot widen
//      what may be written.
//
// A file that fails any of them is refused with the number of the check and a sentence. A file
// that passes all six yields an AcceptedImage - the only way to make one - which the image store
// (task p2-02) demands before it stores anything: a type, not a convention.
//
// One ImageVerdict is ~30 KB (the parser's tokens and the parsed manifest) and belongs in static
// storage, never on a task's stack.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/image_source.h"
#include "core/image/choose.h"
#include "core/image/descriptor.h"
#include "core/image/file_check.h"
#include "core/image/manifest.h"
#include "core/image/signature.h"
#include "core/image/trust.h"

namespace core {
namespace image {

// §5's numbering. kNone is "accepted".
enum class Check : std::uint8_t {
  kNone = 0,
  kSignature = 1,
  kManifest = 2,
  kImage = 3,
  kFile = 4,
  kDescriptor = 5,
  kRegion = 6,
};

const char* describe(Check check);

struct Outcome {
  Check failed = Check::kNone;
  const char* why = "";  // one sentence, static text
  bool ok() const { return failed == Check::kNone; }
};

// What passed: this file, for this slot, with this hash and descriptor. Constructed by
// ImageVerdict and by nothing else - there is no public constructor, so the only one in
// existence is the verdict's own, handed out as a pointer after an accepting judge(); copyable,
// so that the store may keep it.
class AcceptedImage {
 public:
  board::Slot slot() const { return slot_; }
  Use use() const { return use_; }
  const ImageEntry& image() const { return image_; }  // a copy: valid after the verdict moves on
  const FileEntry& file() const { return file_; }
  const std::uint8_t* sha256() const { return file_.sha256; }
  const Fields& descriptor() const { return descriptor_; }
  std::size_t descriptorOffset() const { return descriptor_offset_; }
  // The manifest's version and link, which the descriptor was held against.
  const char* version() const { return version_; }
  std::uint32_t linkMajor() const { return link_major_; }
  std::uint32_t linkMinor() const { return link_minor_; }
  // Which key signed the manifest it came from.
  const std::uint8_t* keyId() const { return key_id_; }

 private:
  friend class ImageVerdict;
  AcceptedImage() = default;

  board::Slot slot_;
  Use use_ = Use::kUnknown;
  ImageEntry image_;
  FileEntry file_;
  Fields descriptor_;
  std::size_t descriptor_offset_ = 0;
  char version_[kVersionSize] = {};
  std::uint32_t link_major_ = 0;
  std::uint32_t link_minor_ = 0;
  std::uint8_t key_id_[kKeyIdSize] = {};
};

class ImageVerdict {
 public:
  ImageVerdict(const TrustTable& trust, const ThisUnit& unit) : trust_(trust), unit_(unit) {}

  // The six checks, in order, over one manifest (its bytes exactly as delivered), its signature
  // line, and one file. `use` is the file wanted: kSwd for a radio slot, with the slot's board
  // profile; kOta for the Walter's own, with no profile. On success accepted() is the token; on
  // failure it is nullptr and the outcome says which check and why.
  Outcome judge(const std::uint8_t* manifest, std::size_t manifest_size, const char* signature_line,
                std::size_t signature_size, const SlotNeeds& needs, Use use,
                const flasher::BoardProfile* profile, const flasher::ImageSource& file);

  // The token of the last judge(), or nullptr. Valid until the next judge(); copy it to keep it.
  const AcceptedImage* accepted() const { return has_accepted_ ? &accepted_ : nullptr; }

  // The manifest as last parsed (whatever the outcome), for a console line.
  const Manifest& manifest() const { return manifest_; }

 private:
  const TrustTable& trust_;
  ThisUnit unit_;
  ManifestParser parser_;
  Manifest manifest_;
  FileHasher hasher_;
  DescriptorScanner scanner_;
  std::uint8_t piece_[256];
  AcceptedImage accepted_;
  bool has_accepted_ = false;
};

}  // namespace image
}  // namespace core
