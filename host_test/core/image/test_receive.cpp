// Loading an image over USB (task p2-02): the framed transfer of core/image/receive.h, against a
// fake console - a laptop that builds frames with the link's own codec and reads the answers out
// of a string. The six checks run on the Walter, so a laptop that sends a file the manifest does
// not describe gets a refusal and the slot stays as it was.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/image/receive.h"
#include "core/link/codec/frame.h"
#include "core/link/codec/packet.h"
#include "fakes/capture.h"
#include "store_bench.h"

using bench::BaseFile;
using bench::Candidate;
using bench::Slot;
using bench::StoreBench;
using bench::StoreError;
using bench::Which;
using core::image::Receiver;
using core::image::Record;
namespace transfer = core::image::transfer;

namespace {

// What a response says.
struct Reply {
  bool got = false;
  std::uint8_t type = 0;
  std::uint8_t seq = 0;
  std::uint8_t status = 0xFF;
  std::uint8_t next_seq = 0;
  std::uint32_t committed = 0;
  std::string text;
};

// The laptop at the other end of the port: the store's bench, a receiver, and the bytes that
// came back.
struct Laptop {
  StoreBench b;
  fakes::CaptureOut out;
  Receiver receiver{b.store, b.verdict, out, b.clock};
  core::link::FrameReceiver rx;

  bool begin(Which which, Slot slot = Slot{0}) { return receiver.begin(which, slot); }

  // One frame in, the answers out. More than one answer never happens (one request, one
  // response), so the last one is the one.
  Reply send(std::uint8_t type, std::uint8_t seq, const std::uint8_t* payload, std::size_t size) {
    out.clear();
    std::uint8_t frame[core::link::kMaxWrite];
    const std::size_t wrote = core::link::buildFrame(
        type, seq, core::link::bytes(payload, size), frame, sizeof frame);
    REQUIRE(wrote > 0);
    receiver.feed(frame, wrote);
    return lastReply();
  }

  Reply send(std::uint8_t type, std::uint8_t seq, const std::vector<std::uint8_t>& payload = {}) {
    return send(type, seq, payload.data(), payload.size());
  }

  // The bytes the Walter wrote, decoded as frames; whatever is not a frame is ignored, as the
  // laptop must ignore the console's own text.
  Reply lastReply() {
    Reply reply;
    const std::string& text = out.text;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const std::uint8_t byte = static_cast<std::uint8_t>(text[i]);
      if (rx.push(byte) != core::link::FrameReceiver::Fed::kPacket) continue;
      const core::link::Packet& packet = rx.packet();
      reply.got = true;
      reply.type = packet.type;
      reply.seq = packet.seq;
      REQUIRE(packet.payload.size >= 6);
      reply.status = packet.payload.data[0];
      reply.next_seq = packet.payload.data[1];
      reply.committed = static_cast<std::uint32_t>(packet.payload.data[2]) |
                        (static_cast<std::uint32_t>(packet.payload.data[3]) << 8) |
                        (static_cast<std::uint32_t>(packet.payload.data[4]) << 16) |
                        (static_cast<std::uint32_t>(packet.payload.data[5]) << 24);
      reply.text.assign(reinterpret_cast<const char*>(packet.payload.data) + 6,
                        packet.payload.size - 6);
    }
    return reply;
  }

  // A kFile frame's payload: the kind and the size.
  static std::vector<std::uint8_t> fileHeader(std::uint8_t kind, std::uint32_t size) {
    return {kind,
            static_cast<std::uint8_t>(size),
            static_cast<std::uint8_t>(size >> 8),
            static_cast<std::uint8_t>(size >> 16),
            static_cast<std::uint8_t>(size >> 24)};
  }

