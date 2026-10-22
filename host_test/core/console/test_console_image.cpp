// The console's five `image` commands (task p2-02): list, verify, erase, bad, receive - and what
// they say when this build has no store at all (a partition table without `rakfw`). The transfer
// itself is host_test/core/image/test_receive.cpp's; here it is the handover of the port and the
// lines a person reads.
#include <doctest.h>

#include <string>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/console/parser.h"
#include "core/health/health.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/frame.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "fakes/fake_wall_clock.h"
#include "core/image/store_bench.h"
#include "sim/scripted_radio.h"

using bench::BaseFile;
using bench::Candidate;
using core::board::kKillTiming;
using core::board::RadioPins;
using core::board::Slot;
using core::console::Console;
using core::health::Health;
using core::health::ResetReason;
using core::health::WakeCause;
using core::image::StoreError;
using core::image::Which;

namespace {

struct Rig {
  bench::StoreBench b;
  fakes::FakePins pins{b.clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  RadioPins radio{pins, b.clock, retained};
  Health health{retained, b.clock};
  sim::ScriptedRadio rak{b.clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, b.clock, b.wall, slots};
  core::image::Receiver receiver{b.store, b.verdict, out, b.clock};
  Console console{out, radio, health, b.clock, restart, link, slots, &b.store, &receiver};

  Rig() {
    pins.record_log = false;
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    b.clock.advance(kKillTiming.settle_ms);
    radio.safe();
  }

  std::string run(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }
};

// The same console with no store: what a build whose partition table has no `rakfw` answers.
struct NoStoreRig {
  fakes::FakeClock clock;
  fakes::FakePins pins{clock};
  fakes::FakeRetained retained;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  fakes::FakeWallClock wall;
  RadioPins radio{pins, clock, retained};
  Health health{retained, clock};
  sim::ScriptedRadio rak{clock};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{radio, rak};
  core::link::client::Client link{port, clock, wall, slots};
  Console console{out, radio, health, clock, restart, link, slots};

  NoStoreRig() {
    pins.record_log = false;
    health.begin(ResetReason::kPowerOn, WakeCause::kNotFromSleep);
    clock.advance(kKillTiming.settle_ms);
    radio.safe();
  }

