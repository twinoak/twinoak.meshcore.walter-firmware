// The file's checks (task p2-01), as the bytes arrive in pieces - from a download, a USB transfer
// or a flash partition read a page at a time: spec/FIRMWARE-MANIFEST.md §5 check 4 (size and
// SHA-256) and check 5 (exactly one descriptor, agreeing with the manifest entry). Both streams
// are fed the same pieces in the same pass; neither needs the file in memory.
//
// The descriptor finder of descriptor.h works over a whole buffer; DescriptorScanner is the same
// search over a stream, held against it in host_test/core/image/test_file_check.cpp for every
// fixture image and every chunk size.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/image/crypto/sha256.h"
#include "core/image/descriptor.h"
#include "core/image/manifest.h"

namespace core {
namespace image {

// Check 4: the bytes counted and hashed as they pass.
class FileHasher {
 public:
  void begin(std::uint32_t expected_size, const std::uint8_t expected_sha256[kSha256Size]);
  void update(const std::uint8_t* data, std::size_t size);
  enum class Result : std::uint8_t { kOk, kSizeMismatch, kHashMismatch };
  // Ends the stream (the hasher is then reset) and says whether the size and the digest are the
  // entry's; the digest itself is left in `digest`.
  Result finish();
  const std::uint8_t* digest() const { return digest_; }
  std::uint64_t seen() const { return seen_; }

 private:
  Sha256 hash_;
  std::uint64_t seen_ = 0;
  std::uint32_t expected_size_ = 0;
  std::uint8_t expected_[kSha256Size] = {};
  std::uint8_t digest_[kSha256Size] = {};
};

const char* describe(FileHasher::Result result);

// Check 5, first half: the descriptors in a stream, counted; the first one's offset and fields
// kept. Same rules as find(): a valid descriptor on a 4-byte boundary, every one counted.
class DescriptorScanner {
 public:
  DescriptorScanner() { reset(); }
  void reset();
  void update(const std::uint8_t* data, std::size_t size);
  // After the last update(): what find() would have returned over the whole file.
  Found result() const;

 private:
  void scan(bool final);

  // Twice a descriptor: at least one whole candidate window plus the bytes a chunk boundary
  // leaves hanging.
  static constexpr std::size_t kWindow = 2 * kDescriptorSize;
  std::uint8_t window_[kWindow];
  std::size_t held_ = 0;      // bytes in window_
  std::uint64_t base_ = 0;    // the file offset of window_[0]
  Found found_;
};

// Check 5, second half: the descriptor inside the file against the entry it was downloaded under
// (§5: "it agrees with the manifest entry") and the slot it is for (§4: `slot` 0xFF is any,
// otherwise the only radio position this image is for).
enum class DescriptorMismatch : std::uint8_t {
  kNone,
  kRole,        // the descriptor's role byte is not the entry's `role`
  kBoard,
  kBand,
  kVersion,     // not the manifest's `version`
  kLinkMajor,   // not the manifest's `link.major`
  kSlot,        // the image is for another radio position
};

const char* describe(DescriptorMismatch mismatch);

DescriptorMismatch descriptorAgrees(const Fields& descriptor, const Manifest& manifest,
                                    const ImageEntry& image, board::Slot slot);

}  // namespace image
}  // namespace core