  // One of the three files, in chunks of at most 240 bytes, acknowledged one at a time.
  void sendFile(std::uint8_t kind, const std::uint8_t* data, std::size_t size,
                std::uint32_t from = 0) {
    Reply reply = send(transfer::kFile, 0, fileHeader(kind, static_cast<std::uint32_t>(size)));
    REQUIRE(reply.got);
    REQUIRE_MESSAGE(reply.status == transfer::kOk, reply.text.c_str());
    std::uint8_t seq = 0;
    for (std::size_t at = from; at < size;) {
      const std::size_t n = size - at < 240 ? size - at : 240;
      reply = send(transfer::kData, seq, data + at, n);
      REQUIRE(reply.got);
      REQUIRE_MESSAGE(reply.status == transfer::kOk, reply.text.c_str());
      at += n;
      ++seq;
    }
    reply = send(transfer::kEnd, seq);
    REQUIRE(reply.got);
    REQUIRE_MESSAGE(reply.status == transfer::kOk, reply.text.c_str());
  }

  void sendFile(std::uint8_t kind, const std::vector<std::uint8_t>& data, std::uint32_t from = 0) {
    sendFile(kind, data.data(), data.size(), from);
  }

  void sendManifestAndSignature(const Candidate& image) {
    sendFile(transfer::kKindManifest, image.manifest);
    sendFile(transfer::kKindSignature,
             reinterpret_cast<const std::uint8_t*>(image.signature.data()),
             image.signature.size());
  }

  // The same three files for the board's base (task p5-01): the transfer does not know the
  // difference, which is the point of the base arriving this way.
  void sendManifestAndSignature(const BaseFile& base) {
    sendFile(transfer::kKindManifest, base.manifest);
    sendFile(transfer::kKindSignature,
             reinterpret_cast<const std::uint8_t*>(base.signature.data()),
             base.signature.size());
  }
};

}  // namespace

TEST_CASE("a whole transfer: the manifest, its signature, the image, and the record that follows") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));
  CHECK(laptop.receiver.active());
  CHECK(std::string(laptop.receiver.needs().role) == "repeater");
  CHECK(std::string(laptop.receiver.needs().board) == "rak4630-single");
  CHECK(std::string(laptop.receiver.needs().band) == core::image::kDefaultBand);

  Reply hello = laptop.send(transfer::kHello, 0);
  REQUIRE(hello.got);
  CHECK(hello.type == (transfer::kHello | transfer::kResponseBit));
  CHECK(hello.status == transfer::kOk);
  CHECK(hello.text.find("walter image receive 1") == 0);
  CHECK(hello.text.find("candidate") != std::string::npos);

  laptop.sendManifestAndSignature(a);
  laptop.sendFile(transfer::kKindImage, a.file);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE(done.got);
  REQUIRE_MESSAGE(done.status == transfer::kDone, done.text.c_str());
  CHECK(laptop.receiver.active() == false);
  CHECK(laptop.receiver.stored());
  CHECK(std::string(laptop.receiver.reason()) == "stored");

  const Record record = laptop.b.read(Which::kCandidate);
  REQUIRE(record.present);
  CHECK(record.size == a.file.size());
  CHECK(std::string(record.version) == a.version);
  CHECK(laptop.b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  CHECK(laptop.receiver.chunks() > 0);
  CHECK(laptop.receiver.duplicates() == 0);
  CHECK(laptop.receiver.outOfOrder() == 0);
  CHECK(laptop.receiver.droppedFrames() == 0);
}

