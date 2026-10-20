#include "core/image/verdict.h"

#include <cstring>

namespace core {
namespace image {

const char* describe(Check check) {
  switch (check) {
    case Check::kNone: return "accepted";
    case Check::kSignature: return "check 1, the signature";
    case Check::kManifest: return "check 2, the manifest";
    case Check::kImage: return "check 3, the image for this slot";
    case Check::kFile: return "check 4, the file's size and hash";
    case Check::kDescriptor: return "check 5, the descriptor inside the file";
    case Check::kRegion: return "check 6, the region";
    case Check::kBase: return "check 5p, the base";
  }
  return "?";
}

namespace {

Outcome refused(Check check, const char* why) {
  Outcome o;
  o.failed = check;
  o.why = why;
  return o;
}

}  // namespace

Outcome ImageVerdict::judge(const std::uint8_t* manifest, std::size_t manifest_size,
                            const char* signature_line, std::size_t signature_size,
                            const SlotNeeds& needs, Use use, const flasher::BoardProfile* profile,
                            const flasher::ImageSource& file) {
  const Outcome first =
      judgeManifest(manifest, manifest_size, signature_line, signature_size, needs, use, profile);
  if (!first.ok()) return first;
  return judgeFile(file);
}

Outcome ImageVerdict::judgeManifest(const std::uint8_t* manifest, std::size_t manifest_size,
                                    const char* signature_line, std::size_t signature_size,
                                    const SlotNeeds& needs, Use use,
                                    const flasher::BoardProfile* profile) {
  manifest_ = Manifest{};
  has_accepted_ = false;
  has_accepted_base_ = false;
  has_expected_ = false;

  // 1. The signature. Everything else waits for it: a manifest nobody we trust signed is not
  //    read at all (§1: the server is not trusted with more than delivering bytes).
  const TrustedKey* key = nullptr;
  const SignatureStatus signed_by =
      verifyManifestSignature(trust_, manifest, manifest_size, signature_line, signature_size, &key);
  if (signed_by != SignatureStatus::kOk) return refused(Check::kSignature, describe(signed_by));

  // 2. The manifest: parsed into the bounded structures, then §2's rules.
  const ParseResult parsed = parser_.parse(reinterpret_cast<const char*>(manifest), manifest_size, &manifest_);
  if (!parsed.ok) return refused(Check::kManifest, describe(parsed.error));
  const ValidateResult valid = validate(manifest_);
  if (!valid.ok) return refused(Check::kManifest, describe(valid.reason));

  // 3. The image for this slot, meeting its requirements, with a file of the wanted use.
  const Choice choice = chooseImage(manifest_, needs, use, unit_);
  if (!choice.ok()) return refused(Check::kImage, describe(choice.reason));
  const ImageEntry& image = *choice.image;
  const FileEntry& entry = *choice.file;

  // 6 (and 5p's first half, which is the same thing), the part that needs no file: the manifest's
  //    region against this build's profile. Done before the file is read so that a manifest that
  //    tries to widen the region costs nothing; reported as check 6 for an `swd` file and as 5p
  //    for a base, because that is what each of them is. §5: "a manifest cannot widen what may be
  //    written" - and a base is written further outside the application than anything else is.
  if (use == Use::kSwd || use == Use::kProvision) {
    const Check as = use == Use::kProvision ? Check::kBase : Check::kRegion;
    if (profile == nullptr) return refused(as, "no board profile for this slot in this build");
    if (std::strcmp(profile->name, image.board) != 0) {
      return refused(as, "the board profile given is not the image's board");
    }
    const nrf52::FlashRegion region{image.app_region_start, image.app_region_end};
    if (!profile->accepts(region)) {
      return refused(as, "the manifest's app_region is not inside this build's region for the board");
    }
  }

  // The promise: what the file must be, for whoever has to make room for it before it arrives
  // (the image store, task p2-02). Nothing of the bytes has been seen yet.
  ExpectedFile& e = expected_;
  e = ExpectedFile{};
  e.slot_ = needs.slot;
  e.use_ = use;
  e.size_ = entry.size;
  std::memcpy(e.sha256_, entry.sha256, kSha256Size);
  e.load_address_ = entry.load_address;
  std::memcpy(e.name_, entry.name, kMaxFileName);
  std::memcpy(e.version_, manifest_.version, kVersionSize);
  e.link_major_ = manifest_.link_major;
  e.has_requires_link_major_ = image.has_requires_link_major;
  e.requires_link_major_ = image.requires_link_major;
  chosen_image_ = choice.image_index;
  chosen_file_ = choice.file_index;
  chosen_profile_ = profile;
  std::memcpy(chosen_key_id_, key->id, kKeyIdSize);
  has_expected_ = true;
  return Outcome{};
}

Outcome ImageVerdict::judgeFile(const flasher::ImageSource& file) {
  has_accepted_ = false;
  has_accepted_base_ = false;
  if (!has_expected_) return refused(Check::kFile, "no manifest has been judged");
  const ImageEntry& image = manifest_.images[chosen_image_];
  const FileEntry& entry = image.files[chosen_file_];
  const Use use = expected_.use_;
  const flasher::BoardProfile* profile = chosen_profile_;

  // 4 and 5, in one pass over the file: the size is known before a byte is read. A base has no
  // descriptor to find (§4a), so nothing looks for one in it: 5 and 6 are 5p instead, below.
  const bool is_base = use == Use::kProvision;
  if (file.size() != entry.size) return refused(Check::kFile, describe(FileHasher::Result::kSizeMismatch));
  hasher_.begin(entry.size, entry.sha256);
  scanner_.reset();
  for (std::uint32_t at = 0; at < entry.size;) {
    const std::uint32_t left = entry.size - at;
    const std::uint32_t n = left < sizeof piece_ ? left : static_cast<std::uint32_t>(sizeof piece_);
    if (!file.read(at, piece_, n)) return refused(Check::kFile, "the file could not be read");
    hasher_.update(piece_, n);
    if (!is_base) scanner_.update(piece_, n);
    at += n;
  }
  const FileHasher::Result hashed = hasher_.finish();
  if (hashed != FileHasher::Result::kOk) return refused(Check::kFile, describe(hashed));

  // 5p, the other half: §4a's seven rules over the bytes, against **this build's** region for
  // the board and not the manifest's - the stricter of the two (§5). The region was already held
  // to lie inside this build's by judgeManifest, so this is the narrower of the two by
  // construction; it is the build's that is used because it is the one the radio really has.
  if (is_base) {
    base_result_ = base_reader_.read(file, profile->app.begin, profile->app.end);
    if (!base_result_.ok()) return refused(Check::kBase, base_result_.why);
    AcceptedBase& b = accepted_base_;
    b = AcceptedBase{};
    b.slot_ = expected_.slot_;
    b.image_ = image;
    b.file_ = entry;
    b.summary_ = base_result_.summary;
    b.runs_ = base_result_.runs;
    b.region_start_ = profile->app.begin;
    b.region_end_ = profile->app.end;
    std::memcpy(b.version_, manifest_.version, kVersionSize);
    std::memcpy(b.commit_, manifest_.commit, kCommitSize);
    b.commit_[kCommitSize] = 0;
    std::memcpy(b.key_id_, chosen_key_id_, kKeyIdSize);
    has_accepted_base_ = true;
    return Outcome{};
  }

  const Found found = scanner_.result();
  if (found.count == 0) return refused(Check::kDescriptor, "the file holds no valid descriptor");
  if (found.count > 1) return refused(Check::kDescriptor, "the file holds more than one valid descriptor");
  const DescriptorMismatch agrees =
      descriptorAgrees(found.fields, manifest_, image, expected_.slot_);
  if (agrees != DescriptorMismatch::kNone) return refused(Check::kDescriptor, describe(agrees));

  // 6, the half that needs the file's entry: the file inside the manifest's region, and inside
  //    the profile's (which the region check above already implies, and is cheap to say again).
  if (use == Use::kSwd) {
    const std::uint64_t end = static_cast<std::uint64_t>(entry.load_address) + entry.size;
    if (!(image.app_region_start <= entry.load_address && end <= image.app_region_end)) {
      return refused(Check::kRegion, "the file does not fit the manifest's app_region");
    }
    if (!profile->holds(entry.load_address, entry.size)) {
      return refused(Check::kRegion, "the file does not fit this build's region for the board");
    }
  }

  AcceptedImage& a = accepted_;
  a = AcceptedImage{};
  a.slot_ = expected_.slot_;
  a.use_ = use;
  a.image_ = image;
  a.file_ = entry;
  a.descriptor_ = found.fields;
  a.descriptor_offset_ = found.offset;
  std::memcpy(a.version_, manifest_.version, kVersionSize);
  std::memcpy(a.commit_, manifest_.commit, kCommitSize);
  a.commit_[kCommitSize] = 0;
  a.link_major_ = manifest_.link_major;
  a.link_minor_ = manifest_.link_minor;
  std::memcpy(a.key_id_, chosen_key_id_, kKeyIdSize);
  has_accepted_ = true;
  return Outcome{};
}

namespace {

using Judge = Outcome (ImageVerdict::*)(const std::uint8_t*, std::size_t, const char*, std::size_t,
                                        const SlotNeeds&, Use, const flasher::BoardProfile*,
                                        const flasher::ImageSource&);

}  // namespace

const void* verdictAnchor() {
  // A volatile read of a pointer to judge(): the compiler must load it, so the reference to the
  // function is in this object's data and the linker keeps the function and all it calls.
  static Judge volatile keep = &ImageVerdict::judge;
  static const char kAnchored = 0;
  const Judge loaded = keep;
  return loaded == nullptr ? nullptr : &kAnchored;
}

}  // namespace image
}  // namespace core