  std::string run(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }
};

bool has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

}  // namespace

TEST_CASE("image list on an empty store says the layout and that every slot is empty") {
  Rig rig;
  const std::string text = rig.run("image list");
  CHECK(has(text, "image store:"));
  CHECK(has(text, "2 slots x 3 images of 815104 bytes (199 sectors)"));
  CHECK(has(text, "plus one base of 655360 bytes (160 sectors) a slot"));
  CHECK(has(text, "6242304 bytes of a 6291456-byte partition, 49152 left"));
  CHECK(has(text, "slot 0, rak4630-single"));
  CHECK(has(text, "slot 1: no radio in this adapter"));
  CHECK(has(text, "golden     empty"));
  CHECK(has(text, "candidate  empty"));
  CHECK(has(text, "scanner    empty"));
  CHECK(has(text, "base       empty"));
}

TEST_CASE("image list, verify and erase over a store with images in it") {
  Rig rig;
  const Candidate a("store-a");
  const Candidate scanner("store-scanner");
  REQUIRE(rig.b.put(Which::kGolden, Slot{0}, a) == StoreError::kNone);
  REQUIRE(rig.b.put(Which::kScanner, Slot{0}, scanner) == StoreError::kNone);

  const std::string listed = rig.run("image list");
  CHECK(has(listed, "golden     repeater rak4630-single 868 0.3.0+storea"));
  CHECK(has(listed, "20480 bytes at 0x00026000"));
  CHECK(has(listed, "sha256 "));
  CHECK(has(listed, "stored when the Walter had no clock, from commit abc1234"));
  CHECK(has(listed, "signed by c17969deb0026955"));
  CHECK(has(listed, "scanner    scanner rak4630-single 868 0.3.0+scanner"));
  CHECK(has(listed, "candidate  empty"));

  // A clock, and the date is in the line.
  rig.b.wall.give(1760000000);
  REQUIRE(rig.b.put(Which::kCandidate, Slot{0}, a) == StoreError::kNone);
  CHECK(has(rig.run("image list"), "stored at 1760000000 (unix)"));

  const std::string verified = rig.run("image verify golden");
  CHECK(has(verified, "golden: 20480 bytes read back and hashed"));
  CHECK(has(verified, "the SHA-256 is the one in its record"));

  // Flash rot, and verify says so.
  const std::uint32_t data =
      core::image::areaOffset(0, rig.b.store.areaOf(Which::kGolden, Slot{0})) +
      core::image::kSectorSize;
  rig.b.flash.rot(data + 777, 0x20);
  const std::string rotted = rig.run("image verify golden");
  CHECK(has(rotted, "error: golden 0.3.0+storea"));
  CHECK(has(rotted, "not the hash the manifest names"));

  // Erase: never golden, and the parser says so before the store does.
  CHECK(has(rig.run("image erase golden"), "error: image erase: the golden image is not erased"));
  CHECK(has(rig.run("image erase scanner"), "scanner: forgotten in slot 0"));
  CHECK(has(rig.run("image list"), "scanner    empty"));
  CHECK(has(rig.run("image verify scanner"), "error: slot 0's scanner image: there is no image"));
  CHECK(has(rig.run("image erase candidate"), "candidate: forgotten in slot 0"));
}

TEST_CASE("image bad: nothing, then the hashes") {
  Rig rig;
  CHECK(has(rig.run("image bad"), "no image has failed a health check on this unit"));
  std::uint8_t hash[32];
  for (std::size_t i = 0; i < 32; ++i) hash[i] = static_cast<std::uint8_t>(i);
  REQUIRE(rig.b.store.markBad(hash) == StoreError::kNone);
  const std::string text = rig.run("image bad");
  CHECK(has(text, "image bad: 1 of at most 8 hashes"));
  CHECK(has(text, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
}

TEST_CASE("image receive hands the port over, says what it will accept, and gives it back") {
  Rig rig;
  const std::string text = rig.run("image receive candidate");
  CHECK(has(text, "this port is now a framed binary transfer"));
  CHECK(has(text, "tools/walterctl.py push --as candidate"));
  CHECK(has(text, "role repeater, board rak4630-single, band 868"));
  CHECK(rig.receiver.active());

  // From here the bytes are frames, not a typed line: nothing is echoed and no prompt appears.
  rig.out.clear();
  rig.console.feed('h');
  rig.console.feed('i');
  CHECK(rig.out.text.empty());

  // A whole transfer, framed, through the console's own feed().
  const Candidate a("store-a");
  std::uint8_t frame[core::link::kMaxWrite];
  auto send = [&](std::uint8_t type, std::uint8_t seq, const std::uint8_t* payload,
                  std::size_t size) {
    const std::size_t wrote = core::link::buildFrame(type, seq,
                                                     core::link::bytes(payload, size),
                                                     frame, sizeof frame);
    REQUIRE(wrote > 0);
    // Section 2's one 0x00 in front of a frame, which is what makes the two characters typed
    // above an empty frame and not the start of this one.
    rig.console.feed('\0');
    for (std::size_t i = 0; i < wrote; ++i) rig.console.feed(static_cast<char>(frame[i]));
  };
  auto sendFile = [&](std::uint8_t kind, const std::uint8_t* data, std::size_t size) {
    const std::uint8_t header[5] = {kind,
                                    static_cast<std::uint8_t>(size),
                                    static_cast<std::uint8_t>(size >> 8),
                                    static_cast<std::uint8_t>(size >> 16),
                                    static_cast<std::uint8_t>(size >> 24)};
    send(core::image::transfer::kFile, 0, header, sizeof header);
    std::uint8_t seq = 0;
    for (std::size_t at = 0; at < size;) {
      const std::size_t n = size - at < 240 ? size - at : 240;
      send(core::image::transfer::kData, seq, data + at, n);
      at += n;
      ++seq;
    }
    send(core::image::transfer::kEnd, seq, nullptr, 0);
  };
  sendFile(core::image::transfer::kKindManifest, a.manifest.data(), a.manifest.size());
  sendFile(core::image::transfer::kKindSignature,
           reinterpret_cast<const std::uint8_t*>(a.signature.data()), a.signature.size());
  sendFile(core::image::transfer::kKindImage, a.file.data(), a.file.size());
  rig.out.clear();
  send(core::image::transfer::kCommit, 0, nullptr, 0);

  // The console has the port back, and says what happened and then a prompt.
  CHECK(rig.receiver.active() == false);
  CHECK(has(rig.out.text, "image receive: stored"));
  CHECK(has(rig.out.text, "chunks,"));
  CHECK(has(rig.out.text, "'image list' shows it"));
  CHECK(rig.out.text.rfind("> ") == rig.out.text.size() - 2);
  CHECK(has(rig.run("image list"), "candidate  repeater rak4630-single 868 0.3.0+storea"));
  CHECK(rig.b.store.verify(Which::kCandidate, Slot{0}) == StoreError::kNone);

  // And the console is a console again.
  CHECK(has(rig.run("image bad"), "no image has failed"));
}

TEST_CASE("a transfer whose other end goes quiet gives the port back on a tick") {
  Rig rig;
  rig.run("image receive candidate");
  REQUIRE(rig.receiver.active());
  rig.out.clear();
  rig.console.tick();
  CHECK(rig.receiver.active());
  rig.b.clock.advance(core::image::transfer::kIdleTimeoutMs + 1);
  rig.console.tick();
  CHECK(rig.receiver.active() == false);
  CHECK(has(rig.out.text, "went quiet"));
}

TEST_CASE("image receive for a slot this adapter has no radio in") {
  Rig rig;
  CHECK(has(rig.run("image receive candidate @1"),
            "error: slot 1: this adapter has 1 radio, so slot 0 is the only one"));
  CHECK(rig.receiver.active() == false);
  CHECK(has(rig.run("image verify golden @1"), "error: slot 1:"));
  CHECK(has(rig.run("image erase candidate @1"), "error: slot 1:"));
  // `image list` is about the whole store, and says what slot 1 is.
  CHECK(has(rig.run("image list"), "slot 1: no radio in this adapter"));
}

TEST_CASE("a build with no image store says so in one line") {
  NoStoreRig rig;
  for (const char* line : {"image list", "image bad", "image verify golden",
                           "image erase candidate", "image receive candidate"}) {
    const std::string text = rig.run(line);
    CHECK_MESSAGE(has(text, "error: there is no image store in this build"), line);
    CHECK_MESSAGE(has(text, "rakfw partition of partitions.csv was"), line);
  }
}

TEST_CASE("the base in image list, image verify and image erase (task p5-01)") {
  Rig rig;
  const BaseFile base("base-small");
  REQUIRE(rig.b.putBase(Slot{0}, base) == StoreError::kNone);

  SUBCASE("image list shows it: the hash, the size, the bootloader and the SoftDevice") {
    const std::string text = rig.run("image list");
    CHECK(has(text, "base       the board's base,"));
    CHECK(has(text, "bootloader 0x000f4000"));
    CHECK(has(text, "S140 6.1.1"));
    CHECK(has(text, "bytes of flash, 2 UICR words"));
    CHECK(has(text, "the MBR's parameter page at 0x000fe000"));
    // And it is not read as an image: no role, board, band or version line for it.
    CHECK_FALSE(has(text, "base       repeater"));
  }
  SUBCASE("image verify base: the hash out of flash, and check 5p over the same bytes") {
    const std::string text = rig.run("image verify base");
    CHECK(has(text, "base: "));
    CHECK(has(text, "read back and hashed"));
    CHECK(has(text, "the SHA-256 is the one in its record"));
    CHECK(has(text, "base: check 5p over the stored bytes: a base for rak4630-single "
                    "(0x00026000 to 0x000ed000)"));
    CHECK(has(text, "runs, 2 UICR words - the same as its record says"));
    CHECK_FALSE(has(text, "WRONG"));
  }
  SUBCASE("image erase base: allowed, and the base is gone") {
    CHECK(has(rig.run("image erase base"), "base: forgotten in slot 0"));
    CHECK(has(rig.run("image list"), "base       empty"));
    CHECK(has(rig.run("image verify base"), "there is no image in that slot"));
  }
  SUBCASE("image receive base: the port is handed over, and the checks are named") {
    const std::string text = rig.run("image receive base");
    CHECK(has(text, "image receive: slot 0's base"));
    CHECK(has(text, "tools/walterctl.py push --as base"));
    CHECK(has(text, "checks 1 to 4 and 5p"));
    CHECK(has(text, "written to a radio only by `rak provision`"));
  }
  SUBCASE("status names the base beside the three images") {
    const std::string text = rig.run("status");
    CHECK(has(text, "images: golden=empty candidate=empty scanner=empty base=0x000f4000"));
  }
}

TEST_CASE("the parser's words name the base where it can be typed") {
  Rig rig;
  CHECK(has(rig.run("image verify"), "image verify needs one of golden, candidate, scanner or base"));
  CHECK(has(rig.run("image verify basis"),
            "image: not one of golden, candidate, scanner or base: basis"));
  CHECK(has(rig.run("image erase golden"),
            "image erase: the golden image is not erased (image erase candidate | scanner | base)"));
}

TEST_CASE("help names the five image commands") {
  Rig rig;
  const std::string text = rig.run("help");
  CHECK(has(text, "image list"));
  CHECK(has(text, "image verify <which>"));
  CHECK(has(text, "image erase <candidate|scanner|base>"));
  CHECK(has(text, "image bad"));
  CHECK(has(text, "image receive <which>"));
}