TEST_CASE("the same transfer, for the board's base: checks 1 to 4 and 5p, and the base's own door") {
  // Task p5-01. `image receive base` hands the port to the same framed transfer; what differs is
  // the `use` the target asks the verdict for (`provision`), the check that judges the file (5p,
  // not 5 and 6) and the door the token opens (commitBase).
  Laptop laptop;
  const BaseFile base("base-small");
  REQUIRE(laptop.begin(Which::kBase));
  CHECK(laptop.receiver.target().use() == core::image::Use::kProvision);
  CHECK(std::string(laptop.receiver.target().name()) == "base");

  const Reply hello = laptop.send(transfer::kHello, 0);
  REQUIRE(hello.got);
  CHECK(hello.status == transfer::kOk);
  CHECK(hello.text.find("base") != std::string::npos);

  laptop.sendManifestAndSignature(base);
  laptop.sendFile(transfer::kKindImage, base.file);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE(done.got);
  REQUIRE_MESSAGE(done.status == transfer::kDone, done.text.c_str());
  CHECK(laptop.receiver.stored());

  const Record record = laptop.b.read(Which::kBase);
  REQUIRE(record.present);
  CHECK(record.use == core::image::Use::kProvision);
  CHECK(record.size == base.file.size());
  CHECK(record.base.bootloader == base.row->bootloader);
  CHECK(laptop.b.store.verify(Which::kBase, Slot{0}) == StoreError::kNone);
  // And the three images are where they were: empty.
  CHECK_FALSE(laptop.b.read(Which::kGolden).present);
  CHECK_FALSE(laptop.b.read(Which::kCandidate).present);
}

TEST_CASE("a base the file's own rules refuse: the transfer says check 5p, and nothing is stored") {
  // The bytes are in the Walter's flash and complete; §4a is what turns them away, with the
  // sentence the specification gives - and the slot's base stays as it was (empty).
  Laptop laptop;
  const BaseFile base("base-in-the-manifest-region");
  REQUIRE(laptop.begin(Which::kBase));
  laptop.sendManifestAndSignature(base);
  laptop.sendFile(transfer::kKindImage, base.file);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE(done.got);
  CHECK(done.status == transfer::kRefused);
  CHECK(done.text.find("check 5p, the base") == 0);
  CHECK(done.text.find("UICR.NRFFW[0] = 0x000D4000") != std::string::npos);
  CHECK_FALSE(laptop.receiver.stored());
  CHECK_FALSE(laptop.b.read(Which::kBase).present);
}

TEST_CASE("a lost chunk, a duplicated chunk and a reordered one") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendManifestAndSignature(a);
  Reply reply = laptop.send(transfer::kFile, 0,
                            Laptop::fileHeader(transfer::kKindImage,
                                               static_cast<std::uint32_t>(a.file.size())));
  REQUIRE(reply.status == transfer::kOk);
  CHECK(reply.committed == 0);

  // Chunk 0 arrives.
  reply = laptop.send(transfer::kData, 0, a.file.data(), 240);
  CHECK(reply.status == transfer::kOk);
  CHECK(reply.next_seq == 1);

  // Chunk 1 is lost on the way: chunk 2 arrives instead, and is refused with the one wanted.
  reply = laptop.send(transfer::kData, 2, a.file.data() + 480, 240);
  CHECK(reply.status == transfer::kResend);
  CHECK(reply.next_seq == 1);
  CHECK(laptop.receiver.outOfOrder() == 1);

  // The laptop sends chunk 1 again, and this time it arrives.
  reply = laptop.send(transfer::kData, 1, a.file.data() + 240, 240);
  CHECK(reply.status == transfer::kOk);
  CHECK(reply.next_seq == 2);

  // And once more: the acknowledgement was lost, not the chunk. It is acknowledged again and
  // written once - the bytes do not go into the image twice.
  reply = laptop.send(transfer::kData, 1, a.file.data() + 240, 240);
  CHECK(reply.status == transfer::kOk);
  CHECK(reply.next_seq == 2);
  CHECK(laptop.receiver.duplicates() == 1);
  CHECK(reply.committed == 0);  // still inside the first sector

  // The rest, and the image is whole.
  std::uint8_t seq = 2;
  for (std::size_t at = 480; at < a.file.size();) {
    const std::size_t n = a.file.size() - at < 240 ? a.file.size() - at : 240;
    reply = laptop.send(transfer::kData, seq, a.file.data() + at, n);
    REQUIRE(reply.status == transfer::kOk);
    at += n;
    ++seq;
  }
  reply = laptop.send(transfer::kEnd, seq);
  CHECK(reply.status == transfer::kOk);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE_MESSAGE(done.status == transfer::kDone, done.text.c_str());
  CHECK(laptop.b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
}

