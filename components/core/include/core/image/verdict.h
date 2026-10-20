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
// And §5's one variation, for the board's base - a `provision` file (§4a; task p5-01):
//
//   5p. instead of 5 and 6, which a base cannot pass (it carries no descriptor and no load
//       address): the image's app_region lies inside this build's region for the board, and the
//       file passes §4a's seven rules (core/image/base.h) against **this build's** region - the
//       stricter of the two, the one the radio really has.
//
// A base that passes 1, 2, 3, 4 and 5p yields an **AcceptedBase** and never an AcceptedImage:
// the two have no conversion between them, so a base cannot be passed where an image is wanted
// or the other way round, and a mistake does not compile.
//
// A file that fails any of them is refused with the number of the check and a sentence. A file
// that passes all six yields an AcceptedImage - the only way to make one - which the image store
// (task p2-02) demands before it stores anything: a type, not a convention.
//
// The six run in two phases, because the store needs the first three before the file exists
// anywhere (task p2-02): judgeManifest() is checks 1, 2, 3 and the half of 6 that needs no file,
// and answers with an **ExpectedFile** - what an accepted manifest promises about the one file
// wanted, which is what the store demands before it will erase a sector; judgeFile() is checks 4,
// 5 and the other half of 6 over the bytes - or 4 and 5p for a base - and answers with the
// AcceptedImage, or the AcceptedBase. judge() is the two in order and is what a caller that has
// the whole file already uses.
//
// One ImageVerdict is ~33 KB (the parser's tokens, the parsed manifest, and §4a's reader) and
// belongs in static storage, never on a task's stack.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/flasher/image_source.h"
#include "core/image/base.h"
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
  // §5's **5p**, which replaces 5 and 6 for a `provision` file. It is not a seventh number of
  // §5's list, so it is not one here either: describe() says "check 5p".
  kBase = 50,
};

const char* describe(Check check);

struct Outcome {
  Check failed = Check::kNone;
  // One sentence. Static text, except for a kBase refusal: §4a's words carry a line number or an
  // address the file gave, so that sentence is the verdict's own buffer (ImageVerdict::base()),
  // which lives as long as the verdict and until its next judge(). Both callers - the transfer's
  // response and the console's line - print it at once.
  const char* why = "";
  bool ok() const { return failed == Check::kNone; }
};

// What an accepted manifest promises about the one file wanted: which slot and use it is for,
// how large it is, the SHA-256 it must have, and where it loads. Constructed by ImageVerdict and
// by nothing else, like AcceptedImage below - so that nothing can ask the image store to erase a
// sector without a manifest signed by a key this build trusts, naming this file for this slot
// (checks 1, 2, 3 and the manifest's half of 6). It is a promise and not a verdict: the bytes it
// describes have not been seen. Copyable, so that the store may keep one.
class ExpectedFile {
 public:
  board::Slot slot() const { return slot_; }
  Use use() const { return use_; }
  std::uint32_t size() const { return size_; }
  const std::uint8_t* sha256() const { return sha256_; }
  std::uint32_t loadAddress() const { return load_address_; }
  const char* name() const { return name_; }        // the file's name in the release
  const char* version() const { return version_; }  // the manifest's version
  // The box link the image speaks (the manifest's `link.major`) and, when its `requires` names
  // one, the link major that must already be there (`requires.link_major`). The Walter's own
  // update (task p2-04) reads S-7's "speaks both majors" off these two before it erases a slot.
  std::uint32_t linkMajor() const { return link_major_; }
  bool hasRequiresLinkMajor() const { return has_requires_link_major_; }
  std::uint32_t requiresLinkMajor() const { return requires_link_major_; }

 private:
  friend class ImageVerdict;
  ExpectedFile() = default;

  board::Slot slot_;
  Use use_ = Use::kUnknown;
  std::uint32_t size_ = 0;
  std::uint8_t sha256_[kSha256Size] = {};
  std::uint32_t load_address_ = 0;
  char name_[kMaxFileName] = {};
  char version_[kVersionSize] = {};
  std::uint32_t link_major_ = 0;
  bool has_requires_link_major_ = false;
  std::uint32_t requires_link_major_ = 0;
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
  // The manifest's `commit` (§2: 40 hex characters), so that the store's record can say which
  // source the image was built from.
  const char* commit() const { return commit_; }
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
  char commit_[kCommitSize + 1] = {};
  std::uint32_t link_major_ = 0;
  std::uint32_t link_minor_ = 0;
  std::uint8_t key_id_[kKeyIdSize] = {};
};

