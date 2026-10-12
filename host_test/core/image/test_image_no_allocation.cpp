// No allocation in the parser or the verifier (task p2-01's third acceptance criterion), as a
// test: every source under core/image/ - the manifest, the signature, the trust table, the
// choice, the file checks, the verdict, SHA-256/512 and Ed25519 - and the two vendored files
// under core/third_party/, read line by line for a word that allocates or an include that could
// bring one in. The vendored tokenizer and the TweetNaCl extract work in arrays the caller
// provides, which is why they were chosen; this is where that stays true.
#include <doctest.h>

#include <cstddef>

#include "checks/no_allocation.h"

namespace {

const char* const kImageDirectories[] = {
    "components/core/image",
    "components/core/image/crypto",
    "components/core/include/core/image",
    "components/core/include/core/image/crypto",
    "components/core/third_party/jsmn",
    "components/core/third_party/tweetnacl",
};

// What core/image/ may include: the freestanding headers, <cstring> (memcpy, strcmp, strchr -
// no allocation in any of them), its own headers, the board profile and the image source of the
// flasher, the slot, the version and the link major, the interfaces the store needs of the world
// (a flash region, a clock, the time of day, bytes out - all of them pure virtual), the link's
// framing, which the USB transfer reuses rather than inventing one, and the two vendored files.
const char* const kImageIncludes[] = {
    "cstddef",
    "cstdint",
    "cstring",
    "stddef.h",  // jsmn.h
    "core/board/slot.h",
    "core/flasher/board_profile.h",
    "core/flasher/image_source.h",
    "core/image/choose.h",
    "core/image/crypto/ed25519.h",
    "core/image/crypto/sha256.h",
    "core/image/crypto/sha512.h",
    "core/image/descriptor.h",
    "core/image/file_check.h",
    "core/image/manifest.h",
    "core/image/signature.h",
    "core/image/receive.h",
    "core/image/store.h",
    "core/image/this_image.h",
    "core/image/trust.h",
    "core/image/verdict.h",
    "core/link/codec/frame.h",
    "core/link/codec/packet.h",
    "core/version.h",
    "interfaces/clock.h",
    "interfaces/flash_region.h",
    "interfaces/text_out.h",
    "interfaces/wall_clock.h",
    "jsmn/jsmn.h",
    "tweetnacl/tweetnacl_verify.h",
    "trust_keys.h",  // generated from keys/release-keys.txt (tools/gen_trust_table.py)
};

}  // namespace

TEST_CASE("nothing under components/core/image/ or the vendored crypto and tokenizer allocates") {
  // Twenty-odd files: the descriptor and SHA-256 of earlier tasks, task p2-01's nine, the
  // store's and the transfer's four, the two vendored ones.
  checks::checkNoAllocation("the image checks",
                            checks::filesUnder(kImageDirectories,
                                               sizeof kImageDirectories / sizeof kImageDirectories[0]),
                            kImageIncludes, sizeof kImageIncludes / sizeof kImageIncludes[0], 24, 3000);
}