TEST_CASE("a frame with a bad checksum is dropped and counted, and bytes that are not frames are ignored") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));

  // The console's own prompt and a line of text reach the laptop before the first frame; and a
  // frame whose CRC is wrong reaches the Walter. Neither is acted on.
  std::uint8_t frame[core::link::kMaxWrite];
  const std::vector<std::uint8_t> payload = Laptop::fileHeader(transfer::kKindManifest, 10);
  const std::size_t wrote = core::link::buildFrame(transfer::kFile, 0,
                                                   core::link::bytes(payload.data(), payload.size()),
                                                   frame, sizeof frame);
  REQUIRE(wrote > 4);
  frame[2] ^= 0x40;  // one bit of the payload, after the CRC was computed
  laptop.out.clear();
  laptop.receiver.feed(frame, wrote);
  CHECK(laptop.receiver.droppedFrames() >= 1);
  CHECK(laptop.out.text.empty());  // nothing was answered: the sender's timeout is the recovery
  CHECK(laptop.receiver.active());

  // The frame again, correct this time.
  const Reply reply = laptop.send(transfer::kFile, 0, payload);
  CHECK(reply.status == transfer::kOk);
}

TEST_CASE("a manifest larger than this firmware reads is refused before it is buffered") {
  Laptop laptop;
  REQUIRE(laptop.begin(Which::kCandidate));
  const Reply reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindManifest, core::image::transfer::kMaxManifestBytes + 1));
  REQUIRE(reply.got);
  CHECK(reply.status == transfer::kRefused);
  CHECK(reply.text.find("larger than this firmware reads") != std::string::npos);
  CHECK(laptop.receiver.active() == false);
  CHECK(laptop.receiver.stored() == false);

  // And one that announces a size it then exceeds.
  Laptop other;
  REQUIRE(other.begin(Which::kCandidate));
  Reply begun = other.send(transfer::kFile, 0, Laptop::fileHeader(transfer::kKindManifest, 300));
  CHECK(begun.status == transfer::kOk);
  std::vector<std::uint8_t> block(240, 'x');
  CHECK(other.send(transfer::kData, 0, block).status == transfer::kOk);
  const Reply over = other.send(transfer::kData, 1, block);
  CHECK(over.status == transfer::kRefused);
  CHECK(over.text.find("than was announced") != std::string::npos);
  CHECK(other.receiver.active() == false);
}

TEST_CASE("a file whose hash is not its manifest's is refused at check 4, with nothing stored") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendManifestAndSignature(a);

  // The right length, one bit different: the laptop is not trusted, and the bytes are hashed
  // where they land.
  std::vector<std::uint8_t> tampered = a.file;
  tampered[1234] ^= 0x10;
  laptop.sendFile(transfer::kKindImage, tampered);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE(done.got);
  CHECK(done.status == transfer::kRefused);
  CHECK(done.text.find("check 4") == 0);
  CHECK(done.text.find("SHA-256") != std::string::npos);
  CHECK(laptop.receiver.stored() == false);
  CHECK(laptop.b.read(Which::kCandidate).present == false);
  CHECK(laptop.b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kEmpty);
}

TEST_CASE("a manifest nobody we trust signed is refused at check 1, before a sector is erased") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendFile(transfer::kKindManifest, a.manifest);
  // The signature of another manifest: a real Ed25519 signature by the right key, over the
  // wrong bytes.
  const Candidate other("store-b");
  laptop.sendFile(transfer::kKindSignature,
                  reinterpret_cast<const std::uint8_t*>(other.signature.data()),
                  other.signature.size());
  const std::uint32_t before = laptop.b.flash.operations();
  const Reply reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(a.file.size())));
  REQUIRE(reply.got);
  CHECK(reply.status == transfer::kRefused);
  CHECK(reply.text.find("check 1") == 0);
  CHECK(laptop.b.flash.operations() == before);  // not one erase
  CHECK(laptop.receiver.active() == false);
}

