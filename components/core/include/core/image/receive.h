// Loading an image over USB (task p2-02): the bench's stand-in for a server, until phase 3 gives
// the Walter an uplink. `image receive <golden|candidate|scanner> [@slot]` on the console
// (docs/console.md) hands the port to this object, which takes three files - `manifest.json`,
// then its `.sig`, then the image - and runs all six checks of spec/FIRMWARE-MANIFEST.md §5 on
// the Walter. **The laptop is not trusted**: it delivers bytes and nothing more. tools/walterctl.py
// is the other end.
//
// The framing is the box link's (spec/LINK-PROTOCOL.md §2) and not a new one: COBS, a CRC-16, a
// type, a sequence number, one 0x00 ending a frame - core/link/codec/, byte for byte, with this
// transfer's own small set of type numbers below. The response convention is §3's too: a response
// is the request's type with bit 7 set, carrying the request's own `seq`.
//
//   type       payload                     what it is
//   ---------- --------------------------- ---------------------------------------------------
//   kHello     -                           "are you there": the answer says what this is
//   kFile      kind u8, size u32           the next file begins; data sequence numbers restart
//   kData      the bytes (at most 240)     one chunk, identified by the frame's seq
//   kEnd       -                           that file is complete
//   kCommit    -                           all three are in: judge and store
//   kAbort     -                           give up; nothing is stored
//
// Every response carries `status u8 | next_seq u8 | committed u32 | text`, where `committed` is
// how many bytes of the image are in the Walter's flash. In the answer to the image's **kFile**
// it is the point a transfer that an interruption left half done continues from - the store's
// last resume point, 64 KB apart (core/image/store.h) - so a laptop whose push was cut short
// asks again and sends the rest, rather than the whole file a second time. In the answer to a
// chunk it is progress, and nothing to resume from: the bytes between two resume points are in
// flash but nothing has written down that they are.
//
// A chunk that arrives twice is acknowledged again and written once; a chunk out of order is
// answered with kResend and the sequence number wanted; a chunk that is lost is the laptop's to
// send again when no acknowledgement comes. Nothing of this is new: it is the link's own shape.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/flasher/board_profile.h"
#include "core/image/store.h"
#include "core/image/verdict.h"
#include "core/link/codec/frame.h"
#include "interfaces/clock.h"
#include "interfaces/text_out.h"

namespace core {
namespace image {

namespace transfer {

// The types. Ours, not the link's: the framing is shared, the numbers are not.
constexpr std::uint8_t kHello = 0x01;
constexpr std::uint8_t kFile = 0x02;
constexpr std::uint8_t kData = 0x03;
constexpr std::uint8_t kEnd = 0x04;
constexpr std::uint8_t kCommit = 0x05;
constexpr std::uint8_t kAbort = 0x06;
constexpr std::uint8_t kResponseBit = 0x80;  // §3: a response is the request with bit 7 set

// Which of the three files a kFile begins.
constexpr std::uint8_t kKindManifest = 1;
constexpr std::uint8_t kKindSignature = 2;
constexpr std::uint8_t kKindImage = 3;

// What a response says.
constexpr std::uint8_t kOk = 0;
constexpr std::uint8_t kResend = 1;   // not the chunk wanted; next_seq says which is
constexpr std::uint8_t kRefused = 2;  // the transfer is over and nothing was stored
constexpr std::uint8_t kDone = 3;     // stored

// The protocol's own version, in the kHello response, so that a laptop with an older walterctl
// is told rather than left to guess.
constexpr std::uint8_t kVersion = 1;

// The manifest this firmware will read over the wire. The specification's own manifests are
// 1.0 to 1.6 KB; a manifest filled to the parser's bounds (eight images of six files) is about
// 7 KB of JSON, and the parser's token array is sized for it (core/image/manifest.h), so 8 KB is
// "anything this firmware could accept" and one byte more is refused before it is buffered.
constexpr std::size_t kMaxManifestBytes = 8192;

// §3: one line of ASCII, `ed25519 <16 hex> <88 characters of base64>` - 114 bytes with its
// newline. 128 leaves room for a CR LF and nothing else.
constexpr std::size_t kMaxSignatureBytes = 128;

// How long a transfer waits for the next frame before it gives up and hands the console back.
// Long enough for a laptop to read a 1 MB file off a slow disk, short enough that an unplugged
// cable does not leave the console mute.
constexpr std::uint32_t kIdleTimeoutMs = 20000;

}  // namespace transfer

// The band a slot is taken to hold when nothing in the store says otherwise (§2: `868`, `433`).
// The band is a property of the radio module, which this firmware cannot read: once a slot holds
// an image, its descriptor says which band that image was for and that is used instead. Until
// then this is the default, and the console prints the three words it will match before a
// transfer starts so that a mistake is visible rather than silent.
constexpr const char* kDefaultBand = "868";

class Receiver {
 public:
  Receiver(Store& store, ImageVerdict& verdict, interfaces::TextOut& out, interfaces::Clock& clock)
      : store_(store), verdict_(verdict), out_(out), clock_(clock) {}

