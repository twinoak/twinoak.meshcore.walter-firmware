#include "core/image/receive.h"

#include <cstring>

#include "core/link/codec/packet.h"

namespace core {
namespace image {

namespace {

// What each of the three images is for, as §5 check 3's three words. The board is the build's
// own profile for the slot (a manifest cannot name a board this Walter has no region for), the
// role follows the image: golden and candidate are the radio's working firmware, the scanner
// slot holds NoiseScope. The band is the caller's.
//
// The base's image is the repeater's: the base is listed under the image it goes under
// (spec/FIRMWARE-MANIFEST.md §4a), and a release carries one per board, not one per role.
const char* roleFor(Which which) {
  return which == Which::kScanner ? "scanner" : "repeater";
}

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// A StoreError as the transfer's sentence: nullptr for "none".
const char* said(StoreError error) { return error == StoreError::kNone ? nullptr : describe(error); }

}  // namespace

// ------------------------------------------------------------------------- the store as a target

bool StoreTarget::begin(Which which, board::Slot slot) {
  const flasher::BoardProfile* profile = flasher::profileForSlot(slot);
  if (profile == nullptr) {
    reason_ = "this adapter has no radio in that slot";
    return false;
  }
  which_ = which;
  slot_ = slot;

  // The band: what the slot already holds, if it holds anything, and the build's default if not.
  // A copy, because needs_ keeps a pointer and a Record does not outlive this function.
  std::memcpy(band_, kDefaultBand, std::strlen(kDefaultBand) + 1);
  Record record;
  if (store_.read(Which::kGolden, slot, &record) == StoreError::kNone && record.present) {
    std::memcpy(band_, record.descriptor.band, kBandSize);
  } else if (store_.read(Which::kCandidate, slot, &record) == StoreError::kNone &&
             record.present) {
    std::memcpy(band_, record.descriptor.band, kBandSize);
  }
  band_[kBandSize - 1] = 0;

  needs_ = SlotNeeds{};
  needs_.slot = slot;
  needs_.role = roleFor(which);
  needs_.board = profile->name;
  needs_.band = band_;
  reason_ = "";
  return true;
}

const char* StoreTarget::beginWrite(const ExpectedFile& expected) {
  return said(store_.beginWrite(which_, slot_, expected));
}

const char* StoreTarget::write(const std::uint8_t* data, std::uint32_t size) {
  return said(store_.write(data, size));
}

const char* StoreTarget::endWrite() { return said(store_.endWrite()); }

const flasher::ImageSource* StoreTarget::staged(const char** why) {
  const StoreError opened = store_.openStaged(&staged_);
  if (opened != StoreError::kNone) {
    *why = describe(opened);
    return nullptr;
  }
  return &staged_;
}

const char* StoreTarget::commit(const AcceptedImage& token) { return said(store_.commit(token)); }

const char* StoreTarget::commit(const AcceptedBase& token) {
  return said(store_.commitBase(token));
}

// ------------------------------------------------------------------------------- the receiver

bool Receiver::begin(Which which, board::Slot slot) {
  if (active()) {
    reason_ = "a transfer is already running";
    return false;
  }
  if (!store_target_.begin(which, slot)) {
    reason_ = store_target_.reason();
    return false;
  }
  target_ = &store_target_;
  return start();
}

bool Receiver::begin(TransferTarget& target) {
  if (active()) {
    reason_ = "a transfer is already running";
    return false;
  }
  target_ = &target;
  return start();
}

bool Receiver::start() {
  stored_ = false;
  manifest_size_ = 0;
  manifest_promised_ = 0;
  manifest_done_ = false;
  signature_size_ = 0;
  signature_promised_ = 0;
  signature_done_ = false;
  image_promised_ = 0;
  expect_seq_ = 0;
  have_expect_ = false;
  chunks_ = 0;
  duplicates_ = 0;
  out_of_order_ = 0;
  rx_.discardPartial();
  reason_ = "";
  more_ = nullptr;
  last_ms_ = clock_.nowMs();
  phase_ = Phase::kManifest;
  return true;
}

void Receiver::feed(const std::uint8_t* bytes, std::size_t count) {
  if (!active() || bytes == nullptr) return;
  last_ms_ = clock_.nowMs();
  for (std::size_t i = 0; i < count && active(); ++i) {
    if (rx_.push(bytes[i]) == link::FrameReceiver::Fed::kPacket) handle(rx_.packet());
  }
}

void Receiver::tick() {
  if (!active()) return;
  if (clock_.nowMs() - last_ms_ >= transfer::kIdleTimeoutMs) {
    if (target_->writing()) target_->abandonWrite();
    finish("the other end went quiet: nothing stored", nullptr, false);
  }
}

void Receiver::respond(std::uint8_t type, std::uint8_t seq, std::uint8_t status, const char* text,
                       const char* more) {
  std::uint8_t payload[link::kMaxPayload];
  payload[0] = status;
  payload[1] = expect_seq_;
  put32(payload + 2, target_->writing() ? target_->written() : 0);
  std::size_t size = 6;
  const char* parts[3] = {text, more != nullptr ? ": " : nullptr, more};
  for (std::size_t p = 0; p < 3; ++p) {
    if (parts[p] == nullptr) continue;
    std::size_t n = std::strlen(parts[p]);
    const std::size_t room = sizeof payload - size;
    if (n > room) n = room;
    std::memcpy(payload + size, parts[p], n);
    size += n;
  }
  const std::size_t wrote = link::buildFrame(static_cast<std::uint8_t>(type | transfer::kResponseBit),
                                             seq, link::bytes(payload, size), frame_, sizeof frame_);
  if (wrote == 0) return;
  // §2's one 0x00 in front of a frame: always, here. The console's port is not a link whose idle
  // time this object measures, and a leading terminator is an empty frame, which §2 says a
  // receiver ignores.
  const char zero = 0;
  out_.write(&zero, 1);
  out_.write(reinterpret_cast<const char*>(frame_), wrote);
}

void Receiver::refuse(std::uint8_t type, std::uint8_t seq, const char* text, const char* more) {
  if (target_->writing()) target_->abandonWrite();
  respond(type, seq, transfer::kRefused, text, more);
  finish(text, more, false);
}

void Receiver::finish(const char* why, const char* more, bool stored) {
  phase_ = Phase::kIdle;
  reason_ = why;
  more_ = more;
  stored_ = stored;
}

void Receiver::handle(const link::Packet& packet) {
  switch (packet.type) {
    case transfer::kHello:
      // What this is and what it is about to take, so that a laptop speaking another version of
      // this protocol is told rather than left to guess. walterctl holds the text to what it
      // knows and stops if it does not recognise it (tools/walterctl.py).
      respond(packet.type, packet.seq, transfer::kOk, "walter image receive 1", target_->name());
      return;
    case transfer::kFile: onFile(packet); return;
    case transfer::kData: onData(packet); return;
    case transfer::kEnd: onEnd(packet); return;
    case transfer::kCommit: onCommit(packet); return;
    case transfer::kAbort:
      if (target_->writing()) target_->abandonWrite();
      respond(packet.type, packet.seq, transfer::kOk, "aborted");
      finish("the other end aborted: nothing stored", nullptr, false);
      return;
    default:
      // A type this firmware does not know: said, and the transfer stands where it was. §5.1 of
      // the link protocol's rule - a value that selects what follows it is refused - applies.
      respond(packet.type, packet.seq, transfer::kResend, "unknown type");
      return;
  }
}

void Receiver::onFile(const link::Packet& packet) {
  if (packet.payload.size < 5) {
    refuse(packet.type, packet.seq, "a file frame is a kind and a size");
    return;
  }
  const std::uint8_t kind = packet.payload.data[0];
  const std::uint32_t size = get32(packet.payload.data + 1);
  expect_seq_ = 0;
  have_expect_ = true;

  if (kind == transfer::kKindManifest && phase_ == Phase::kManifest) {
    if (size > transfer::kMaxManifestBytes) {
      refuse(packet.type, packet.seq, "the manifest is larger than this firmware reads");
      return;
    }
    manifest_size_ = 0;
    manifest_promised_ = size;
    manifest_done_ = false;
    respond(packet.type, packet.seq, transfer::kOk, "manifest");
    return;
  }
  if (kind == transfer::kKindSignature && manifest_done_) {
    if (size > transfer::kMaxSignatureBytes) {
      refuse(packet.type, packet.seq, "a signature is one line (section 3)");
      return;
    }
    phase_ = Phase::kSignature;
    signature_size_ = 0;
    signature_promised_ = size;
    signature_done_ = false;
    respond(packet.type, packet.seq, transfer::kOk, "signature");
    return;
  }
  if (kind == transfer::kKindImage && manifest_done_ && signature_done_) {
    // The same file announced again: the first answer was lost, not the question. Saying how many
    // bytes are in flash and letting the chunks start over is the whole of what is needed - a
    // write that is already open is not reopened (the store would refuse a second one).
    if (target_->writing() && target_->expectedSize() == size) {
      phase_ = Phase::kImage;
      respond(packet.type, packet.seq, transfer::kOk, "image, already open");
      return;
    }
    // Here the six checks begin: the first three (and the half of the sixth that needs no file)
    // over the manifest and its signature, before one byte of the image is let near the flash.
    const Outcome outcome = verdict_.judgeManifest(
        manifest_, manifest_size_, signature_, signature_size_, target_->needs(), target_->use(),
        target_->profile());
    if (!outcome.ok()) {
      // The check's number and its sentence, the same words the console prints.
      refuse(packet.type, packet.seq, describe(outcome.failed), outcome.why);
      return;
    }
    const ExpectedFile* expected = verdict_.expected();
    if (expected == nullptr) {
      refuse(packet.type, packet.seq, "the manifest named no file");
      return;
    }
    if (expected->size() != size) {
      refuse(packet.type, packet.seq, "that is not the size the manifest gives for this file");
      return;
    }
    const char* opened = target_->beginWrite(*expected);
    if (opened != nullptr) {
      refuse(packet.type, packet.seq, opened);
      return;
    }
    phase_ = Phase::kImage;
    image_promised_ = size;
    respond(packet.type, packet.seq, transfer::kOk, "image");
    return;
  }
  refuse(packet.type, packet.seq, "that file does not come next");
}

void Receiver::onData(const link::Packet& packet) {
  if (!have_expect_) {
    refuse(packet.type, packet.seq, "no file has begun");
    return;
  }
  // A chunk that arrived before and was acknowledged: acknowledge it again and write nothing.
  if (packet.seq == static_cast<std::uint8_t>(expect_seq_ - 1)) {
    ++duplicates_;
    respond(packet.type, packet.seq, transfer::kOk, "again");
    return;
  }
  if (packet.seq != expect_seq_) {
    ++out_of_order_;
    respond(packet.type, packet.seq, transfer::kResend, "not that chunk");
    return;
  }

  const std::uint8_t* data = packet.payload.data;
  const std::size_t size = packet.payload.size;
  switch (phase_) {
    case Phase::kManifest:
      // More than was announced is refused as it arrives, not after the buffer has been filled:
      // what a sender says it is sending is the bound it is held to.
      if (manifest_size_ + size > manifest_promised_ ||
          manifest_size_ + size > transfer::kMaxManifestBytes) {
        refuse(packet.type, packet.seq, "more of the manifest than was announced");
        return;
      }
      std::memcpy(manifest_ + manifest_size_, data, size);
      manifest_size_ += size;
      break;
    case Phase::kSignature:
      if (signature_size_ + size > signature_promised_ ||
          signature_size_ + size > transfer::kMaxSignatureBytes) {
        refuse(packet.type, packet.seq, "more of the signature than was announced");
        return;
      }
      std::memcpy(signature_ + signature_size_, data, size);
      signature_size_ += size;
      break;
    case Phase::kImage: {
      const char* wrote = target_->write(data, static_cast<std::uint32_t>(size));
      if (wrote != nullptr) {
        refuse(packet.type, packet.seq, wrote);
        return;
      }
      break;
    }
    default:
      refuse(packet.type, packet.seq, "no file is open");
      return;
  }
  ++chunks_;
  ++expect_seq_;
  respond(packet.type, packet.seq, transfer::kOk, nullptr);
}

void Receiver::onEnd(const link::Packet& packet) {
  switch (phase_) {
    case Phase::kManifest:
      if (manifest_promised_ != manifest_size_) {
        refuse(packet.type, packet.seq, "the manifest is not the length it was announced as");
        return;
      }
      manifest_done_ = true;
      respond(packet.type, packet.seq, transfer::kOk, "manifest in");
      return;
    case Phase::kSignature:
      if (signature_promised_ != signature_size_) {
        refuse(packet.type, packet.seq, "the signature is not the length it was announced as");
        return;
      }
      signature_done_ = true;
      respond(packet.type, packet.seq, transfer::kOk, "signature in");
      return;
    case Phase::kImage: {
      if (!target_->writing()) {
        refuse(packet.type, packet.seq, "no image is being written");
        return;
      }
      // The file is complete: the tail goes to flash, and the target says so if fewer bytes
      // arrived than the manifest promised.
      const char* ended = target_->endWrite();
      if (ended != nullptr) {
        refuse(packet.type, packet.seq, "the image is not the length the manifest gives", ended);
        return;
      }
      if (target_->written() != image_promised_) {
        refuse(packet.type, packet.seq, "the image is not the length it was announced as");
        return;
      }
      phase_ = Phase::kJudged;
      respond(packet.type, packet.seq, transfer::kOk, "image in");
      return;
    }
    default:
      refuse(packet.type, packet.seq, "no file is open");
      return;
  }
}

void Receiver::onCommit(const link::Packet& packet) {
  if (phase_ != Phase::kJudged || !target_->writing()) {
    refuse(packet.type, packet.seq, "the three files are not all in");
    return;
  }
  // Checks 4, 5 and the rest of 6, over the bytes **as they are in the Walter's flash** - not
  // over what arrived on the wire. A file whose hash is not the manifest's is refused here with
  // nothing stored: the record is never written, so the slot stays as it was.
  const char* why = "";
  const flasher::ImageSource* staged = target_->staged(&why);
  if (staged == nullptr) {
    refuse(packet.type, packet.seq, why);
    return;
  }
  const Outcome outcome = verdict_.judgeFile(*staged);
  if (!outcome.ok()) {
    refuse(packet.type, packet.seq, describe(outcome.failed), outcome.why);
    return;
  }
  // One of the two tokens, never both: a `provision` file's is an AcceptedBase and an image's an
  // AcceptedImage, and each goes through the door that takes it.
  const AcceptedImage* token = verdict_.accepted();
  const AcceptedBase* base = verdict_.acceptedBase();
  if (token == nullptr && base == nullptr) {
    refuse(packet.type, packet.seq, "the verdict gave no token");
    return;
  }
  const char* committed = token != nullptr ? target_->commit(*token) : target_->commit(*base);
  if (committed != nullptr) {
    refuse(packet.type, packet.seq, committed);
    return;
  }
  respond(packet.type, packet.seq, transfer::kDone, "stored");
  finish("stored", nullptr, true);
}

}  // namespace image
}  // namespace core