TEST_CASE("the store's own refusals come back as refusals of the transfer") {
  // A golden image that is already there (S-5: golden is reached by a promotion).
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.b.put(Which::kGolden, Slot{0}, a) == StoreError::kNone);
  REQUIRE(laptop.begin(Which::kGolden));
  laptop.sendManifestAndSignature(a);
  const Reply reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(a.file.size())));
  CHECK(reply.status == transfer::kRefused);
  CHECK(reply.text.find("promotion") != std::string::npos);
  CHECK(laptop.b.store.verify(Which::kGolden, Slot{0}) == StoreError::kNone);

  // And an image on the bad list.
  Laptop bad;
  REQUIRE(bad.b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  const Record record = bad.b.read(Which::kCandidate);
  REQUIRE(record.present);
  REQUIRE(bad.b.store.markBad(record.sha256) == StoreError::kNone);
  REQUIRE(bad.b.store.erase(Which::kCandidate, Slot{0}) == StoreError::kNone);
  REQUIRE(bad.begin(Which::kCandidate));
  bad.sendManifestAndSignature(a);
  const Reply refused = bad.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(a.file.size())));
  CHECK(refused.status == transfer::kRefused);
  CHECK(refused.text.find("never tried again") != std::string::npos);
}

TEST_CASE("the three files have to come in order, and a scanner image goes in the scanner slot") {
  Laptop laptop;
  const Candidate scanner("store-scanner");
  REQUIRE(laptop.begin(Which::kScanner));
  CHECK(std::string(laptop.receiver.needs().role) == "scanner");
  // The image before the manifest is not the order.
  const Reply early = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(scanner.file.size())));
  CHECK(early.status == transfer::kRefused);
  CHECK(early.text.find("does not come next") != std::string::npos);

  // In order, it works.
  Laptop again;
  REQUIRE(again.begin(Which::kScanner));
  again.sendManifestAndSignature(scanner);
  again.sendFile(transfer::kKindImage, scanner.file);
  const Reply done = again.send(transfer::kCommit, 0);
  REQUIRE_MESSAGE(done.status == transfer::kDone, done.text.c_str());
  CHECK(again.b.read(Which::kScanner).descriptor.role == core::image::Role::kScanner);
}

TEST_CASE("a transfer that is abandoned, and one whose other end goes quiet") {
  Laptop laptop;
  const Candidate a("store-a");
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendManifestAndSignature(a);
  Reply reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(a.file.size())));
  REQUIRE(reply.status == transfer::kOk);
  reply = laptop.send(transfer::kData, 0, a.file.data(), 240);
  REQUIRE(reply.status == transfer::kOk);
  const Reply aborted = laptop.send(transfer::kAbort, 0);
  CHECK(aborted.status == transfer::kOk);
  CHECK(laptop.receiver.active() == false);
  CHECK(laptop.receiver.stored() == false);
  CHECK(laptop.b.read(Which::kCandidate).present == false);
  CHECK(laptop.b.store.writing() == false);

  // The cable comes out in the middle: the transfer ends by itself so that the console is not
  // mute for ever.
  Laptop quiet;
  REQUIRE(quiet.begin(Which::kCandidate));
  quiet.sendManifestAndSignature(a);
  quiet.receiver.tick();
  CHECK(quiet.receiver.active());
  quiet.b.clock.advance(core::image::transfer::kIdleTimeoutMs - 1);
  quiet.receiver.tick();
  CHECK(quiet.receiver.active());
  quiet.b.clock.advance(2);
  quiet.receiver.tick();
  CHECK(quiet.receiver.active() == false);
  CHECK(std::string(quiet.receiver.reason()).find("went quiet") != std::string::npos);
  CHECK(quiet.b.store.writing() == false);
}

