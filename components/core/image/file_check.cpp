#include "core/image/file_check.h"

#include <cstring>

namespace core {
namespace image {

// -- FileHasher -------------------------------------------------------------------------------

void FileHasher::begin(std::uint32_t expected_size, const std::uint8_t expected_sha256[kSha256Size]) {
  hash_.reset();
  seen_ = 0;
  expected_size_ = expected_size;
  for (std::size_t i = 0; i < kSha256Size; ++i) {
    expected_[i] = expected_sha256[i];
    digest_[i] = 0;
  }
}

void FileHasher::update(const std::uint8_t* data, std::size_t size) {
  seen_ += size;
  hash_.update(data, size);
}

FileHasher::Result FileHasher::finish() {
  hash_.finish(digest_);
  const bool size_ok = seen_ == expected_size_;
  const bool hash_ok = sha256Equal(digest_, expected_);
  if (!size_ok) return Result::kSizeMismatch;
  return hash_ok ? Result::kOk : Result::kHashMismatch;
}

const char* describe(FileHasher::Result result) {
  switch (result) {
    case FileHasher::Result::kOk: return "the size and the SHA-256 are the manifest's";
    case FileHasher::Result::kSizeMismatch: return "the file's size is not the manifest's";
    case FileHasher::Result::kHashMismatch: return "the file's SHA-256 is not the manifest's";
  }
  return "?";
}

// -- DescriptorScanner ------------------------------------------------------------------------

void DescriptorScanner::reset() {
  held_ = 0;
  base_ = 0;
  found_ = Found{};
}

// Every 4-byte-aligned file offset whose 96-byte window is wholly in window_ is tried once:
// scan() tries them, then keeps the last kDescriptorSize - 1 bytes, which are exactly the
// offsets not yet tried (their windows reach beyond what has arrived).
void DescriptorScanner::scan(bool final) {
  std::size_t p = 0;
  while (p + kDescriptorSize <= held_) {
    if ((base_ + p) % 4 == 0) {
      Fields fields;
      if (parse(window_ + p, kDescriptorSize, &fields)) {
        if (found_.count == 0) {
          found_.offset = static_cast<std::size_t>(base_ + p);
          found_.fields = fields;
        }
        ++found_.count;
      }
    }
    ++p;
  }
  if (final) return;
  // Keep what could still begin a descriptor: from the first offset not tried.
  const std::size_t keep = held_ - p;
  std::memmove(window_, window_ + p, keep);
  base_ += p;
  held_ = keep;
}

void DescriptorScanner::update(const std::uint8_t* data, std::size_t size) {
  while (size > 0) {
    const std::size_t room = kWindow - held_;
    const std::size_t take = size < room ? size : room;
    std::memcpy(window_ + held_, data, take);
    held_ += take;
    data += take;
    size -= take;
    if (held_ == kWindow) scan(false);
  }
}

Found DescriptorScanner::result() const {
  // The bytes still held can hold at most one more candidate window (held_ < kWindow). Tried on
  // a copy so that result() may be asked more than once.
  DescriptorScanner tail = *this;
  tail.scan(true);
  return tail.found_;
}

// -- the descriptor against the entry ---------------------------------------------------------

const char* describe(DescriptorMismatch mismatch) {
  switch (mismatch) {
    case DescriptorMismatch::kNone: return "the descriptor agrees with the manifest entry";
    case DescriptorMismatch::kRole: return "the descriptor's role is not the manifest entry's";
    case DescriptorMismatch::kBoard: return "the descriptor's board is not the manifest entry's";
    case DescriptorMismatch::kBand: return "the descriptor's band is not the manifest entry's";
    case DescriptorMismatch::kVersion: return "the descriptor's version is not the manifest's";
    case DescriptorMismatch::kLinkMajor: return "the descriptor's link major is not the manifest's";
    case DescriptorMismatch::kSlot: return "the image is for another radio position than this slot";
  }
  return "?";
}

DescriptorMismatch descriptorAgrees(const Fields& descriptor, const Manifest& manifest,
                                    const ImageEntry& image, board::Slot slot) {
  const char* role = role_name(descriptor.role);
  if (role == nullptr || std::strcmp(role, image.role) != 0) return DescriptorMismatch::kRole;
  if (std::strcmp(descriptor.board, image.board) != 0) return DescriptorMismatch::kBoard;
  if (std::strcmp(descriptor.band, image.band) != 0) return DescriptorMismatch::kBand;
  if (std::strcmp(descriptor.version, manifest.version) != 0) return DescriptorMismatch::kVersion;
  if (descriptor.link_major != manifest.link_major) return DescriptorMismatch::kLinkMajor;
  if (descriptor.slot != kAnySlot && descriptor.slot != slot.index) return DescriptorMismatch::kSlot;
  return DescriptorMismatch::kNone;
}

}  // namespace image
}  // namespace core