// What passed for a base: this `provision` file, for this slot, judged against this build's own
// region for the board. Constructed by ImageVerdict and by nothing else, like the two tokens
// above; copyable, so that the store may keep one.
//
// **It is not an AcceptedImage and cannot become one.** A base is not an image: it carries no
// descriptor, it has no load address, it is written once by a person's command (spec/SYSTEM.md
// S-5a) and never by an update. The store's two doors say which they take, so handing one where
// the other belongs is a compile error and not a bug.
class AcceptedBase {
 public:
  board::Slot slot() const { return slot_; }
  Use use() const { return Use::kProvision; }  // the only use a base has
  const ImageEntry& image() const { return image_; }  // a copy: valid after the verdict moves on
  const FileEntry& file() const { return file_; }
  const std::uint8_t* sha256() const { return file_.sha256; }
  // What §4a's rules found in it: the bootloader's address, the flash byte count, the UICR
  // registers it gives, NRFFW[1] if it has one, and which SoftDevice it carries.
  const BaseSummary& summary() const { return summary_; }
  // The addresses it covers, so that task p5-03 can walk its words (core/image/base.h's
  // BaseWords) without parsing the file a second time to find them.
  const BaseRuns& runs() const { return runs_; }
  // The region §4a's rules were applied against: this build's, for the board in the slot.
  std::uint32_t regionStart() const { return region_start_; }
  std::uint32_t regionEnd() const { return region_end_; }
  const char* version() const { return version_; }
  const char* commit() const { return commit_; }
  const std::uint8_t* keyId() const { return key_id_; }

 private:
  friend class ImageVerdict;
  AcceptedBase() = default;

  board::Slot slot_;
  ImageEntry image_;
  FileEntry file_;
  BaseSummary summary_;
  BaseRuns runs_;
  std::uint32_t region_start_ = 0;
  std::uint32_t region_end_ = 0;
  char version_[kVersionSize] = {};
  char commit_[kCommitSize + 1] = {};
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

  // The first phase: checks 1, 2, 3 and the half of 6 that needs no file. On success expected()
  // is the promise about the file, and judgeFile() may be called with the bytes when they are
  // somewhere readable. The manifest's bytes need not stay around afterwards.
  Outcome judgeManifest(const std::uint8_t* manifest, std::size_t manifest_size,
                        const char* signature_line, std::size_t signature_size,
                        const SlotNeeds& needs, Use use, const flasher::BoardProfile* profile);

  // What the last accepting judgeManifest() promises about the file, or nullptr. Valid until the
  // next judgeManifest() or judge(); copy it to keep it.
  const ExpectedFile* expected() const { return has_expected_ ? &expected_ : nullptr; }

  // The second phase: checks 4, 5 and the half of 6 that needs the file, over the bytes of the
  // file the last judgeManifest() chose. Refused with kFile and "no manifest has been judged"
  // when there was no accepting judgeManifest().
  Outcome judgeFile(const flasher::ImageSource& file);

  // The token of the last judge() or judgeFile(), or nullptr - and nullptr for a `provision`
  // file, whose token is acceptedBase(). Valid until the next judge(); copy it to keep it.
  const AcceptedImage* accepted() const { return has_accepted_ ? &accepted_ : nullptr; }

  // The same for a base: the token of the last accepting judge() of a `provision` file, or
  // nullptr. Exactly one of accepted() and acceptedBase() is ever not null.
  const AcceptedBase* acceptedBase() const { return has_accepted_base_ ? &accepted_base_ : nullptr; }

  // What §4a made of the last `provision` file judged, whatever the outcome: the rule that
  // refused it and its sentence, or the summary. Where an Outcome's `why` points after a kBase
  // refusal.
  const BaseResult& base() const { return base_result_; }

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
  // The base's reader and what it last found. ~2.5 KB, which is why an ImageVerdict belongs in
  // static storage (as it did before: the parser's tokens are 20 KB of it).
  BaseReader base_reader_;
  BaseResult base_result_;
  AcceptedBase accepted_base_;
  bool has_accepted_base_ = false;
  // What the manifest phase left for the file phase: the promise, which image and file of the
  // manifest were chosen, and the three things the file's checks still need.
  ExpectedFile expected_;
  bool has_expected_ = false;
  std::uint8_t chosen_image_ = 0;
  std::uint8_t chosen_file_ = 0;
  const flasher::BoardProfile* chosen_profile_ = nullptr;
  std::uint8_t chosen_key_id_[kKeyIdSize] = {};
};

// Linked, not called, until the image store (task p2-02) has something to judge: main/main.cpp
// keeps this function's result in a volatile, the way it keeps the modem library's anchor, and
// this function reads judge()'s address through a volatile - so --gc-sections keeps the six
// checks in the image and the build proves they compile and link for the board.
const void* verdictAnchor();

}  // namespace image
}  // namespace core