TEST_CASE("a transfer resumes after a reset: the Walter says how far it got and the rest is sent") {
  Laptop laptop;
  const Candidate big("store-resume");
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendManifestAndSignature(big);
  Reply reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(big.file.size())));
  REQUIRE(reply.status == transfer::kOk);
  CHECK(reply.committed == 0);

  // Two thirds of the way, then the Walter resets.
  std::uint8_t seq = 0;
  const std::size_t stop = big.file.size() * 2 / 3;
  for (std::size_t at = 0; at < stop;) {
    const std::size_t n = stop - at < 240 ? stop - at : 240;
    reply = laptop.send(transfer::kData, seq, big.file.data() + at, n);
    REQUIRE(reply.status == transfer::kOk);
    at += n;
    ++seq;
  }
  // The acknowledgements say how many bytes are in flash; the point a *reset* can resume from is
  // the last progress record, which goes down every 64 KB - so the resume point is at or below
  // this, and the Walter is the one that says where it is.
  const std::uint32_t in_flash = reply.committed;
  CHECK(in_flash >= core::image::kResumeStep);

  // The cable comes out: the transfer gives up by itself, and then the Walter resets.
  laptop.b.clock.advance(core::image::transfer::kIdleTimeoutMs + 1);
  laptop.receiver.tick();
  REQUIRE(laptop.receiver.active() == false);
  laptop.b.reboot();

  // The laptop asks again. The Walter answers with how many bytes it has, and the rest is sent -
  // not the whole file a second time.
  REQUIRE(laptop.begin(Which::kCandidate));
  laptop.sendManifestAndSignature(big);
  reply = laptop.send(
      transfer::kFile, 0,
      Laptop::fileHeader(transfer::kKindImage, static_cast<std::uint32_t>(big.file.size())));
  REQUIRE_MESSAGE(reply.status == transfer::kOk, reply.text.c_str());
  const std::uint32_t resume_at = reply.committed;
  CHECK(resume_at > 0);
  CHECK(resume_at <= in_flash);
  CHECK(resume_at % core::image::kResumeStep == 0);

  seq = 0;
  std::size_t sent = 0;
  for (std::size_t at = resume_at; at < big.file.size();) {
    const std::size_t n = big.file.size() - at < 240 ? big.file.size() - at : 240;
    reply = laptop.send(transfer::kData, seq, big.file.data() + at, n);
    REQUIRE(reply.status == transfer::kOk);
    at += n;
    sent += n;
    ++seq;
  }
  CHECK(sent == big.file.size() - resume_at);
  reply = laptop.send(transfer::kEnd, seq);
  REQUIRE(reply.status == transfer::kOk);
  const Reply done = laptop.send(transfer::kCommit, 0);
  REQUIRE_MESSAGE(done.status == transfer::kDone, done.text.c_str());
  CHECK(laptop.b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);
  MESSAGE("a transfer of " << big.file.size() << " bytes was interrupted with " << in_flash
                           << " bytes in flash; it resumed at " << resume_at
                           << " (the last progress record) and sent the remaining " << sent);
}

TEST_CASE("a commit before the three files are in, and a type this firmware does not know") {
  Laptop laptop;
  REQUIRE(laptop.begin(Which::kCandidate));
  const Reply early = laptop.send(transfer::kCommit, 0);
  CHECK(early.status == transfer::kRefused);
  CHECK(early.text.find("not all in") != std::string::npos);

  Laptop other;
  REQUIRE(other.begin(Which::kCandidate));
  const Reply unknown = other.send(0x7F, 0);
  CHECK(unknown.got);
  CHECK(unknown.status == transfer::kResend);
  CHECK(other.receiver.active());  // the transfer stands where it was
}

TEST_CASE("a transfer cannot be started while one is running, nor for a slot with no radio") {
  Laptop laptop;
  REQUIRE(laptop.begin(Which::kCandidate));
  CHECK(laptop.begin(Which::kGolden) == false);
  CHECK(std::string(laptop.receiver.reason()).find("already running") != std::string::npos);

  Laptop other;
  CHECK(other.begin(Which::kCandidate, Slot{1}) == false);
  CHECK(std::string(other.receiver.reason()).find("no radio in that slot") != std::string::npos);
}