  // Hands the port over. False, with reason(), when the slot has no radio in this build, when
  // the store refuses the role, or when a transfer is already running.
  bool begin(Which which, board::Slot slot);

  // Whether the console should keep giving bytes to this object.
  bool active() const { return phase_ != Phase::kIdle; }

  // Bytes from the port. Any chunking; a frame is acted on when its CRC has passed and never
  // before (§2).
  void feed(const std::uint8_t* bytes, std::size_t count);

  // Called when nothing arrives: ends a transfer whose other end has gone away.
  void tick();

  // What happened, in one sentence (two static parts, the second often empty), for the console to
  // print when active() goes false.
  const char* reason() const { return reason_; }
  const char* reasonMore() const { return more_ != nullptr ? more_ : ""; }
  bool stored() const { return stored_; }

  // Counters, for the console and the tests.
  std::uint32_t chunks() const { return chunks_; }
  std::uint32_t duplicates() const { return duplicates_; }
  std::uint32_t outOfOrder() const { return out_of_order_; }
  std::uint32_t droppedFrames() const { return rx_.dropped(); }

  // What this transfer will accept, for the console's ready line.
  const SlotNeeds& needs() const { return needs_; }

 private:
  enum class Phase : std::uint8_t {
    kIdle,       // not running
    kManifest,   // waiting for, or taking, manifest.json
    kSignature,  // ... its .sig
    kImage,      // ... the image, straight into the store
    kJudged,     // the image is in flash and complete; kCommit may come
  };

  void handle(const link::Packet& packet);
  void onFile(const link::Packet& packet);
  void onData(const link::Packet& packet);
  void onEnd(const link::Packet& packet);
  void onCommit(const link::Packet& packet);

  // The response's text is one or two static sentences - "check 4, the file's size and hash" and
  // "the file's SHA-256 is not the manifest's" - joined with ": ". Two parts and not a format
  // string because core/image/ holds no formatting and no allocation (the no-allocation test).
  void respond(std::uint8_t type, std::uint8_t seq, std::uint8_t status, const char* text,
               const char* more = nullptr);
  // A refusal, and the transfer is over: nothing is stored.
  void refuse(std::uint8_t type, std::uint8_t seq, const char* text, const char* more = nullptr);
  void finish(const char* why, const char* more, bool stored);

  Store& store_;
  ImageVerdict& verdict_;
  interfaces::TextOut& out_;
  interfaces::Clock& clock_;

  Phase phase_ = Phase::kIdle;
  Which which_ = Which::kCandidate;
  board::Slot slot_;
  SlotNeeds needs_;
  char band_[kBandSize] = {};  // the band needs_ points at: a copy, so it outlives a Record
  const char* reason_ = "";
  const char* more_ = nullptr;
  bool stored_ = false;
  std::uint64_t last_ms_ = 0;

  link::FrameReceiver rx_;
  std::uint8_t expect_seq_ = 0;
  bool have_expect_ = false;
  std::uint32_t chunks_ = 0;
  std::uint32_t duplicates_ = 0;
  std::uint32_t out_of_order_ = 0;

  std::uint8_t manifest_[transfer::kMaxManifestBytes];
  std::size_t manifest_size_ = 0;
  std::size_t manifest_promised_ = 0;
  bool manifest_done_ = false;
  char signature_[transfer::kMaxSignatureBytes];
  std::size_t signature_size_ = 0;
  std::size_t signature_promised_ = 0;
  bool signature_done_ = false;
  std::uint32_t image_promised_ = 0;

  std::uint8_t frame_[link::kMaxWrite];
};

}  // namespace image
}  // namespace core
